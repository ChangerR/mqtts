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
#include "mqtt_coroutine_utils.h"
#include "mqtt_message_queue.h"
#include "mqtt_takeover.h"
#include "mqtt_protocol_counters.h"
#include "mqtt_parser.h"
#include "mqtt_send_worker_pool.h"
#include "mqtt_session_info.h"
#include "mqtt_durable_store.h"
#include "mqtt_stl_allocator.h"
#include "mqtt_topic_tree.h"
#include "mqtt_config.h"
#include "mqtt_will.h"
#include "pthread_rwlock_wrapper.h"
#include "singleton.h"
#include "mqtt_event_forwarding_service.h"
#include "forwarding_connection_pool.h"
#include "mqtt_forwarding_rpc_client.h"
#include "mqtt_forwarding_service.h"
#include "mqtt_router_rpc_client.h"

namespace mqtt {

// 前向声明
class MQTTProtocolHandler;
struct SessionInfo;
class SafeHandlerRef;
class RetainStore;

/**
 * @brief 线程本地会话管理器，管理单个线程下的所有Handler
 */
class ThreadLocalSessionManager
{
 public:
  explicit ThreadLocalSessionManager(std::thread::id thread_id, MQTTAllocator* allocator = nullptr);
  ~ThreadLocalSessionManager();

  // 禁止拷贝和赋值
  ThreadLocalSessionManager(const ThreadLocalSessionManager&) = delete;
  ThreadLocalSessionManager& operator=(const ThreadLocalSessionManager&) = delete;

  /**
   * @brief 初始化线程本地会话管理器
   * @return MQ_SUCCESS成功，其他值失败
   */
  int init();

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
   * @param message 待发送消息
   * @return MQ_SUCCESS成功，其他值失败
   */
  int enqueue_message(const PendingMessage& message);

  /**
   * @brief 将共享消息内容加入待发送队列（优化版本）
   * @param content 共享消息内容
   * @param target_client_id 目标客户端ID
   * @return MQ_SUCCESS成功，其他值失败
   */
  int enqueue_shared_message(const SharedMessageContentPtr& content,
                             const MQTTString& target_client_id);

  /**
   * @brief 批量将共享消息内容加入待发送队列（批量优化版本）
   * @param content 共享消息内容
   * @param target_client_ids 目标客户端ID列表
   * @return MQ_SUCCESS成功，其他值失败
   */
  int enqueue_shared_messages(const SharedMessageContentPtr& content,
                              const std::vector<MQTTString>& target_client_ids);

  /**
   * @brief 处理待发送队列中的消息（协程友好的阻塞等待）
   */
  int process_pending_messages(int max_process_count = 0, int timeout_ms = -1);

  /**
   * @brief 非阻塞处理待发送队列中的消息
   */
  int process_pending_messages_nowait(int max_process_count = 0);

  // 跨线程会话接管。只入队，由本事件线程的事件循环回调执行。
  int enqueue_takeover(const std::string& client_id, MQTTProtocolHandler* newcomer,
                       const std::shared_ptr<SessionTakeoverGate>& gate);
  int process_control_messages();

  /**
   * @brief 等待新消息到达（协程友好的阻塞等待）
   * @param timeout_ms 超时时间（毫秒），-1表示无限等待
   * @param has_message 输出参数：是否有新消息
   * @return MQ_SUCCESS成功，其他值失败
   */
  int wait_for_new_message(int timeout_ms, bool& has_message);

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
   * @brief 设置等待引用归零后再移除/替换 session 的超时（毫秒）
   *
   * 生产默认 3000ms。超时后不擦除仍被 SafeHandlerRef 持有的 SessionInfo。
   */
  void set_session_removal_timeout_ms(int timeout_ms)
  {
    session_removal_timeout_ms_ = timeout_ms < 0 ? 0 : timeout_ms;
  }

