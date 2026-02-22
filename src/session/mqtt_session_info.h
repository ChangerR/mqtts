#ifndef MQTT_SESSION_INFO_H
#define MQTT_SESSION_INFO_H

#include <atomic>
#include <memory>
#include "mqtt_coroutine_utils.h"

namespace mqtt {

// 前向声明
class MQTTProtocolHandler;

/**
 * @brief Session信息结构，使用协程兼容的同步机制
 */
struct SessionInfo
{
  MQTTProtocolHandler* handler;       // handler指针（不管理生命周期）
  mutable CoroMutex session_mutex;    // 每个session独立的协程锁
  std::atomic<int> ref_count;         // 当前引用计数
  std::atomic<bool> is_valid;         // session是否有效
  std::atomic<bool> pending_removal;  // 是否等待移除

  // 使用协程信号量实现优雅的等待机制
  mutable CoroCondition zero_refs_cond;  // 等待引用计数归零的信号量

  explicit SessionInfo(MQTTProtocolHandler* h)
      : handler(h), ref_count(0), is_valid(true), pending_removal(false)
  {
  }

  // 禁止拷贝，只允许移动
  SessionInfo(const SessionInfo&) = delete;
  SessionInfo& operator=(const SessionInfo&) = delete;
  SessionInfo(SessionInfo&&) = default;
  SessionInfo& operator=(SessionInfo&&) = default;

  /**
   * @brief 协程友好的等待引用计数归零
   * @param timeout_ms 超时时间（毫秒），-1表示无限等待
   * @return true成功等待到引用计数归零，false超时
   */
  bool wait_for_zero_refs(int timeout_ms = 5000) const
  {
    while (ref_count.load() > 0) {
      // 使用协程信号量等待
      int result = zero_refs_cond.wait(timeout_ms);
      if (result != 0) {
        // 超时或出错
        return false;
      }
      // 被唤醒后再次检查引用计数
    }
    return true;
  }

  /**
   * @brief 通知等待引用计数归零的协程
   * 在引用计数变为0时调用
   */
  void notify_zero_refs() const
  {
    if (ref_count.load() == 0) {
      zero_refs_cond.broadcast();
    }
  }
};

/**
 * @brief 安全的handler引用包装器，使用RAII管理引用计数
 */
class SafeHandlerRef
{
 public:
  SafeHandlerRef() = default;

  // 从SessionInfo构造安全引用
  explicit SafeHandlerRef(SessionInfo* info);

  // 析构函数，自动释放引用
  ~SafeHandlerRef();

  // 移动构造和赋值
  SafeHandlerRef(SafeHandlerRef&& other) noexcept;
  SafeHandlerRef& operator=(SafeHandlerRef&& other) noexcept;

  // 禁止拷贝
  SafeHandlerRef(const SafeHandlerRef&) = delete;
  SafeHandlerRef& operator=(const SafeHandlerRef&) = delete;

  // 检查是否有效
  bool is_valid() const { return handler_ != nullptr && is_acquired_; }

  // 访问handler
  MQTTProtocolHandler* operator->() const { return handler_; }
  MQTTProtocolHandler* get() const { return handler_; }

  // 显式释放引用
  void release();

 private:
  MQTTProtocolHandler* handler_ = nullptr;
  SessionInfo* session_info_ = nullptr;
  bool is_acquired_ = false;

  void acquire();
  void internal_release();
};

}  // namespace mqtt

#endif  // MQTT_SESSION_INFO_H