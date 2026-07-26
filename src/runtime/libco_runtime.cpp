#include "mqtt_runtime.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>
#include "co_comm.h"
#include "co_routine.h"
#include "co_routine_inner.h"

int co_accept(int fd, struct sockaddr* addr, socklen_t* len);

namespace mqtt {
namespace runtime {

namespace {

struct TaskState
{
  stCoRoutine_t* task;
  TaskEntry entry;
  void* arg;
  bool finished;
  bool detached;

  TaskState(TaskEntry task_entry, void* task_arg)
      : task(nullptr),
        entry(task_entry),
        arg(task_arg),
        finished(false),
        detached(false)
  {
  }
};

std::mutex& reap_mutex()
{
  static std::mutex mutex;
  return mutex;
}

std::vector<TaskState*>& reap_queue()
{
  static std::vector<TaskState*> queue;
  return queue;
}

void enqueue_reap(TaskState* state)
{
  std::lock_guard<std::mutex> lock(reap_mutex());
  reap_queue().push_back(state);
}

void reap_finished_tasks()
{
  std::vector<TaskState*> ready;
  {
    std::lock_guard<std::mutex> lock(reap_mutex());
    ready.swap(reap_queue());
  }

  for (TaskState* state : ready) {
    if (state && state->finished) {
      co_release(state->task);
      delete state;
    } else if (state) {
      enqueue_reap(state);
    }
  }
}

void* task_trampoline(void* arg)
{
  TaskState* state = static_cast<TaskState*>(arg);
  void* result = state->entry(state->arg);
  state->finished = true;
  if (state->detached) {
    enqueue_reap(state);
  }
  return result;
}

TaskState* as_state(void* task)
{
  return static_cast<TaskState*>(task);
}

bool is_coroutine_context()
{
  stCoRoutine_t* self = co_self();
  return self && !self->cIsMain;
}

struct EventLoopContext
{
  EventLoopCallback callback;
  void* arg;
};

int eventloop_trampoline(void* arg)
{
  EventLoopContext* ctx = static_cast<EventLoopContext*>(arg);
  reap_finished_tasks();
  if (!ctx->callback) {
    return 0;
  }
  return ctx->callback(ctx->arg);
}

struct JoinContext
{
  TaskState* state;
  int timeout_ms;
  std::chrono::steady_clock::time_point start;
  bool timed_out;
};

int join_eventloop_callback(void* arg)
{
  JoinContext* ctx = static_cast<JoinContext*>(arg);
  reap_finished_tasks();
  if (ctx->state->finished) {
    return -1;
  }

  if (ctx->timeout_ms >= 0) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - ctx->start);
    if (elapsed.count() >= ctx->timeout_ms) {
      ctx->timed_out = true;
      return -1;
    }
  }

  return 0;
}

}  // namespace

TaskHandle::TaskHandle() : task_(nullptr) {}

TaskHandle::TaskHandle(void* task) : task_(task) {}

TaskHandle::~TaskHandle()
{
  release();
}

TaskHandle::TaskHandle(TaskHandle&& other) noexcept : task_(other.task_)
{
  other.task_ = nullptr;
}

TaskHandle& TaskHandle::operator=(TaskHandle&& other) noexcept
{
  if (this != &other) {
    release();
    task_ = other.task_;
    other.task_ = nullptr;
  }
  return *this;
}

void TaskHandle::release()
{
  reap_finished_tasks();

  TaskState* state = as_state(task_);
  if (!state) {
    return;
  }

  if (!state->finished) {
    state->detached = true;
    if (!state->finished) {
      task_ = nullptr;
      return;
    }
  }

  co_release(state->task);
  delete state;
  task_ = nullptr;
}

bool TaskHandle::is_finished() const
{
  TaskState* state = as_state(task_);
  return !state || state->finished;
}

int TaskHandle::join(int timeout_ms)
{
  TaskState* state = as_state(task_);
  if (!state || state->finished) {
    return 0;
  }

  if (timeout_ms == 0) {
    errno = ETIMEDOUT;
    return -1;
  }

  JoinContext ctx = {state, timeout_ms, std::chrono::steady_clock::now(), false};
  co_eventloop(co_get_epoll_ct(), join_eventloop_callback, &ctx);
  if (!state->finished) {
    errno = ctx.timed_out ? ETIMEDOUT : EAGAIN;
    return -1;
  }

  return 0;
}

AsyncMutex::AsyncMutex() : impl_(new clsCoMutex()) {}

AsyncMutex::~AsyncMutex()
{
  delete static_cast<clsCoMutex*>(impl_);
  impl_ = nullptr;
}

void AsyncMutex::lock()
{
  static_cast<clsCoMutex*>(impl_)->CoLock();
}

void AsyncMutex::unlock()
{
  static_cast<clsCoMutex*>(impl_)->CoUnLock();
}

