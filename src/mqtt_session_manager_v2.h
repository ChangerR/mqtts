#ifndef MQTT_SESSION_MANAGER_V2_H
#define MQTT_SESSION_MANAGER_V2_H

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_session_allocator.h"
#include "mqtt_coroutine_utils.h"
#include "mqtt_message_queue.h"
#include "mqtt_parser.h"
#include "mqtt_send_worker_pool.h"
#include "mqtt_session_info.h"
#include "pthread_rwlock_wrapper.h"
#include "singleton.h"
#include "mqtt_send_worker_pool.h"
#include "mqtt_topic_tree.h"

namespace mqtt {

// 前向声明
class MQTTProtocolHandler;
struct SessionInfo;
class SafeHandlerRef;

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
   * @brief 将共享消息内容加入待发送队列（优化版本）
   */
  void enqueue_shared_message(const SharedMessageContentPtr& content,
                              const MQTTString& target_client_id);

  /**
   * @brief 批量将共享消息内容加入待发送队列（批量优化版本）
   */
  void enqueue_shared_messages(const SharedMessageContentPtr& content,
                               const std::vector<MQTTString>& target_client_ids);

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

  /**
   * @brief 配置Worker池
   * @param worker_count Worker数量
   * @param max_queue_size 每个Worker最大队列长度
   * @return 0成功，非0失败
   */
  int configure_worker_pool(size_t worker_count = 4, size_t max_queue_size = 1000);

  /**
   * @brief 获取Worker池统计信息
   */
  SendWorkerPool::Statistics get_worker_statistics() const;

  /**
   * @brief 获取当前线程管理器的内存使用情况
   * @param memory_usage 输出参数，内存使用量（字节）
   * @return 错误码，MQ_SUCCESS表示成功
   */
  int get_memory_usage(size_t& memory_usage) const;

  /**
   * @brief 检查内存使用是否超过限制
   * @param limit_exceeded 输出参数，true表示超过限制
   * @return 错误码，MQ_SUCCESS表示成功
   */
  int is_memory_limit_exceeded(bool& limit_exceeded) const;

  /**
   * @brief 设置内存限制
   * @param limit 内存限制（字节），0表示无限制
   * @return 错误码，MQ_SUCCESS表示成功
   */
  int set_memory_limit(size_t limit);

  /**
   * @brief 获取内存分配器统计信息
   * @param stats 输出参数，包含各种分配器统计信息的结构
   * @return 错误码，MQ_SUCCESS表示成功
   */
  struct MemoryStats {
    size_t total_usage;
    size_t session_usage;
    size_t queue_usage;
    size_t worker_usage;
    size_t limit;
    bool limit_exceeded;
  };
  int get_memory_statistics(MemoryStats& stats) const;

 private:
  std::thread::id thread_id_;
  mutable CoroMutex sessions_mutex_;
  
  // 使用Session分配器的容器
  MQTTAllocator* session_allocator_;
  MQTTAllocator* queue_allocator_;
  MQTTAllocator* worker_allocator_;
  
  SessionUnorderedMap<std::string, std::unique_ptr<SessionInfo>> sessions_;

  // 消息队列需要支持多线程访问（其他线程会向此队列写入消息）
  mutable std::mutex queue_mutex_;
  SessionQueue<PendingMessage> pending_messages_;

  CoroCondition new_message_cond_;
  std::atomic<bool> has_new_messages_;

  // 发送Worker池
  std::unique_ptr<SendWorkerPool> worker_pool_;

  bool is_handler_valid(MQTTProtocolHandler* handler) const;
  void safe_remove_session(const std::string& client_id, SessionInfo* info);
  int internal_process_messages(int max_process_count);
  
  // 内存管理相关方法
  void initialize_allocators();
  void cleanup_allocators();
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
   * @brief 转发PUBLISH消息到指定客户端（使用共享内容，内存优化版本）
   */
  int forward_publish_shared(const MQTTString& target_client_id,
                             const SharedMessageContentPtr& content);

  /**
   * @brief 转发PUBLISH消息到订阅指定主题的所有客户端
   */
  int forward_publish_by_topic(const MQTTString& topic, const PublishPacket& packet,
                               const MQTTString& sender_client_id);

  /**
   * @brief 转发PUBLISH消息到订阅指定主题的所有客户端（使用共享内容，内存优化版本）
   */
  int forward_publish_by_topic_shared(const MQTTString& topic,
                                      const SharedMessageContentPtr& content);

