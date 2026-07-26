#pragma once

#include <pthread.h>
#include <sys/socket.h>
#include <poll.h>
#include <cstddef>

namespace mqtt {
namespace runtime {

typedef void* (*TaskEntry)(void*);
typedef int (*EventLoopCallback)(void*);

class TaskHandle
{
 public:
  TaskHandle();
  ~TaskHandle();

  TaskHandle(TaskHandle&& other) noexcept;
  TaskHandle& operator=(TaskHandle&& other) noexcept;

  TaskHandle(const TaskHandle&) = delete;
  TaskHandle& operator=(const TaskHandle&) = delete;

  bool is_valid() const { return task_ != nullptr; }
  bool is_finished() const;
  int join(int timeout_ms = -1);
  void release();

 private:
  friend class LibcoRuntime;

  explicit TaskHandle(void* task);

  void* task_;
};

class AsyncMutex
{
 public:
  AsyncMutex();
  ~AsyncMutex();

  AsyncMutex(const AsyncMutex&) = delete;
  AsyncMutex& operator=(const AsyncMutex&) = delete;

  void lock();
  void unlock();

 private:
  void* impl_;
};

class AsyncLockGuard
{
 public:
  explicit AsyncLockGuard(AsyncMutex* mutex);
  ~AsyncLockGuard();

  AsyncLockGuard(const AsyncLockGuard&) = delete;
  AsyncLockGuard& operator=(const AsyncLockGuard&) = delete;

 private:
  AsyncMutex* mutex_;
};

class AsyncCondition
{
 public:
  AsyncCondition();
  ~AsyncCondition();

  AsyncCondition(const AsyncCondition&) = delete;
  AsyncCondition& operator=(const AsyncCondition&) = delete;

  AsyncCondition(AsyncCondition&& other) noexcept;
  AsyncCondition& operator=(AsyncCondition&& other) noexcept;

  int wait(int timeout_ms = -1);
  void signal();
  void broadcast();
  bool is_valid() const { return impl_ != nullptr; }

 private:
  void* impl_;
};

class IoWaiter
{
 public:
  // timeout_ms == 0 performs a non-blocking ::poll().
  // Positive and infinite waits must be called from a libco coroutine.
  int wait(int fd, short events, int timeout_ms);
  int wait_readable(int fd, int timeout_ms);
  int wait_writable(int fd, int timeout_ms);
};

class LibcoRuntime
{
 public:
  static LibcoRuntime& instance();

  void enable_hook();
  void disable_hook();

  IoWaiter& io_waiter();
  // Creates and immediately resumes a task. With libco, entry may run until its
  // first yield point before spawn() returns.
  TaskHandle spawn(TaskEntry entry, void* arg, size_t stack_size = 0);
  int run_event_loop(EventLoopCallback callback, void* arg);
  int wait(int fd, short events, int timeout_ms);
  int wait_readable(int fd, int timeout_ms);
  int wait_writable(int fd, int timeout_ms);
  int accept(int fd, struct sockaddr* addr, socklen_t* len);

  void* get_specific(pthread_key_t key);
  int set_specific(pthread_key_t key, const void* value);

 private:
  LibcoRuntime() = default;
  ~LibcoRuntime() = default;

  LibcoRuntime(const LibcoRuntime&) = delete;
  LibcoRuntime& operator=(const LibcoRuntime&) = delete;

  IoWaiter io_waiter_;
};

using Runtime = LibcoRuntime;

LibcoRuntime& current_runtime();

}  // namespace runtime
}  // namespace mqtt