  int get_session_removal_timeout_ms() const { return session_removal_timeout_ms_; }

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
   * @brief 获取分配器
   */
  MQTTAllocator* get_allocator() const { return allocator_; }

  // 只允许本事件线程写入。
  ProtocolCounters& protocol_counters() { return protocol_counters_; }
  const ProtocolCounters& protocol_counters() const { return protocol_counters_; }

 private:
  std::thread::id thread_id_;
  MQTTAllocator* allocator_;
  bool initialized_;
  int session_removal_timeout_ms_;
  mutable CoroMutex sessions_mutex_;
  std::unordered_map<std::string, std::unique_ptr<SessionInfo>> sessions_;

  // 消息队列需要支持多线程访问（其他线程会向此队列写入消息）
  mutable std::mutex queue_mutex_;
  std::queue<PendingMessage> pending_messages_;
  std::queue<SessionTakeoverRequest> takeover_requests_;

  CoroCondition new_message_cond_;
  std::atomic<bool> has_new_messages_;

  // 发送Worker池
  std::unique_ptr<SendWorkerPool> worker_pool_;
  ProtocolCounters protocol_counters_;

  bool is_handler_valid(MQTTProtocolHandler* handler) const;
  int safe_remove_session(const std::string& client_id, SessionInfo* info);
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
  void configure_persistence(const PersistenceConfig& config) {
    if (config.enabled) durable_store_.reset(new DurableStore(config));
  }
  // 启动时冻结。true 时恢复“总是不把消息投回发布者”的旧行为。
  void set_implicit_no_local(bool enabled) { implicit_no_local_ = enabled; }
  bool implicit_no_local() const { return implicit_no_local_; }
  void set_retain_config(const RetainConfig& config);
  DurableStore* durable_store() const { return durable_store_.get(); }
  WillScheduler& will_scheduler() { return will_scheduler_; }
  const WillScheduler& will_scheduler() const { return will_scheduler_; }
  // 在事件循环回调里调用。flush_all 为真时发布本线程尚未到期的遗嘱。
  void process_due_wills(auth::AuthManager* auth, bool flush_all);
  ~GlobalSessionManager();

  // 禁止拷贝和赋值
  GlobalSessionManager(const GlobalSessionManager&) = delete;
  GlobalSessionManager& operator=(const GlobalSessionManager&) = delete;

  /**
   * @brief 预注册线程管理器（启动阶段调用）
   * @param thread_count 预期的线程数量
   * @param reserve_client_count 预期的客户端数量（用于预分配）
   * @return MQ_SUCCESS成功，其他值失败
   */
  int pre_register_threads(size_t thread_count, size_t reserve_client_count = 10000);

  /**
   * @brief 注册线程管理器（每个工作线程启动时调用一次）
   * @param thread_id 线程ID
   * @return 线程管理器指针
   */
  ThreadLocalSessionManager* register_thread_manager(std::thread::id thread_id);

  /**
   * @brief 完成线程注册，切换到运行模式（所有线程注册完毕后调用）
   * @return MQ_SUCCESS成功，其他值失败
   */
  int finalize_thread_registration();

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
  int subscribe_topic(const MQTTString& topic_filter, const MQTTString& client_id,
                      const SubscriptionOptions& options);

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
   * @param subscriptions 输出参数：订阅的主题过滤器列表
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_client_subscriptions(const MQTTString& client_id,
                               std::vector<MQTTString>& subscriptions) const;

  /**
   * @brief 使用高性能主题匹配树查找订阅者
   * @param topic 发布主题
   * @param subscribers 输出参数：匹配的订阅者列表
   * @return MQ_SUCCESS成功，其他值失败
   */
  int find_topic_subscribers(const MQTTString& topic,
                             std::vector<SubscriberInfo>& subscribers) const;

