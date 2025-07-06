#include "mqtt_send_worker_pool.h"
#include <algorithm>
#include "logger.h"
#include "mqtt_protocol_handler.h"
#include "mqtt_session_info.h"
#include "mqtt_session_manager_v2.h"

namespace mqtt {

// 新增错误代码定义
#define MQ_ERR_TIMEOUT_V2 -802

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

  // 创建Worker协程
  for (size_t i = 0; i < worker_count_; ++i) {
    stCoRoutineAttr_t attr;
    attr.stack_size = 128 * 1024;  // 128KB栈空间

    // 创建Worker上下文结构
    struct WorkerContext
    {
      SendWorkerPool* pool;
      size_t worker_id;
    };

    WorkerContext* ctx = new WorkerContext{this, i};

    int ret = co_create(
        &workers_[i]->worker_coroutine, &attr,
        [](void* arg) -> void* {
          WorkerContext* ctx = static_cast<WorkerContext*>(arg);
          ctx->pool->worker_main(ctx->worker_id);
          delete ctx;
          return nullptr;
        },
        ctx);

    if (ret != 0) {
      LOG_ERROR("Failed to create worker coroutine {}: {}", i, ret);
      delete ctx;
      stop();
      return MQ_ERR_MEMORY_ALLOC;
    }
  }

  running_.store(true);

  // 启动所有Worker协程
  for (size_t i = 0; i < worker_count_; ++i) {
    co_resume(workers_[i]->worker_coroutine);
  }

  LOG_INFO("SendWorkerPool started with {} workers", worker_count_);
  return MQ_SUCCESS;
}

void SendWorkerPool::stop()
{
  if (!running_.load()) {
    return;
  }

  should_stop_.store(true);

  // 通知所有Worker停止
  for (size_t i = 0; i < worker_count_; ++i) {
    workers_[i]->task_available.broadcast();
  }

  // 等待所有Worker协程结束
  for (size_t i = 0; i < worker_count_; ++i) {
    if (workers_[i]->worker_coroutine) {
      co_release(workers_[i]->worker_coroutine);
      workers_[i]->worker_coroutine = nullptr;
    }
  }

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