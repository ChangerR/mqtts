#ifndef MQTT_COROUTINE_UTILS_H
#define MQTT_COROUTINE_UTILS_H

#include <stdexcept>
#include <memory>
#include "../3rd/libco/co_comm.h"
#include "../3rd/libco/co_routine.h"

namespace mqtt {

/**
 * @brief 协程锁类型定义，直接使用libco的clsCoMutex
 */
using CoroMutex = clsCoMutex;

/**
 * @brief 协程锁的RAII包装器，直接使用libco的clsSmartLock
 */
using CoroLockGuard = clsSmartLock;

/**
 * @brief 安全的协程锁包装器，可以检测协程环境是否可用
 */
class SafeCoroLockGuard {
public:
    SafeCoroLockGuard(CoroMutex* mutex) : mutex_(mutex), locked_(false) {
        if (mutex_ && is_coroutine_environment_available()) {
            try {
                lock_guard_.reset(new CoroLockGuard(mutex_));
                locked_ = true;
            } catch (...) {
                // 如果协程锁失败，降级为普通互斥锁行为
                locked_ = false;
            }
        }
    }
    
    ~SafeCoroLockGuard() = default;
    
    bool is_locked() const { return locked_; }
    
private:
    static bool is_coroutine_environment_available() {
        // 检查当前是否在协程环境中
        stCoEpoll_t* epoll_ctx = co_get_epoll_ct();
        return epoll_ctx != nullptr;
    }
    
    CoroMutex* mutex_;
    std::unique_ptr<CoroLockGuard> lock_guard_;
    bool locked_;
};

/**
 * @brief 协程信号量的RAII包装器
 */
class CoroCondition
{
 public:
  CoroCondition()
  {
    cond_ = co_cond_alloc();
    if (!cond_) {
      // Don't throw in test environments where coroutines aren't available
      // throw std::runtime_error("Failed to allocate coroutine condition variable");
    }
  }

  ~CoroCondition()
  {
    if (cond_) {
      co_cond_free(cond_);
      cond_ = nullptr;
    }
  }

  // 禁止拷贝
  CoroCondition(const CoroCondition&) = delete;
  CoroCondition& operator=(const CoroCondition&) = delete;

  // 允许移动
  CoroCondition(CoroCondition&& other) noexcept : cond_(other.cond_) { other.cond_ = nullptr; }

  CoroCondition& operator=(CoroCondition&& other) noexcept
  {
    if (this != &other) {
      if (cond_) {
        co_cond_free(cond_);
      }
      cond_ = other.cond_;
      other.cond_ = nullptr;
    }
    return *this;
  }

  /**
   * @brief 等待信号
   * @param timeout_ms 超时时间（毫秒），-1表示无限等待
   * @return 0成功，非0超时或错误
   */
  int wait(int timeout_ms = -1) { return cond_ ? co_cond_timedwait(cond_, timeout_ms) : -1; }

  /**
   * @brief 发送信号给一个等待的协程
   */
  void signal()
  {
    if (cond_) {
      co_cond_signal(cond_);
    }
  }

  /**
   * @brief 发送信号给所有等待的协程
   */
  void broadcast()
  {
    if (cond_) {
      co_cond_broadcast(cond_);
    }
  }

  /**
   * @brief 检查是否有效
   */
  bool is_valid() const { return cond_ != nullptr; }

 private:
  stCoCond_t* cond_ = nullptr;
};

}  // namespace mqtt

#endif  // MQTT_COROUTINE_UTILS_H