AsyncLockGuard::AsyncLockGuard(AsyncMutex* mutex) : mutex_(mutex)
{
  if (mutex_) {
    mutex_->lock();
  }
}

AsyncLockGuard::~AsyncLockGuard()
{
  if (mutex_) {
    mutex_->unlock();
  }
}

AsyncCondition::AsyncCondition() : impl_(co_cond_alloc())
{
  if (!impl_) {
    throw std::runtime_error("Failed to allocate coroutine condition variable");
  }
}

AsyncCondition::~AsyncCondition()
{
  if (impl_) {
    co_cond_free(static_cast<stCoCond_t*>(impl_));
    impl_ = nullptr;
  }
}

AsyncCondition::AsyncCondition(AsyncCondition&& other) noexcept : impl_(other.impl_)
{
  other.impl_ = nullptr;
}

AsyncCondition& AsyncCondition::operator=(AsyncCondition&& other) noexcept
{
  if (this != &other) {
    if (impl_) {
      co_cond_free(static_cast<stCoCond_t*>(impl_));
    }
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

int AsyncCondition::wait(int timeout_ms)
{
  return impl_ ? co_cond_timedwait(static_cast<stCoCond_t*>(impl_), timeout_ms) : -1;
}

void AsyncCondition::signal()
{
  if (impl_) {
    co_cond_signal(static_cast<stCoCond_t*>(impl_));
  }
}

void AsyncCondition::broadcast()
{
  if (impl_) {
    co_cond_broadcast(static_cast<stCoCond_t*>(impl_));
  }
}

int IoWaiter::wait(int fd, short events, int timeout_ms)
{
  return current_runtime().wait(fd, events, timeout_ms);
}

int IoWaiter::wait_readable(int fd, int timeout_ms)
{
  return current_runtime().wait_readable(fd, timeout_ms);
}

int IoWaiter::wait_writable(int fd, int timeout_ms)
{
  return current_runtime().wait_writable(fd, timeout_ms);
}

LibcoRuntime& LibcoRuntime::instance()
{
  static LibcoRuntime runtime;
  return runtime;
}

void LibcoRuntime::enable_hook()
{
  co_enable_hook_sys();
}

void LibcoRuntime::disable_hook()
{
  co_disable_hook_sys();
}

IoWaiter& LibcoRuntime::io_waiter()
{
  return io_waiter_;
}

TaskHandle LibcoRuntime::spawn(TaskEntry entry, void* arg, size_t stack_size)
{
  reap_finished_tasks();

  stCoRoutine_t* task = nullptr;
  TaskState* state = new TaskState(entry, arg);
  stCoRoutineAttr_t attr;
  std::memset(&attr, 0, sizeof(attr));
  stCoRoutineAttr_t* attr_ptr = nullptr;
  if (stack_size > 0) {
    attr.stack_size = stack_size;
    attr_ptr = &attr;
  }

  int ret = co_create(&task, attr_ptr, task_trampoline, state);
  if (ret != 0 || !task) {
    delete state;
    return TaskHandle();
  }

  state->task = task;
  co_resume(task);
  return TaskHandle(state);
}

int LibcoRuntime::run_event_loop(EventLoopCallback callback, void* arg)
{
  EventLoopContext ctx = {callback, arg};
  co_eventloop(co_get_epoll_ct(), eventloop_trampoline, &ctx);
  reap_finished_tasks();
  return 0;
}

int LibcoRuntime::wait(int fd, short events, int timeout_ms)
{
  struct pollfd pf;
  std::memset(&pf, 0, sizeof(pf));
  pf.fd = fd;
  pf.events = events;

  if (timeout_ms == 0) {
    return ::poll(&pf, 1, 0);
  }

  if (!is_coroutine_context()) {
    errno = EINVAL;
    return -1;
  }

  return co_poll(co_get_epoll_ct(), &pf, 1, timeout_ms);
}

int LibcoRuntime::wait_readable(int fd, int timeout_ms)
{
  return wait(fd, POLLIN | POLLERR | POLLHUP, timeout_ms);
}

int LibcoRuntime::wait_writable(int fd, int timeout_ms)
{
  return wait(fd, POLLOUT | POLLERR | POLLHUP, timeout_ms);
}

int LibcoRuntime::accept(int fd, struct sockaddr* addr, socklen_t* len)
{
  return co_accept(fd, addr, len);
}

void* LibcoRuntime::get_specific(pthread_key_t key)
{
  return co_getspecific(key);
}

int LibcoRuntime::set_specific(pthread_key_t key, const void* value)
{
  return co_setspecific(key, value);
}

LibcoRuntime& current_runtime()
{
  return LibcoRuntime::instance();
}

}  // namespace runtime
}  // namespace mqtt
