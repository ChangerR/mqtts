#include "mqtt_runtime.h"

#include <pthread.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "co_routine.h"
#include "co_routine_inner.h"

int co_accept(int fd, struct sockaddr* addr, socklen_t* len);

namespace mqtt {
namespace runtime {

namespace {

// Size of the per-coroutine slot array libco indexes with a pthread key value.
const pthread_key_t kMaxTaskLocalKeys = 1024;

// A libco coroutine may only be resumed or freed by the thread that created it,
// and its stack stays live until it has yielded for the last time. The lifecycle
// state below encodes who is allowed to reclaim the coroutine so that the stack
// is never freed while libco can still touch it.
enum TaskLifecycle
{
  kTaskRunning = 0,   // entry has not returned yet, the handle owns the state
  kTaskFinished = 1,  // entry returned, the handle is still responsible
  kTaskDetached = 2,  // handle gave up ownership, the task reclaims itself
  kTaskReaped = 3     // reclaim has been claimed, nobody else may free it
};

struct TaskState
{
  stCoRoutine_t* task;
  stCoRoutineEnv_t* owner_env;
  TaskEntry entry;
  void* arg;
  std::atomic<int> lifecycle;

  TaskState(TaskEntry task_entry, void* task_arg)
      : task(nullptr),
        owner_env(nullptr),
        entry(task_entry),
        arg(task_arg),
        lifecycle(kTaskRunning)
  {
  }
};

// Intentionally never destroyed: tasks can be reclaimed from static destructors,
// which would otherwise run after a thread_local container is already gone.
std::vector<TaskState*>& reap_queue()
{
  static thread_local std::vector<TaskState*>* queue = new std::vector<TaskState*>();
  return *queue;
}

void destroy_task_state(TaskState* state)
{
  co_release(state->task);
  delete state;
}

// Only ever holds tasks owned by this thread whose entry has already returned,
// so by the time the queue is drained the coroutine has performed its final
// yield and its stack is no longer referenced by libco.
void reap_finished_tasks()
{
  std::vector<TaskState*>& queue = reap_queue();
  while (!queue.empty()) {
    TaskState* state = queue.back();
    queue.pop_back();
    destroy_task_state(state);
  }
}

void* task_trampoline(void* arg)
{
  TaskState* state = static_cast<TaskState*>(arg);
  void* result = state->entry(state->arg);

  int expected = kTaskRunning;
  if (!state->lifecycle.compare_exchange_strong(expected, kTaskFinished)) {
    // The handle was released while this task was still running, so the task is
    // now responsible for handing its state back to the owning thread.
    state->lifecycle.store(kTaskReaped);
    reap_queue().push_back(state);
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

bool is_owner_thread(const TaskState* state)
{
  return state->owner_env == co_get_curr_thread_env();
}

// libco interposes poll() and turns it into a coroutine yield whenever the hook
// is active, which is not usable outside a coroutine. Disabling the hook around
// the call routes it to the real system poll().
int poll_without_hook(struct pollfd* fds, nfds_t nfds, int timeout_ms)
{
  const bool hook_enabled = co_is_enable_sys_hook();
  if (hook_enabled) {
    co_disable_hook_sys();
  }

  int ret = ::poll(fds, nfds, timeout_ms);

  if (hook_enabled) {
    co_enable_hook_sys();
  }

  return ret;
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
};

int join_eventloop_callback(void* arg)
{
  JoinContext* ctx = static_cast<JoinContext*>(arg);
  reap_finished_tasks();
  if (ctx->state->lifecycle.load() != kTaskRunning) {
    return -1;
  }

  if (ctx->timeout_ms > 0) {
    std::chrono::milliseconds elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - ctx->start);
    if (elapsed.count() >= ctx->timeout_ms) {
      return -1;
    }
  }

  return 0;
}

struct MutexState
{
  stCoCond_t* cond;
  std::atomic<int> hold_count;
  std::atomic<void*> waiter_env;
};

struct ConditionState
{
  stCoCond_t* cond;
  std::atomic<void*> waiter_env;
};

// libco queues a woken coroutine on the current thread's run list, so a wakeup
// issued from any thread other than the waiter's would run that coroutine on the
// wrong thread. Such wakeups are dropped instead.
bool can_wake_waiter(const std::atomic<void*>& waiter_env)
{
  void* env = co_get_curr_thread_env();
  if (!env) {
    return false;
  }

  void* waiter = waiter_env.load();
  return !waiter || waiter == env;
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
  TaskState* state = as_state(task_);
  task_ = nullptr;
  if (!state) {
    return;
  }

  if (!is_owner_thread(state)) {
    // Freeing the coroutine from here could race with its owning thread, so the
    // task is abandoned. It is reclaimed by the task itself if that thread is
    // still pumping an event loop, and leaked otherwise.
    int expected = kTaskRunning;
    state->lifecycle.compare_exchange_strong(expected, kTaskDetached);
    return;
  }

  reap_finished_tasks();

  int expected = kTaskRunning;
  if (state->lifecycle.compare_exchange_strong(expected, kTaskDetached)) {
    return;
  }

  if (expected == kTaskFinished &&
      state->lifecycle.compare_exchange_strong(expected, kTaskReaped)) {
    destroy_task_state(state);
  }
}

bool TaskHandle::is_finished() const
{
  const TaskState* state = as_state(task_);
  return !state || state->lifecycle.load() != kTaskRunning;
}

int TaskHandle::join(int timeout_ms)
{
  TaskState* state = as_state(task_);
  if (!state || state->lifecycle.load() != kTaskRunning) {
    return 0;
  }

  if (!is_owner_thread(state)) {
    errno = EPERM;
    return -1;
  }

  if (is_coroutine_context()) {
    // A nested event loop would starve the outer loop's callback, so waiting for
    // another task from inside a coroutine is rejected instead.
    errno = EPERM;
    return -1;
  }

  if (timeout_ms == 0) {
    errno = EBUSY;
    return -1;
  }

  JoinContext ctx = {state, timeout_ms, std::chrono::steady_clock::now()};
  co_eventloop(co_get_epoll_ct(), join_eventloop_callback, &ctx);

  if (state->lifecycle.load() == kTaskRunning) {
    errno = ETIMEDOUT;
    return -1;
  }

  return 0;
}

CoroutineMutex::CoroutineMutex() : impl_(nullptr)
{
  MutexState* state = new MutexState();
  state->cond = co_cond_alloc();
  state->hold_count.store(0);
  state->waiter_env.store(nullptr);
  if (!state->cond) {
    delete state;
    throw std::runtime_error("Failed to allocate coroutine mutex");
  }
  impl_ = state;
}

CoroutineMutex::~CoroutineMutex()
{
  MutexState* state = static_cast<MutexState*>(impl_);
  if (state) {
    co_cond_free(state->cond);
    delete state;
    impl_ = nullptr;
  }
}

void CoroutineMutex::lock()
{
  MutexState* state = static_cast<MutexState*>(impl_);
  if (!state) {
    return;
  }

  // Each unlock() hands the mutex to exactly one queued waiter, so the count of
  // holders plus waiters is enough to decide whether this caller has to queue.
  if (state->hold_count.fetch_add(1) > 0 && is_coroutine_context()) {
    state->waiter_env.store(co_get_curr_thread_env());
    co_cond_timedwait(state->cond, -1);
  }

  // Outside a coroutine there is nothing to yield to, so a contended lock is
  // taken without waiting rather than corrupting libco's call stack.
}

void CoroutineMutex::unlock()
{
  MutexState* state = static_cast<MutexState*>(impl_);
  if (!state) {
    return;
  }

  state->hold_count.fetch_sub(1);
  if (can_wake_waiter(state->waiter_env)) {
    co_cond_signal(state->cond);
  }
}

CoroutineLockGuard::CoroutineLockGuard(CoroutineMutex* mutex) : mutex_(mutex)
{
  if (mutex_) {
    mutex_->lock();
  }
}

CoroutineLockGuard::~CoroutineLockGuard()
{
  if (mutex_) {
    mutex_->unlock();
  }
}

CoroutineCondition::CoroutineCondition() : impl_(nullptr)
{
  ConditionState* state = new ConditionState();
  state->cond = co_cond_alloc();
  state->waiter_env.store(nullptr);
  if (!state->cond) {
    delete state;
    throw std::runtime_error("Failed to allocate coroutine condition variable");
  }
  impl_ = state;
}

CoroutineCondition::~CoroutineCondition()
{
  ConditionState* state = static_cast<ConditionState*>(impl_);
  if (state) {
    co_cond_free(state->cond);
    delete state;
    impl_ = nullptr;
  }
}

CoroutineCondition::CoroutineCondition(CoroutineCondition&& other) noexcept : impl_(other.impl_)
{
  other.impl_ = nullptr;
}

CoroutineCondition& CoroutineCondition::operator=(CoroutineCondition&& other) noexcept
{
  if (this != &other) {
    ConditionState* state = static_cast<ConditionState*>(impl_);
    if (state) {
      co_cond_free(state->cond);
      delete state;
    }
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

int CoroutineCondition::wait(int timeout_ms)
{
  ConditionState* state = static_cast<ConditionState*>(impl_);
  if (!state) {
    errno = EINVAL;
    return -1;
  }

  if (!is_coroutine_context()) {
    // There is no coroutine to suspend here. A bounded wait still honours its
    // timeout by sleeping, which matches what libco reports for a timed out
    // wait; an unbounded one would never be woken up.
    if (timeout_ms > 0) {
      poll_without_hook(nullptr, 0, timeout_ms);
      return 0;
    }
    errno = EPERM;
    return -1;
  }

  state->waiter_env.store(co_get_curr_thread_env());
  return co_cond_timedwait(state->cond, timeout_ms);
}

bool CoroutineCondition::can_wake() const
{
  const ConditionState* state = static_cast<const ConditionState*>(impl_);
  return state && can_wake_waiter(state->waiter_env);
}

void CoroutineCondition::signal()
{
  if (can_wake()) {
    co_cond_signal(static_cast<ConditionState*>(impl_)->cond);
  }
}

void CoroutineCondition::broadcast()
{
  if (can_wake()) {
    co_cond_broadcast(static_cast<ConditionState*>(impl_)->cond);
  }
}

Runtime& Runtime::instance()
{
  static Runtime runtime;
  return runtime;
}

void Runtime::enable_async_syscalls()
{
  co_enable_hook_sys();
}

void Runtime::disable_async_syscalls()
{
  co_disable_hook_sys();
}

TaskHandle Runtime::spawn(TaskEntry entry, void* arg, size_t stack_size)
{
  if (!entry) {
    return TaskHandle();
  }

  reap_finished_tasks();

  stCoRoutine_t* task = nullptr;
  stCoRoutineAttr_t attr;
  std::memset(&attr, 0, sizeof(attr));
  stCoRoutineAttr_t* attr_ptr = nullptr;
  if (stack_size > 0) {
    attr.stack_size = static_cast<int>(stack_size);
    attr_ptr = &attr;
  }

  TaskState* state = new TaskState(entry, arg);
  int ret = co_create(&task, attr_ptr, task_trampoline, state);
  if (ret != 0 || !task) {
    delete state;
    return TaskHandle();
  }

  state->task = task;
  state->owner_env = task->env;

  // The entry runs until its first yield point before co_resume() returns, so
  // the lifecycle state has to be fully initialized before this point.
  co_resume(task);

  return TaskHandle(state);
}

int Runtime::run_event_loop(EventLoopCallback callback, void* arg)
{
  EventLoopContext ctx = {callback, arg};
  co_eventloop(co_get_epoll_ct(), eventloop_trampoline, &ctx);
  reap_finished_tasks();
  return 0;
}

int Runtime::reap_tasks()
{
  reap_finished_tasks();
  return 0;
}

int Runtime::wait(int fd, short events, int timeout_ms)
{
  struct pollfd pf;
  std::memset(&pf, 0, sizeof(pf));
  pf.fd = fd;
  pf.events = events;

  // libco's co_poll() dereferences a null poll function for a zero timeout and
  // cannot suspend outside a coroutine, so both cases go through a real poll().
  if (timeout_ms == 0 || !is_coroutine_context()) {
    return poll_without_hook(&pf, 1, timeout_ms);
  }

  return co_poll(co_get_epoll_ct(), &pf, 1, timeout_ms);
}

int Runtime::wait_readable(int fd, int timeout_ms)
{
  return wait(fd, POLLIN | POLLERR | POLLHUP, timeout_ms);
}

int Runtime::wait_writable(int fd, int timeout_ms)
{
  return wait(fd, POLLOUT | POLLERR | POLLHUP, timeout_ms);
}

int Runtime::accept(int fd, struct sockaddr* addr, socklen_t* len)
{
  return co_accept(fd, addr, len);
}

int Runtime::create_task_local_key(TaskLocalKey* key)
{
  if (!key) {
    errno = EINVAL;
    return -1;
  }

  pthread_key_t native_key = 0;
  int ret = pthread_key_create(&native_key, nullptr);
  if (ret != 0) {
    errno = ret;
    return -1;
  }

  // libco indexes a fixed-size per-coroutine array with the raw key value and
  // never validates it, so a key beyond that array is unusable here.
  if (native_key >= kMaxTaskLocalKeys) {
    pthread_key_delete(native_key);
    errno = EINVAL;
    return -1;
  }

  key->slot_ = static_cast<unsigned int>(native_key);
  key->valid_ = true;
  return 0;
}

void* Runtime::get_task_local(const TaskLocalKey& key)
{
  if (!key.is_valid()) {
    return nullptr;
  }
  return co_getspecific(static_cast<pthread_key_t>(key.slot_));
}

int Runtime::set_task_local(const TaskLocalKey& key, const void* value)
{
  if (!key.is_valid()) {
    errno = EINVAL;
    return -1;
  }
  return co_setspecific(static_cast<pthread_key_t>(key.slot_), value);
}

Runtime& current_runtime()
{
  return Runtime::instance();
}

}  // namespace runtime
}  // namespace mqtt
