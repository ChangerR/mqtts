#ifndef PTHREAD_RWLOCK_WRAPPER_H
#define PTHREAD_RWLOCK_WRAPPER_H

#include <pthread.h>
#include <stdexcept>

namespace mqtt {

/**
 * @brief 基于pthread_rwlock的读写锁封装类（C++11兼容）
 */
class RWMutex
{
 public:
  RWMutex()
  {
    int result = pthread_rwlock_init(&rwlock_, nullptr);
    if (result != 0) {
      throw std::runtime_error("Failed to initialize pthread_rwlock");
    }
  }

  ~RWMutex() { pthread_rwlock_destroy(&rwlock_); }

  // 禁止拷贝和赋值
  RWMutex(const RWMutex&) = delete;
  RWMutex& operator=(const RWMutex&) = delete;

  void lock_read()
  {
    int result = pthread_rwlock_rdlock(&rwlock_);
    if (result != 0) {
      throw std::runtime_error("Failed to acquire read lock");
    }
  }

  void lock_write()
  {
    int result = pthread_rwlock_wrlock(&rwlock_);
    if (result != 0) {
      throw std::runtime_error("Failed to acquire write lock");
    }
  }

  void unlock()
  {
    int result = pthread_rwlock_unlock(&rwlock_);
    if (result != 0) {
      throw std::runtime_error("Failed to unlock rwlock");
    }
  }

  // 提供原生句柄访问（如果需要）
  pthread_rwlock_t* native_handle() { return &rwlock_; }

 private:
  pthread_rwlock_t rwlock_;
};

/**
 * @brief 读锁守护类（RAII）
 */
class ReadLockGuard
{
 public:
  explicit ReadLockGuard(RWMutex& mutex) : mutex_(mutex) { mutex_.lock_read(); }

  ~ReadLockGuard() { mutex_.unlock(); }

  // 禁止拷贝和赋值
  ReadLockGuard(const ReadLockGuard&) = delete;
  ReadLockGuard& operator=(const ReadLockGuard&) = delete;

 private:
  RWMutex& mutex_;
};

/**
 * @brief 写锁守护类（RAII）
 */
class WriteLockGuard
{
 public:
  explicit WriteLockGuard(RWMutex& mutex) : mutex_(mutex) { mutex_.lock_write(); }

  ~WriteLockGuard() { mutex_.unlock(); }

  // 禁止拷贝和赋值
  WriteLockGuard(const WriteLockGuard&) = delete;
  WriteLockGuard& operator=(const WriteLockGuard&) = delete;

 private:
  RWMutex& mutex_;
};

}  // namespace mqtt

#endif  // PTHREAD_RWLOCK_WRAPPER_H