#ifndef MQTT_SEND_WORKER_POOL_H
#define MQTT_SEND_WORKER_POOL_H

#include <atomic>
#include <chrono>
#include <memory>
#include <queue>
#include <vector>

#include "mqtt_coroutine_utils.h"
#include "mqtt_define.h"
#include "mqtt_packet.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {

// 前向声明
class ThreadLocalSessionManager;

/**
 * @brief Worker发送任务结构
 */
struct WorkerSendTask
{
  PublishPacket packet;
  MQTTString target_client_id;
  MQTTString sender_client_id;
  std::chrono::steady_clock::time_point enqueue_time;

  WorkerSendTask() : enqueue_time(std::chrono::steady_clock::now()) {}

  WorkerSendTask(const PublishPacket& p, const MQTTString& target, const MQTTString& sender)
      : packet(p),
        target_client_id(target),
        sender_client_id(sender),
        enqueue_time(std::chrono::steady_clock::now())
  {
  }
};

/**
 * @brief 协程友好的发送Worker池
 *
 * 专门负责执行实际的消息发送操作，避免在主事件循环中阻塞
 * 使用协程锁和协程同步原语，完全协程友好
 */
class SendWorkerPool
{
 public:
  /**
   * @brief 构造函数
   * @param worker_count Worker协程数量
   * @param max_queue_size 每个Worker的最大队列长度
   */
  explicit SendWorkerPool(size_t worker_count = 4, size_t max_queue_size = 1000);

  /**
   * @brief 析构函数
   */
  ~SendWorkerPool();

  /**
   * @brief 启动Worker池
   * @return 0成功，非0失败
   */
  int start();

  /**
   * @brief 停止Worker池
   */
  void stop();

  /**
   * @brief 提交发送任务
   * @param task 发送任务
   * @return 0成功，非0失败
   */
  int submit_task(const WorkerSendTask& task);

  /**
   * @brief 获取统计信息
   */
  struct Statistics
  {
    size_t total_submitted;
    size_t total_processed;
    size_t total_failed;
    size_t pending_tasks;
    double avg_processing_time_ms;
  };

  Statistics get_statistics() const;

  /**
   * @brief 检查是否运行中
   */
  bool is_running() const { return running_.load(); }

  /**
   * @brief 设置会话管理器引用
   * @param session_manager 会话管理器指针
   */
  void set_session_manager(ThreadLocalSessionManager* session_manager)
  {
    session_manager_ = session_manager;
  }

 private:
  struct WorkerData
  {
    std::queue<WorkerSendTask> task_queue;
    mutable CoroMutex queue_mutex;  // 使用协程锁
    CoroCondition task_available;
    std::atomic<size_t> processed_count{0};
    std::atomic<size_t> failed_count{0};
    stCoRoutine_t* worker_coroutine = nullptr;
  };

  /**
   * @brief Worker协程主函数
   * @param worker_id Worker ID
   */
  void worker_main(size_t worker_id);

  /**
   * @brief 处理单个发送任务
   * @param task 任务
   * @param worker_id Worker ID
   * @return true成功，false失败
   */
  bool process_send_task(const WorkerSendTask& task, size_t worker_id);

  /**
   * @brief 选择负载最少的Worker
   * @return Worker索引
   */
  size_t select_worker() const;

 private:
  size_t worker_count_;
  size_t max_queue_size_;
  std::atomic<bool> running_;
  std::atomic<bool> should_stop_;

  std::vector<std::unique_ptr<WorkerData>> workers_;

  // 统计信息
  std::atomic<size_t> total_submitted_{0};
  mutable CoroMutex stats_mutex_;  // 使用协程锁
  std::atomic<double> avg_processing_time_ms_{0.0};

  // 全局会话管理器引用（用于查找handler）
  ThreadLocalSessionManager* session_manager_;
};

}  // namespace mqtt

#endif  // MQTT_SEND_WORKER_POOL_H