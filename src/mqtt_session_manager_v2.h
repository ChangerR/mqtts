#ifndef MQTT_SESSION_MANAGER_V2_H
#define MQTT_SESSION_MANAGER_V2_H

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_coroutine_utils.h"
#include "mqtt_message_queue.h"
#include "mqtt_parser.h"
#include "mqtt_session_info.h"
#include "pthread_rwlock_wrapper.h"
#include "singleton.h"

namespace mqtt {

// 前向声明
class MQTTProtocolHandler;

/**
 * @brief 线程本地会话管理器，管理单个线程下的所有Handler
 */
class ThreadLocalSessionManager
{
 public:
  explicit ThreadLocalSessionManager(std::thread::id thread_id);
  ~ThreadLocalSessionManager();

  // 禁止拷贝和赋值
  ThreadLocalSessionManager(const ThreadLocalSessionManager&) = delete;
  ThreadLocalSessionManager& operator=(const ThreadLocalSessionManager&) = delete;

  /**
   * @brief 注册Handler到当前线程的SessionManager
   */
  int register_handler(const MQTTString& client_id, MQTTProtocolHandler* handler);

  /**
   * @brief 注销Handler
   */
  int unregister_handler(const MQTTString& client_id);

  /**
   * @brief 获取安全的Handler引用（线程安全，自动管理引用计数）
   */
  SafeHandlerRef get_safe_handler(const MQTTString& client_id);

  /**
   * @brief 将消息加入待发送队列（协程友好）
   */
  void enqueue_message(const PendingMessage& message);

  /**
   * @brief 处理待发送队列中的消息（协程友好的阻塞等待）
   */
  int process_pending_messages(int max_process_count = 0, int timeout_ms = -1);

  /**
   * @brief 非阻塞处理待发送队列中的消息
   */
  int process_pending_messages_nowait(int max_process_count = 0);

  /**
   * @brief 等待新消息到达（协程友好的阻塞等待）
   */
  bool wait_for_new_message(int timeout_ms = -1);

  /**
   * @brief 获取当前线程ID
   */
  std::thread::id get_thread_id() const { return thread_id_; }

  /**
   * @brief 获取管理的Handler数量
   */
  size_t get_handler_count() const;

  /**
   * @brief 获取待发送队列长度
   */
  size_t get_pending_message_count() const;

  /**
   * @brief 获取所有客户端ID列表
   */
  std::vector<MQTTString> get_all_client_ids() const;

  /**
   * @brief 清理无效的Handler
   */
  int cleanup_invalid_handlers();

 private:
  std::thread::id thread_id_;
  mutable CoroMutex sessions_mutex_;
  std::unordered_map<std::string, std::unique_ptr<SessionInfo>> sessions_;

  // 消息队列需要支持多线程访问（其他线程会向此队列写入消息）
  mutable std::mutex queue_mutex_;
  std::queue<PendingMessage> pending_messages_;

  CoroCondition new_message_cond_;
  std::atomic<bool> has_new_messages_;

  bool is_handler_valid(MQTTProtocolHandler* handler) const;
  void safe_remove_session(const std::string& client_id, SessionInfo* info);
  int internal_process_messages(int max_process_count);
};

/**
 * @brief 全局会话管理器，管理所有线程的ThreadLocalSessionManager
 * 优化设计：程序启动时确定线程数，运行时无锁冲突
 */
class GlobalSessionManager
{
 public:
  GlobalSessionManager();
  ~GlobalSessionManager();

  // 禁止拷贝和赋值
  GlobalSessionManager(const GlobalSessionManager&) = delete;
  GlobalSessionManager& operator=(const GlobalSessionManager&) = delete;

  /**
   * @brief 预注册线程管理器（启动阶段调用）
   * @param thread_count 预期的线程数量
   * @param reserve_client_count 预期的客户端数量（用于预分配）
   */
  void pre_register_threads(size_t thread_count, size_t reserve_client_count = 10000);

