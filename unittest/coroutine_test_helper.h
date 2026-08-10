#ifndef COROUTINE_TEST_HELPER_H
#define COROUTINE_TEST_HELPER_H

#include "mqtt_runtime.h"

namespace mqtt {
namespace test {

/**
 * @brief RAII包装器，用于在测试中安全地使用协程环境。
 *
 * 构造时开启当前任务的异步系统调用，并跑一个空任务确认运行时可用；析构时把
 * 开关恢复并回收本线程遗留的任务。
 */
class CoroutineTestScope
{
 public:
  CoroutineTestScope() : available_(false)
  {
    runtime::current_runtime().enable_async_syscalls();

    runtime::TaskHandle probe = runtime::current_runtime().spawn(&probe_task, nullptr);
    available_ = probe.is_valid() && probe.is_finished();
  }

  ~CoroutineTestScope()
  {
    runtime::current_runtime().reap_tasks();
    runtime::current_runtime().disable_async_syscalls();
  }

  CoroutineTestScope(const CoroutineTestScope&) = delete;
  CoroutineTestScope& operator=(const CoroutineTestScope&) = delete;

  bool is_available() const { return available_; }

 private:
  static void* probe_task(void* /*arg*/) { return nullptr; }

  bool available_;
};

}  // namespace test
}  // namespace mqtt

#endif  // COROUTINE_TEST_HELPER_H