  /**
   * @brief 获取主题匹配树统计信息
   * @param subscriber_count 输出参数：订阅者数量
   * @param node_count 输出参数：节点数量
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_topic_tree_stats(size_t& subscriber_count, size_t& node_count) const;

  /**
   * @brief 清理主题匹配树中的空节点
   * @param cleaned_count 输出参数：清理的节点数量
   * @return MQ_SUCCESS成功，其他值失败
   */
  int cleanup_topic_tree(size_t& cleaned_count) const;

  /**
   * @brief 获取全局分配器
   */
  MQTTAllocator* get_allocator() const { return global_allocator_; }

  /**
   * @brief 清理所有无效的会话记录
   */
  int cleanup_all_invalid_sessions();

  /**
   * @brief 清理过期的消息内容缓存
   * @param max_age_seconds 最大年龄（秒）
   * @return MQ_SUCCESS成功，其他值失败
   */
  int cleanup_message_cache(int max_age_seconds = 300);

  /**
   * @brief 获取消息缓存统计信息
   * @param cache_size 输出参数：缓存大小
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_message_cache_size(size_t& cache_size) const;

  /**
   * @brief 获取线程数量
   * @return 线程数量
   */
  size_t get_thread_count() const;

  // 下列计数只在连接所在的事件线程调用。监控线程用 collect_protocol_counters 汇总。
  void note_publish_received(uint64_t payload_bytes);
  void note_publish_sent(uint64_t payload_bytes);
  void note_bytes_received(uint64_t bytes);
  void note_bytes_sent(uint64_t bytes);
  void note_connect_timeout();
  void note_keepalive_timeout();
  void note_session_takeover();
  void note_malformed_disconnect();
  void note_outbound_packet_size_drop();
  void note_inflight_window_delay();
  void note_will_published();
  void note_will_rejected();
  void note_retained_delta(int64_t messages, int64_t bytes);
  void note_retained_not_stored();
  void note_retained_delivery_truncated();
  void collect_protocol_counters(ProtocolCounterSnapshot& out) const;

  // 订阅变为可见之前加锁，直到 deliver_retained 入队结束再放开。
  // 和保留发布的“写入 + 扇出”互斥，避免新订阅先看到新值再补上旧快照。
  std::unique_lock<std::recursive_mutex> lock_retain_replay();

  // SUBACK 之后调用。retain 恒为 1，QoS 取订阅和消息的较小值。
  // 持久化会话的 QoS>0 仍走在线发送，会被在线路径拒绝；定向入队留到保留日志那份 PR。
  int deliver_retained(const MQTTString& client_id, const MQTTString& topic_filter,
                       const SubscriptionOptions& options, bool subscription_existed);

  /**
   * @brief 获取总的Handler数量
   * @param total_count 输出参数：总Handler数量
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_total_handler_count(size_t& total_count) const;

  /**
   * @brief 获取总的待处理消息数量
   * @param total_count 输出参数：总待处理消息数量
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_total_pending_message_count(size_t& total_count) const;

  /**
   * @brief 设置事件转发服务
   * @param forwarding_service 事件转发服务指针
   */
  void set_event_forwarding_service(events::EventForwardingService* forwarding_service);

  /**
   * @brief 触发登录事件
   * @param client_id 客户端ID
   * @param username 用户名
   * @param protocol_version 协议版本
   * @param keep_alive 保活时间
   * @param clean_session 清理会话标志
   * @param client_ip 客户端IP
   * @param client_port 客户端端口
   */
  void trigger_login_event(const MQTTString& client_id,
                          const MQTTString& username,
                          const MQTTString& protocol_version,
                          int keep_alive,
                          bool clean_session,
                          const MQTTString& client_ip = MQTTString(),
                          int client_port = 0);

