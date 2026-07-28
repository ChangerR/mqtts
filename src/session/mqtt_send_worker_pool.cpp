#include "mqtt_send_worker_pool.h"
#include <algorithm>
#include "logger.h"
#include "mqtt_protocol_handler.h"
#include "mqtt_session_info.h"
#include "mqtt_session_manager_v2.h"

namespace mqtt {

SendWorkerPool::SendWorkerPool(size_t worker_count, size_t max_queue_size)
    : worker_count_(worker_count),
      max_queue_size_(max_queue_size),
      running_(false),
      should_stop_(false),
      session_manager_(nullptr)
{
  workers_.reserve(worker_count_);
  for (size_t i = 0; i < worker_count_; ++i) {
    workers_.emplace_back(std::unique_ptr<WorkerData>(new WorkerData()));
  }

  LOG_INFO("SendWorkerPool created with {} workers, max queue size: {}", worker_count_,
           max_queue_size_);
}

SendWorkerPool::~SendWorkerPool()
{
  stop();
  LOG_INFO("SendWorkerPool destroyed");
}

int SendWorkerPool::start()
{
  if (running_.load()) {
    LOG_WARN("SendWorkerPool already running");
    return MQ_SUCCESS;
  }

  should_stop_.store(false);
  owner_thread_ = std::this_thread::get_id();

  // 创建Worker协程任务
  for (size_t i = 0; i < worker_count_; ++i) {
    // 创建Worker上下文结构
    struct WorkerContext
    {
      SendWorkerPool* pool;
      size_t worker_id;
    };

    WorkerContext* ctx = new WorkerContext{this, i};

    workers_[i]->worker_task = runtime::current_runtime().spawn(
        [](void* arg) -> void* {
          WorkerContext* ctx = static_cast<WorkerContext*>(arg);
          ctx->pool->worker_main(ctx->worker_id);
          delete ctx;
          return nullptr;
        },
        ctx, 128 * 1024);

    if (!workers_[i]->worker_task.is_valid()) {
      LOG_ERROR("Failed to create worker task {}", i);
      delete ctx;
      // spawn() already started the previous workers, so they have to be shut
      // down explicitly: running_ is still false and stop() cannot do it.
      shutdown_workers();
      return MQ_ERR_MEMORY_ALLOC;
    }
  }

  running_.store(true);

  LOG_INFO("SendWorkerPool started with {} workers", worker_count_);
  return MQ_SUCCESS;
}

void SendWorkerPool::shutdown_workers()
{
  should_stop_.store(true);

  // 协程条件变量和协程栈都只能由创建它们的线程操作：从其他线程唤醒会把Worker协程
  // 调度到错误的线程上，释放协程栈则会与归属线程竞争。因此跨线程停止时只设置停止
  // 标志并放弃句柄，由归属线程的事件循环收尾。
  const bool on_owner_thread = (owner_thread_ == std::this_thread::get_id());
  if (!on_owner_thread) {
    for (size_t i = 0; i < worker_count_; ++i) {
      workers_[i]->worker_task.release();
    }
    LOG_WARN("SendWorkerPool stopped from a foreign thread; worker tasks were detached");
    return;
  }

  // 通知所有Worker停止
  for (size_t i = 0; i < worker_count_; ++i) {
    workers_[i]->task_available.broadcast();
  }

  // 等待所有Worker协程结束
  for (size_t i = 0; i < worker_count_; ++i) {
    if (workers_[i]->worker_task.is_valid() && workers_[i]->worker_task.join(1000) != 0) {
      LOG_WARN("Worker {} did not stop before timeout; detaching task", i);
    }
    workers_[i]->worker_task.release();
  }
}

void SendWorkerPool::stop()
{
  bool has_worker_tasks = false;
  for (size_t i = 0; i < worker_count_; ++i) {
    if (workers_[i]->worker_task.is_valid()) {
      has_worker_tasks = true;
      break;
    }
  }

  if (!running_.load() && !has_worker_tasks) {
    return;
  }

  shutdown_workers();

  // 清空所有队列
  for (size_t i = 0; i < worker_count_; ++i) {
    CoroLockGuard lock(&workers_[i]->queue_mutex);  // 使用协程锁
    while (!workers_[i]->task_queue.empty()) {
      workers_[i]->task_queue.pop();
    }
  }

  running_.store(false);
  LOG_INFO("SendWorkerPool stopped");
}

int SendWorkerPool::submit_task(const WorkerSendTask& task)
{
  if (!running_.load()) {
    return MQ_ERR_INVALID_ARGS;
  }

  size_t worker_id = select_worker();
  WorkerData* worker = workers_[worker_id].get();

  {
    CoroLockGuard lock(&worker->queue_mutex);  // 使用协程锁

    if (worker->task_queue.size() >= max_queue_size_) {
      LOG_WARN("Worker {} queue full, dropping task for client: {}", worker_id,
               from_mqtt_string(task.target_client_id));
      return MQ_ERR_TIMEOUT_V2;
    }

    worker->task_queue.push(task);
  }

  // 通知Worker有新任务
  worker->task_available.signal();
  total_submitted_.fetch_add(1);

  return MQ_SUCCESS;
}

SendWorkerPool::Statistics SendWorkerPool::get_statistics() const
{
  Statistics stats = {};
  stats.total_submitted = total_submitted_.load();
  stats.avg_processing_time_ms = avg_processing_time_ms_.load();

  for (size_t i = 0; i < worker_count_; ++i) {
    stats.total_processed += workers_[i]->processed_count.load();
    stats.total_failed += workers_[i]->failed_count.load();

    CoroLockGuard lock(&workers_[i]->queue_mutex);  // 使用协程锁
    stats.pending_tasks += workers_[i]->task_queue.size();
  }

  return stats;
}

void SendWorkerPool::worker_main(size_t worker_id)
{
  LOG_INFO("Worker {} started", worker_id);

  WorkerData* worker = workers_[worker_id].get();

  while (!should_stop_.load()) {
    WorkerSendTask task;
    bool has_task = false;

    // 从队列中获取任务
    {
      CoroLockGuard lock(&worker->queue_mutex);  // 使用协程锁
      if (!worker->task_queue.empty()) {
        task = worker->task_queue.front();
        worker->task_queue.pop();
        has_task = true;
      }
    }

    if (has_task) {
      // 处理任务
      auto start_time = std::chrono::steady_clock::now();
      bool success = process_send_task(task, worker_id);
      auto end_time = std::chrono::steady_clock::now();

      // 更新统计信息
      if (success) {
        worker->processed_count.fetch_add(1);
      } else {
        worker->failed_count.fetch_add(1);
      }

      // 更新平均处理时间
      auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
      double processing_time_ms = duration.count() / 1000.0;
      {
        CoroLockGuard stats_lock(&stats_mutex_);  // 使用协程锁
        double current_avg = avg_processing_time_ms_.load();
        double new_avg = (current_avg * 0.9) + (processing_time_ms * 0.1);  // 指数移动平均
        avg_processing_time_ms_.store(new_avg);
      }
    } else {
      // 没有任务，等待新任务到达
      worker->task_available.wait(100);  // 100ms超时
    }
  }

  LOG_INFO("Worker {} stopped", worker_id);
}

bool SendWorkerPool::process_send_task(const WorkerSendTask& task, size_t worker_id)
{
  if (!session_manager_) {
    LOG_ERROR("Worker {}: session manager not available", worker_id);
    return false;
  }

  try {
    SafeHandlerRef safe_handler = session_manager_->get_safe_handler(task.get_target_client_id());
    if (!safe_handler.is_valid()) {
      LOG_WARN("Worker {}: handler not found for client: {}", worker_id,
               from_mqtt_string(task.get_target_client_id()));
      return false;
    }

    // 计算任务在队列中的等待时间
    auto now = std::chrono::steady_clock::now();
    auto queue_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - task.enqueue_time);

    if (queue_time.count() > 5000) {  // 5秒超时
      LOG_WARN("Worker {}: task expired (waited {}ms) for client: {}", worker_id,
               queue_time.count(), from_mqtt_string(task.get_target_client_id()));
      return false;
    }

    // 检查任务是否有效
    if (!task.is_valid()) {
      LOG_ERROR("Worker {}: invalid task for client: {}", worker_id,
                from_mqtt_string(task.target_client_id));
      return false;
    }

    // 实际发送PUBLISH消息（使用共享内容）
    MQTTProtocolHandler* handler = safe_handler.get();
    int result = handler->send_publish(task.get_topic(), task.get_payload(), task.get_qos(),
                                       task.is_retain(), task.is_dup(), task.get_properties());

    if (result == MQ_SUCCESS) {
      LOG_DEBUG(
          "Worker {}: successfully sent shared message to client: {} (topic: {}, queue time: {}ms)",
          worker_id, from_mqtt_string(task.get_target_client_id()),
          from_mqtt_string(task.get_topic()), queue_time.count());
      return true;
    } else {
      LOG_ERROR(
          "Worker {}: failed to send shared message to client: {}, error: {} (topic: {}, queue "
          "time: {}ms)",
          worker_id, from_mqtt_string(task.get_target_client_id()), result,
          from_mqtt_string(task.get_topic()), queue_time.count());
      return false;
    }

  } catch (const std::exception& e) {
    LOG_ERROR("Worker {}: exception processing task for client {}: {}", worker_id,
              from_mqtt_string(task.get_target_client_id()), e.what());
    return false;
  }
}

size_t SendWorkerPool::select_worker() const
{
  size_t min_queue_size = SIZE_MAX;
  size_t selected_worker = 0;

  // 选择队列长度最短的Worker
  for (size_t i = 0; i < worker_count_; ++i) {
    CoroLockGuard lock(&workers_[i]->queue_mutex);  // 使用协程锁
    size_t queue_size = workers_[i]->task_queue.size();

    if (queue_size < min_queue_size) {
      min_queue_size = queue_size;
      selected_worker = i;
    }
  }

  return selected_worker;
}

}  // namespace mqtt