  /**
   * @brief 注册线程管理器（每个工作线程启动时调用一次）
   * @param thread_id 线程ID
   * @return 线程管理器指针
   */
  ThreadLocalSessionManager* register_thread_manager(std::thread::id thread_id);

  /**
   * @brief 完成线程注册，切换到运行模式（所有线程注册完毕后调用）
   */
  void finalize_thread_registration();

  /**
   * @brief 获取当前线程的SessionManager（运行时调用，无锁）
   * @return 当前线程的SessionManager指针
   */
  ThreadLocalSessionManager* get_thread_manager();

  /**
   * @brief 注册会话（运行时高频调用，最小化锁冲突）
   */
  int register_session(const MQTTString& client_id, MQTTProtocolHandler* handler);

  /**
   * @brief 注销会话（运行时调用，最小化锁冲突）
   */
  int unregister_session(const MQTTString& client_id);

  /**
   * @brief 快速查找客户端所在的线程管理器（无锁读取）
   */
  ThreadLocalSessionManager* find_client_manager(const MQTTString& client_id);

  /**
   * @brief 转发PUBLISH消息到指定客户端（高频调用，无锁）
   */
  int forward_publish(const MQTTString& target_client_id, const PublishPacket& packet,
                      const MQTTString& sender_client_id);

  /**
   * @brief 转发PUBLISH消息到订阅指定主题的所有客户端
   */
  int forward_publish_by_topic(const MQTTString& topic, const PublishPacket& packet,
                               const MQTTString& sender_client_id);

  /**
   * @brief 获取所有活跃会话的数量
   */
  size_t get_total_session_count() const;

  /**
   * @brief 获取所有客户端ID列表
   */
  std::vector<MQTTString> get_all_client_ids() const;

  /**
   * @brief 检查主题是否匹配主题过滤器（支持通配符）
   */
  bool is_topic_match(const MQTTString& topic, const MQTTString& topic_filter) const;

  /**
   * @brief 获取订阅了指定主题的客户端列表
   */
  std::vector<MQTTString> get_subscribers(const MQTTString& topic) const;

  /**
   * @brief 清理所有无效的会话记录
   */
  int cleanup_all_invalid_sessions();

 private:
  // 运行状态
  enum class ManagerState {
    INITIALIZING,  // 初始化阶段
    REGISTERING,   // 线程注册阶段
    RUNNING        // 运行阶段（无锁优化）
  };

  std::atomic<ManagerState> state_;

  // 线程管理器存储（启动后不再变更，支持无锁访问）
  mutable RWMutex managers_mutex_;  // 读写锁，读多写少
  std::unordered_map<std::thread::id, std::unique_ptr<ThreadLocalSessionManager>> thread_managers_;
  std::vector<ThreadLocalSessionManager*> thread_manager_array_;  // 预分配数组，支持快速遍历

  // 客户端索引（高频读写，使用读写锁优化）
  mutable RWMutex client_index_mutex_;  // 读写锁
  std::unordered_map<std::string, ThreadLocalSessionManager*>
      client_to_manager_;  // 直接存储管理器指针

  // 线程本地缓存（避免重复查找）
  thread_local static ThreadLocalSessionManager* cached_thread_manager_;

  /**
   * @brief 内部快速查找客户端管理器（无锁版本）
   */
  ThreadLocalSessionManager* fast_find_client_manager(const std::string& client_id_str) const;

  /**
   * @brief 内部辅助函数：检查通配符匹配
   */
  bool match_topic_filter(const MQTTString& topic, const MQTTString& filter) const;

  /**
   * @brief 更新客户端索引（写操作，使用写锁）
   */
  void update_client_index(const std::string& client_id, ThreadLocalSessionManager* manager);

  /**
   * @brief 移除客户端索引（写操作，使用写锁）
   */
  void remove_client_index(const std::string& client_id);
};

// 单例访问
typedef Singleton<GlobalSessionManager> GlobalSessionManagerInstance;

}  // namespace mqtt

#endif  // MQTT_SESSION_MANAGER_V2_H