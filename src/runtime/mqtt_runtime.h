#pragma once

#include <pthread.h>
#include <sys/socket.h>
#include <poll.h>
#include <cstddef>

namespace mqtt {
namespace runtime {

typedef void* (*TaskEntry)(void*);
typedef int (*EventLoopCallback)(void*);

// Upper bound of the per-task local storage slots offered by the backend.
const pthread_key_t kMaxTaskLocalKeys = 1024;

/**
 * @brief 协程任务句柄。
 *
 * 任务归属于创建它的线程（调用 spawn() 的线程），只有该线程可以运行、等待或
 * 回收它。join()/release() 从其他线程调用时不会释放协程栈：join() 返回 EPERM，
 * release() 仅放弃所有权（可能造成泄漏），以避免与归属线程竞争。
 */
class TaskHandle
{
 public:
  TaskHandle();
  ~TaskHandle();

  TaskHandle(TaskHandle&& other) noexcept;
  TaskHandle& operator=(TaskHandle&& other) noexcept;

  TaskHandle(const TaskHandle&) = delete;
  TaskHandle& operator=(const TaskHandle&) = delete;

  // 句柄是否指向一个任务，与任务是否仍在运行无关。
  bool is_valid() const { return task_ != nullptr; }

  // 任务入口函数是否已经返回。无效句柄视为已结束。
  bool is_finished() const;

  /**
   * @brief 在归属线程的非协程上下文中驱动事件循环，直到任务结束或超时。
   * @param timeout_ms 正数为超时毫秒数；负数表示无限等待；0 表示只做一次探测。
   * @return 0 表示任务已结束；-1 并设置 errno：
   *         EPERM 非归属线程或处于协程上下文，EBUSY timeout_ms 为 0 且任务未结束，
   *         ETIMEDOUT 等待超时。
   */
  int join(int timeout_ms = -1);

  // 放弃句柄。任务已结束且当前为归属线程时立即回收，否则交由任务自行回收。
  void release();

 private:
  friend class LibcoRuntime;

  explicit TaskHandle(void* task);

  void* task_;
};

/**
 * @brief 协程互斥量，仅用于同一线程内的协程之间，不能跨线程使用。
 *
 * 非协程上下文没有可让出的目标，此时 lock() 不会等待而直接进入临界区。
 */
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

/**
 * @brief 协程条件变量，唤醒动作只作用于当前线程的事件循环，不能跨线程使用。
 *
 * 移动操作只搬移内部指针，存在等待者时移动是未定义行为。
 */
class AsyncCondition
{
 public:
  AsyncCondition();
  ~AsyncCondition();

  AsyncCondition(const AsyncCondition&) = delete;
  AsyncCondition& operator=(const AsyncCondition&) = delete;

  AsyncCondition(AsyncCondition&& other) noexcept;
  AsyncCondition& operator=(AsyncCondition&& other) noexcept;

  /**
   * @brief 挂起当前协程直到被唤醒或超时。
   *
   * 非协程上下文没有可挂起的协程：timeout_ms 为正数时退化为真实睡眠并返回 0，
   * 否则返回 -1 并设置 errno 为 EPERM。
   *
   * @param timeout_ms 正数为超时毫秒数；0 与负数均表示无限等待。
   * @return 0 表示被唤醒或超时（底层无法区分两者）；-1 并设置 errno。
   */
  int wait(int timeout_ms = -1);

  // 唤醒本线程上的等待者。等待者位于其他线程时唤醒会被忽略。
  void signal();
  void broadcast();
  bool is_valid() const { return impl_ != nullptr; }

 private:
  bool can_wake() const;

  void* impl_;
};

class IoWaiter
{
 public:
  // timeout_ms == 0 或调用方不在协程上下文时退化为真实的 ::poll()。
  int wait(int fd, short events, int timeout_ms);
  int wait_readable(int fd, int timeout_ms);
  int wait_writable(int fd, int timeout_ms);
};

class LibcoRuntime
{
 public:
  static LibcoRuntime& instance();

  // 系统调用拦截只对当前协程生效，每个需要它的协程都要自行开启。
  void enable_hook();
  void disable_hook();

  IoWaiter& io_waiter();

  /**
   * @brief 在当前线程创建并立即启动一个任务。
   *
   * 入口函数会在 spawn() 返回前同步执行到第一个让出点，因此任务依赖的状态必须
   * 在调用 spawn() 之前准备好。返回的句柄归属于当前线程。
   */
  TaskHandle spawn(TaskEntry entry, void* arg, size_t stack_size = 0);

  int run_event_loop(EventLoopCallback callback, void* arg);

  // 回收本线程已结束且被放弃的任务，供不驱动事件循环的线程定期调用。
  int reap_tasks();

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

// 进程级单例。libco 的调度状态本身是线程本地的，因此同一个 Runtime 对象在不同
// 线程上操作的是各自线程的事件循环。
LibcoRuntime& current_runtime();

}  // namespace runtime
}  // namespace mqtt