  /**
   * @brief 获取或创建共享消息内容
   */
  SharedMessageContentPtr get_or_create_shared_content(const PublishPacket& packet,
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
   * @brief 获取订阅了指定主题的客户端列表（已弃用，使用主题匹配树代替）
   */
  std::vector<MQTTString> get_subscribers(const MQTTString& topic) const;

  /**
   * @brief 客户端订阅主题
   * @param topic_filter 主题过滤器（支持通配符）
   * @param client_id 客户端ID
   * @param qos QoS级别
   * @return 0成功，非0失败
   */
  int subscribe_topic(const MQTTString& topic_filter, const MQTTString& client_id, uint8_t qos = 0);

  /**
   * @brief 客户端取消订阅主题
   * @param topic_filter 主题过滤器
   * @param client_id 客户端ID
   * @return 0成功，非0失败
   */
  int unsubscribe_topic(const MQTTString& topic_filter, const MQTTString& client_id);

  /**
   * @brief 取消客户端的所有订阅
   * @param client_id 客户端ID
   * @return 取消的订阅数量
   */
  int unsubscribe_all_topics(const MQTTString& client_id);

  /**
   * @brief 获取客户端的所有订阅
   * @param client_id 客户端ID
   * @return 订阅的主题过滤器列表
   */
  std::vector<MQTTString> get_client_subscriptions(const MQTTString& client_id) const;

  /**
   * @brief 使用高性能主题匹配树查找订阅者
   * @param topic 发布主题
   * @return 匹配的订阅者列表
   */
  std::vector<SubscriberInfo> find_topic_subscribers(const MQTTString& topic) const;

  /**
   * @brief 获取主题匹配树统计信息
   * @return 包含订阅者数量和节点数量的pair
   */
  std::pair<size_t, size_t> get_topic_tree_stats() const;

  /**
   * @brief 清理主题匹配树中的空节点
   * @return 清理的节点数量
   */
  size_t cleanup_topic_tree() const;

  /**
   * @brief 清理所有无效的会话记录
   */
  int cleanup_all_invalid_sessions();

  /**
   * @brief 清理过期的消息内容缓存
   */
  void cleanup_message_cache(int max_age_seconds = 300);

  /**
   * @brief 获取消息缓存统计信息
   */
  size_t get_message_cache_size() const;

  /**
   * @brief 获取全局内存使用统计
   * @param memory_usage 输出参数，内存使用量（字节）
   * @return 错误码，MQ_SUCCESS表示成功
   */
  int get_global_memory_usage(size_t& memory_usage) const;

  /**
   * @brief 获取所有客户端的内存使用情况
   * @param client_usage 输出参数，客户端ID到内存使用量的映射
   * @return 错误码，MQ_SUCCESS表示成功
   */
  int get_client_memory_usage(std::unordered_map<std::string, size_t>& client_usage) const;

  /**
   * @brief 设置客户端内存限制
   * @param client_id 客户端ID
   * @param limit 内存限制（字节），0表示无限制
   * @return 错误码，MQ_SUCCESS表示成功
   */
  int set_client_memory_limit(const MQTTString& client_id, size_t limit);

  /**
   * @brief 检查客户端内存使用是否超过限制
   * @param client_id 客户端ID
   * @param limit_exceeded 输出参数，true表示超过限制
   * @return 错误码，MQ_SUCCESS表示成功
   */
  int is_client_memory_limit_exceeded(const MQTTString& client_id, bool& limit_exceeded);

  /**
   * @brief 获取内存分配器层次结构信息
   * @param hierarchy 输出参数，分配器层次结构的字符串表示
   * @return 错误码，MQ_SUCCESS表示成功
   */
  int get_allocator_hierarchy(std::string& hierarchy) const;

  /**
   * @brief 清理所有过期的内存分配器
   * @param cleaned_count 输出参数，清理的分配器数量
   * @return 错误码，MQ_SUCCESS表示成功
   */
  int cleanup_expired_allocators(int& cleaned_count);

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

  // 消息内容缓存管理器
  std::unique_ptr<MessageContentCache> message_cache_;

  // 高性能主题匹配树
  std::unique_ptr<ConcurrentTopicTree> topic_tree_;

  // 线程本地缓存（避免重复查找）
  thread_local static ThreadLocalSessionManager* cached_thread_manager_;

  // 全局内存管理
  MQTTAllocator* global_allocator_;
  std::unordered_map<std::string, size_t> client_memory_limits_;
  mutable std::mutex memory_limits_mutex_;

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

  /**
   * @brief 批量转发消息到多个客户端（内部优化方法）
   */
  int batch_forward_publish(const std::vector<MQTTString>& target_client_ids,
                            const SharedMessageContentPtr& content);
};

// 单例访问
typedef Singleton<GlobalSessionManager> GlobalSessionManagerInstance;

}  // namespace mqtt

#endif  // MQTT_SESSION_MANAGER_V2_H