  /**
   * @brief 触发登出事件
   * @param client_id 客户端ID
   * @param reason 登出原因
   * @param session_duration_seconds 会话持续时间（秒）
   * @param bytes_sent 发送字节数
   * @param bytes_received 接收字节数
   * @param messages_sent 发送消息数
   * @param messages_received 接收消息数
   * @param client_ip 客户端IP
   * @param client_port 客户端端口
   */
  void trigger_logout_event(const MQTTString& client_id,
                           const MQTTString& reason,
                           int session_duration_seconds,
                           int64_t bytes_sent,
                           int64_t bytes_received,
                           int messages_sent,
                           int messages_received,
                           const MQTTString& client_ip = MQTTString(),
                           int client_port = 0);

  /**
   * @brief 触发发布事件
   * @param client_id 客户端ID
   * @param topic 主题
   * @param qos QoS级别
   * @param retain 保留标志
   * @param duplicate 重复标志
   * @param payload_size 载荷大小
   * @param payload 载荷数据（可选）
   * @param client_ip 客户端IP
   * @param client_port 客户端端口
   */
  void trigger_publish_event(const MQTTString& client_id,
                            const MQTTString& topic,
                            int qos,
                            bool retain,
                            bool duplicate,
                            int payload_size,
                            const MQTTVector<uint8_t>& payload = MQTTVector<uint8_t>(),
                            const MQTTString& client_ip = MQTTString(),
                            int client_port = 0);

  /**
   * @brief 设置路由RPC客户端
   * @param router_client 路由RPC客户端指针
   */
  void set_router_client(std::unique_ptr<MQTTRouterRpcClient> router_client);

  /**
   * @brief 设置集群配置
   * @param cluster_config 集群配置
   * @return MQ_SUCCESS成功，其他值失败
   */
  int set_cluster_config(const ClusterConfig& cluster_config);

  /**
   * @brief 获取路由RPC客户端
   * @return 路由RPC客户端指针
   */
  MQTTRouterRpcClient* get_router_client() const;

  /**
   * @brief 设置服务器ID
   * @param server_id 服务器ID
   */
  void set_server_id(const MQTTString& server_id);

  /**
   * @brief 获取服务器ID
   * @return 服务器ID
   */
  const MQTTString& get_server_id() const;

  /**
   * @brief 向路由器注册服务器
   * @return MQ_SUCCESS成功，其他值失败
   */
  int register_with_router();
  int heartbeat_with_router();

  /**
   * @brief 通知路由器客户端连接
   * @param client_id 客户端ID
   * @param username 用户名
   * @param protocol_version 协议版本
   * @param keep_alive 保活时间
   * @param clean_session 清理会话标志
   * @return MQ_SUCCESS成功，其他值失败
   */
  int notify_router_client_connect(const MQTTString& client_id,
                                  const MQTTString& username = MQTTString(),
                                  uint32_t protocol_version = 4,
                                  uint32_t keep_alive = 60,
                                  bool clean_session = true);

  /**
   * @brief 通知路由器客户端断开
   * @param client_id 客户端ID
   * @param disconnect_reason 断开原因
   * @return MQ_SUCCESS成功，其他值失败
   */
  int notify_router_client_disconnect(const MQTTString& client_id,
                                     const MQTTString& disconnect_reason = MQTTString());

  /**
   * @brief 通过路由器订阅主题
   * @param topic_filter 主题过滤器
   * @param client_id 客户端ID
   * @param qos QoS级别
   * @return MQ_SUCCESS成功，其他值失败
   */
  int subscribe_topic_with_router(const MQTTString& topic_filter, const MQTTString& client_id, uint8_t qos = 0);
  int subscribe_topic_with_router(const MQTTString& topic_filter, const MQTTString& client_id,
                                  const SubscriptionOptions& options);
  int subscribe_topic_cluster(const MQTTString& topic_filter, const MQTTString& client_id, uint8_t qos = 0);
  int subscribe_topic_cluster(const MQTTString& topic_filter, const MQTTString& client_id,
                              const SubscriptionOptions& options);

