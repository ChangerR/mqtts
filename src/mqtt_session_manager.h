#ifndef MQTT_SESSION_MANAGER_H
#define MQTT_SESSION_MANAGER_H

#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include "mqtt_allocator.h"
#include "mqtt_parser.h"
#include "singleton.h"

namespace mqtt {

// 前向声明
class MQTTProtocolHandler;

/**
 * @brief 安全的handler访问包装器，使用RAII确保访问期间handler不被释放
 */
class SafeHandlerRef {
public:
  SafeHandlerRef();
  SafeHandlerRef(MQTTProtocolHandler* handler, std::mutex* mutex, std::atomic<int>* ref_count, std::condition_variable* cv);
  ~SafeHandlerRef();
  
  // 移动构造和赋值
  SafeHandlerRef(SafeHandlerRef&& other) noexcept;
  SafeHandlerRef& operator=(SafeHandlerRef&& other) noexcept;
  
  // 禁止拷贝
  SafeHandlerRef(const SafeHandlerRef&) = delete;
  SafeHandlerRef& operator=(const SafeHandlerRef&) = delete;
  
  // 访问handler
  MQTTProtocolHandler* get() const { return handler_; }
  MQTTProtocolHandler* operator->() const { return handler_; }
  MQTTProtocolHandler& operator*() const { return *handler_; }
  
  // 检查是否有效
  bool is_valid() const { return handler_ != nullptr; }
  explicit operator bool() const { return is_valid(); }

private:
  MQTTProtocolHandler* handler_;
  std::mutex* mutex_;
  std::atomic<int>* ref_count_;
  std::condition_variable* cv_;
  bool acquired_;
  
  void release();
};

/**
 * @brief 会话信息结构，包含handler指针、互斥锁和引用计数
 */
struct SessionInfo {
  MQTTProtocolHandler* handler;          // 原始指针，不管理生命周期
  mutable std::mutex handler_mutex;      // 保护handler访问的互斥锁
  mutable std::condition_variable handler_cv;  // 条件变量，用于等待handler空闲
  std::atomic<bool> is_valid;            // 有效性标记（原子操作）
  std::atomic<int> ref_count;            // 当前使用handler的线程数
  std::atomic<bool> pending_release;     // 是否等待释放
  
  SessionInfo(MQTTProtocolHandler* h) 
    : handler(h), is_valid(true), ref_count(0), pending_release(false) {}
  
  // 禁止拷贝，只允许移动
  SessionInfo(const SessionInfo&) = delete;
  SessionInfo& operator=(const SessionInfo&) = delete;
  SessionInfo(SessionInfo&&) = default;
  SessionInfo& operator=(SessionInfo&&) = default;
};

/**
 * @brief MQTT会话管理器，线程安全地管理所有客户端连接
 * 注意：不负责handler的生命周期管理，但确保访问期间handler不被释放
 */
class SessionManager {
public:
  using MessageForwarder = std::function<void(const PublishPacket&, const MQTTString& sender_client_id)>;

  SessionManager();
  ~SessionManager();

  // 禁止拷贝和赋值
  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  /**
   * @brief 注册新的客户端会话
   * @param client_id 客户端ID
   * @param handler 协议处理器原始指针
   * @return 0成功，非0失败
   */
  int register_session(const MQTTString& client_id, MQTTProtocolHandler* handler);

  /**
   * @brief 注销客户端会话（等待所有使用完成后才标记为无效）
   * @param client_id 客户端ID
   * @param wait_for_completion 是否等待所有使用完成
   * @return 0成功，非0失败
   */
  int unregister_session(const MQTTString& client_id, bool wait_for_completion = true);

  /**
   * @brief 安全地获取handler引用，确保使用期间不被释放
   * @param client_id 客户端ID
   * @return 安全的handler引用，如果不存在或无效返回空引用
   */
  SafeHandlerRef get_safe_handler(const MQTTString& client_id);

  /**
   * @brief 准备释放handler（通知SessionManager即将释放某个handler）
   * @param handler 即将被释放的handler指针
   * @param timeout_ms 等待超时时间（毫秒），0表示不等待
   * @return true成功准备释放，false超时或失败
   */
  bool prepare_handler_release(MQTTProtocolHandler* handler, int timeout_ms = 5000);

  /**
   * @brief 获取所有活跃会话的数量
   * @return 会话数量
   */
  size_t get_session_count() const;

  /**
   * @brief 获取所有客户端ID列表
   * @return 客户端ID列表
   */
  std::vector<MQTTString> get_all_client_ids() const;

  /**
   * @brief 转发PUBLISH消息到订阅了指定主题的客户端
   * @param topic 主题名称
   * @param packet PUBLISH数据包
   * @param sender_client_id 发送者客户端ID（避免回环）
   * @return 转发的客户端数量
   */
  int forward_publish(const MQTTString& topic, const PublishPacket& packet, 
                     const MQTTString& sender_client_id);

  /**
   * @brief 广播消息到所有连接的客户端
   * @param packet PUBLISH数据包
   * @param exclude_client_id 要排除的客户端ID
   * @return 广播的客户端数量
   */
  int broadcast_publish(const PublishPacket& packet, const MQTTString& exclude_client_id = MQTTString());

  /**
   * @brief 检查主题是否匹配主题过滤器（支持通配符）
   * @param topic 实际主题
   * @param topic_filter 主题过滤器
   * @return true匹配，false不匹配
   */
  bool is_topic_match(const MQTTString& topic, const MQTTString& topic_filter) const;

  /**
   * @brief 获取订阅了指定主题的客户端列表
   * @param topic 主题名称
   * @return 客户端ID列表
   */
  std::vector<MQTTString> get_subscribers(const MQTTString& topic) const;

  /**
   * @brief 清理所有无效的会话记录
   * @return 清理的会话数量
   */
  int cleanup_invalid_sessions();

  /**
   * @brief 设置消息转发回调函数
   * @param forwarder 转发回调函数
   */
  void set_message_forwarder(const MessageForwarder& forwarder);

private:
  mutable std::mutex sessions_mutex_;  // 保护会话映射的互斥锁
  std::unordered_map<std::string, std::unique_ptr<SessionInfo>> sessions_;  // 客户端ID到会话信息的映射
  std::unordered_set<MQTTProtocolHandler*> valid_handlers_;  // 有效的handler集合

  MessageForwarder message_forwarder_;  // 消息转发回调
  mutable std::mutex forwarder_mutex_;  // 保护转发器的锁

  /**
   * @brief 内部辅助函数：检查通配符匹配
   * @param topic 实际主题
   * @param filter 主题过滤器
   * @return true匹配，false不匹配
   */
  bool match_topic_filter(const MQTTString& topic, const MQTTString& filter) const;

  /**
   * @brief 内部辅助函数：检查handler是否有效且已连接
   * @param info 会话信息
   * @return true有效且已连接，false无效或未连接
   */
  bool is_session_active(const SessionInfo& info) const;

  /**
   * @brief 等待指定的handler没有任何线程在使用
   * @param info 会话信息
   * @param timeout_ms 超时时间（毫秒）
   * @return true等待成功，false超时
   */
  bool wait_for_handler_idle(SessionInfo& info, int timeout_ms);
};

// 单例访问
typedef Singleton<SessionManager> GlobalSessionManager;

}  // namespace mqtt

#endif  // MQTT_SESSION_MANAGER_H 