  /**
   * @brief 通过路由器取消订阅主题
   * @param topic_filter 主题过滤器
   * @param client_id 客户端ID
   * @return MQ_SUCCESS成功，其他值失败
   */
  int unsubscribe_topic_with_router(const MQTTString& topic_filter, const MQTTString& client_id);
  int unsubscribe_topic_cluster(const MQTTString& topic_filter, const MQTTString& client_id);

  /**
   * @brief 通过路由器转发PUBLISH消息
   * @param topic 主题
   * @param packet 发布包
   * @param sender_client_id 发送者客户端ID
   * @return 转发的目标数量
   */
  int forward_publish_via_router(const MQTTString& topic, const PublishPacket& packet,
                                const MQTTString& sender_client_id);
  int forward_publish_cluster(const MQTTString& topic,
                              const PublishPacket& packet,
                              const MQTTString& sender_client_id,
                              int& delivered_count);

  /**
   * @brief 检查路由器是否可用
   * @return true可用，false不可用
   */
  bool is_router_available() const;

 private:
  std::unique_ptr<DurableStore> durable_store_;
  bool implicit_no_local_ = false;
  RetainConfig retain_config_;
  std::unique_ptr<RetainStore> retain_store_;
  // 递归锁：订阅侧持有它再调用 deliver_retained，投递内部会再锁一次。
  std::recursive_mutex retain_replay_mu_;
  WillScheduler will_scheduler_;
  // 运行状态
  enum class ManagerState {
    INITIALIZING,  // 初始化阶段
    REGISTERING,   // 线程注册阶段
    RUNNING        // 运行阶段（无锁优化）
  };

  std::atomic<ManagerState> state_;

  // 线程管理器存储（启动后不再变更，支持无锁访问）
  MQTTAllocator* global_allocator_;
  mutable RWMutex managers_mutex_;  // 读写锁，读多写少
  std::unordered_map<std::thread::id, std::unique_ptr<ThreadLocalSessionManager>> thread_managers_;
  std::vector<ThreadLocalSessionManager*> thread_manager_array_;  // 预分配数组，支持快速遍历

  // 客户端索引（高频读写，使用读写锁优化）
  mutable RWMutex client_index_mutex_;  // 读写锁
  std::unordered_map<std::string, ThreadLocalSessionManager*>
      client_to_manager_;  // 直接存储管理器指针
  // 从查找到安装完成之前占住 Client ID，避免并发接管看到旧主人或空位后再注册一份。
  std::unordered_map<std::string, std::shared_ptr<SessionTakeoverGate>> client_reservations_;

  // 消息内容缓存管理器
  std::unique_ptr<MessageContentCache> message_cache_;

  // 高性能主题匹配树
  std::unique_ptr<ConcurrentTopicTree> topic_tree_;

  // 事件转发服务
  events::EventForwardingService* event_forwarding_service_;

  // 路由RPC客户端
  std::unique_ptr<MQTTRouterRpcClient> router_client_;
  std::unique_ptr<ForwardingConnectionPool> forwarding_pool_;
  std::unique_ptr<MQTTForwardingRpcClient> forwarding_client_;
  std::unique_ptr<MQTTForwardingService> forwarding_service_;
  MQTTString server_id_;
  ClusterConfig cluster_config_;

  // 线程本地缓存（避免重复查找）
  thread_local static GlobalSessionManager* cached_manager_owner_;
  thread_local static ThreadLocalSessionManager* cached_thread_manager_;

  /**
   * @brief 内部快速查找客户端管理器（无锁版本）
   */
  ThreadLocalSessionManager* fast_find_client_manager(const std::string& client_id_str) const;
  ProtocolCounters* current_thread_counters();

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
  // retain = 0 的发布不得调用这里，避免热路径碰到保留库的锁。
  void remember_retained(const PublishPacket& packet, const MQTTString& sender);
};

// 单例访问
typedef Singleton<GlobalSessionManager> GlobalSessionManagerInstance;

}  // namespace mqtt

#endif  // MQTT_SESSION_MANAGER_V2_H
