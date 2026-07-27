#include "mqtt_runtime.h"

#include <cerrno>
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct CounterContext
{
  int counter;
};

struct WaitContext
{
  int fd;
  int result;
  bool done;
};

struct MutexContext
{
  mqtt::runtime::AsyncMutex* mutex;
  std::string* order;
  const char* label;
  int hold_ms;
  bool done;
};

struct ConditionContext
{
  mqtt::runtime::AsyncCondition* condition;
  int wait_result;
  bool woken;
};

struct JoinAttemptContext
{
  mqtt::runtime::TaskHandle* target;
  int join_result;
  int join_errno;
  bool done;
};

struct ThreadTaskResult
{
  bool spawned;
  bool finished_after_join;
  int join_result;
  int counter;
};

void* increment_task(void* arg)
{
  CounterContext* ctx = static_cast<CounterContext*>(arg);
  ++ctx->counter;
  return nullptr;
}

void* wait_readable_task(void* arg)
{
  WaitContext* ctx = static_cast<WaitContext*>(arg);
  ctx->result = mqtt::runtime::current_runtime().io_waiter().wait_readable(ctx->fd, 10);
  ctx->done = true;
  return nullptr;
}

void* long_wait_task(void* arg)
{
  WaitContext* ctx = static_cast<WaitContext*>(arg);
  ctx->result = mqtt::runtime::current_runtime().wait_readable(ctx->fd, 60000);
  ctx->done = true;
  return nullptr;
}

void* mutex_task(void* arg)
{
  MutexContext* ctx = static_cast<MutexContext*>(arg);
  ctx->mutex->lock();
  ctx->order->append(ctx->label);
  if (ctx->hold_ms > 0) {
    // A negative fd makes this a pure timeout, so the lock is held across a yield.
    mqtt::runtime::current_runtime().wait_readable(-1, ctx->hold_ms);
  }
  ctx->mutex->unlock();
  ctx->done = true;
  return nullptr;
}

void* condition_wait_task(void* arg)
{
  ConditionContext* ctx = static_cast<ConditionContext*>(arg);
  ctx->wait_result = ctx->condition->wait(5000);
  ctx->woken = true;
  return nullptr;
}

void* join_attempt_task(void* arg)
{
  JoinAttemptContext* ctx = static_cast<JoinAttemptContext*>(arg);
  errno = 0;
  ctx->join_result = ctx->target->join(100);
  ctx->join_errno = errno;
  ctx->done = true;
  return nullptr;
}

int stop_when_wait_done(void* arg)
{
  WaitContext* ctx = static_cast<WaitContext*>(arg);
  return ctx->done ? -1 : 0;
}

int stop_when_both_done(void* arg)
{
  MutexContext* contexts = static_cast<MutexContext*>(arg);
  return (contexts[0].done && contexts[1].done) ? -1 : 0;
}

int stop_when_woken(void* arg)
{
  ConditionContext* ctx = static_cast<ConditionContext*>(arg);
  ctx->condition->signal();
  return ctx->woken ? -1 : 0;
}

// A task suspended on a pipe keeps pointing at the caller's stack, so it has to
// be woken and drained before that frame goes away.
void drain_pipe_waiter(int write_fd, WaitContext* ctx)
{
  if (!ctx->done) {
    ASSERT_EQ(1, write(write_fd, "x", 1));
    mqtt::runtime::current_runtime().run_event_loop(stop_when_wait_done, ctx);
  }
  ASSERT_TRUE(ctx->done);
}

class RuntimeTest : public ::testing::Test
{
 protected:
  void SetUp() override { mqtt::runtime::current_runtime().enable_hook(); }
};

TEST_F(RuntimeTest, SpawnRunsTaskAndTracksFinishedState)
{
  CounterContext ctx = {0};
  mqtt::runtime::TaskHandle task =
      mqtt::runtime::current_runtime().spawn(increment_task, &ctx, 64 * 1024);

  ASSERT_TRUE(task.is_valid());
  EXPECT_EQ(1, ctx.counter);
  EXPECT_TRUE(task.is_finished());
  EXPECT_EQ(0, task.join(0));
}

TEST_F(RuntimeTest, SpawnRejectsNullEntry)
{
  mqtt::runtime::TaskHandle task = mqtt::runtime::current_runtime().spawn(nullptr, nullptr);
  EXPECT_FALSE(task.is_valid());
  EXPECT_TRUE(task.is_finished());
}

TEST_F(RuntimeTest, MovedHandleTransfersOwnership)
{
  CounterContext ctx = {0};
  mqtt::runtime::TaskHandle task =
      mqtt::runtime::current_runtime().spawn(increment_task, &ctx, 64 * 1024);
  ASSERT_TRUE(task.is_valid());

  mqtt::runtime::TaskHandle moved(std::move(task));
  EXPECT_FALSE(task.is_valid());
  EXPECT_TRUE(moved.is_valid());
  EXPECT_TRUE(moved.is_finished());

  mqtt::runtime::TaskHandle assigned;
  assigned = std::move(moved);
  EXPECT_FALSE(moved.is_valid());
  EXPECT_TRUE(assigned.is_valid());

  assigned.release();
  EXPECT_FALSE(assigned.is_valid());
  // Releasing twice must stay a no-op.
  assigned.release();
  EXPECT_FALSE(assigned.is_valid());
}

TEST_F(RuntimeTest, ZeroTimeoutUsesNonBlockingPollOutsideCoroutine)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));

  EXPECT_EQ(0, mqtt::runtime::current_runtime().io_waiter().wait_readable(fds[0], 0));

  ASSERT_EQ(1, write(fds[1], "x", 1));
  EXPECT_EQ(1, mqtt::runtime::current_runtime().io_waiter().wait_readable(fds[0], 0));

  close(fds[0]);
  close(fds[1]);
}

TEST_F(RuntimeTest, BlockingWaitOutsideCoroutineFallsBackToRealPoll)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));

  // Without a coroutine to suspend, the wait must still block for its timeout
  // rather than fail, otherwise the socket layer's retry loops would spin.
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  int wait_ret = mqtt::runtime::current_runtime().io_waiter().wait_readable(fds[0], 50);
  int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  EXPECT_EQ(0, wait_ret);
  EXPECT_GE(elapsed_ms, 40);

  close(fds[0]);
  close(fds[1]);
}

TEST_F(RuntimeTest, CoroutineWaitCanJoinAfterTimeout)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));

  WaitContext ctx = {fds[0], -1, false};
  mqtt::runtime::TaskHandle task =
      mqtt::runtime::current_runtime().spawn(wait_readable_task, &ctx, 64 * 1024);

  ASSERT_TRUE(task.is_valid());
  EXPECT_FALSE(task.is_finished());
  EXPECT_EQ(0, task.join(1000));
  EXPECT_TRUE(ctx.done);
  EXPECT_EQ(0, ctx.result);
  EXPECT_TRUE(task.is_finished());

  close(fds[0]);
  close(fds[1]);
}

TEST_F(RuntimeTest, JoinReportsBusyAndTimeoutForRunningTask)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));

  WaitContext ctx = {fds[0], -1, false};
  mqtt::runtime::TaskHandle task =
      mqtt::runtime::current_runtime().spawn(long_wait_task, &ctx, 64 * 1024);
  ASSERT_TRUE(task.is_valid());
  ASSERT_FALSE(task.is_finished());

  errno = 0;
  EXPECT_EQ(-1, task.join(0));
  EXPECT_EQ(EBUSY, errno);

  errno = 0;
  EXPECT_EQ(-1, task.join(50));
  EXPECT_EQ(ETIMEDOUT, errno);
  EXPECT_FALSE(ctx.done);

  drain_pipe_waiter(fds[1], &ctx);
  EXPECT_TRUE(task.is_finished());

  close(fds[0]);
  close(fds[1]);
}

TEST_F(RuntimeTest, JoinFromCoroutineIsRejected)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));

  WaitContext ctx = {fds[0], -1, false};
  mqtt::runtime::TaskHandle target =
      mqtt::runtime::current_runtime().spawn(long_wait_task, &ctx, 64 * 1024);
  ASSERT_TRUE(target.is_valid());

  JoinAttemptContext join_ctx = {&target, 0, 0, false};
  mqtt::runtime::TaskHandle joiner =
      mqtt::runtime::current_runtime().spawn(join_attempt_task, &join_ctx, 64 * 1024);
  ASSERT_TRUE(joiner.is_valid());

  // The rejection is immediate: a nested event loop would starve the outer one.
  EXPECT_TRUE(join_ctx.done);
  EXPECT_EQ(-1, join_ctx.join_result);
  EXPECT_EQ(EPERM, join_ctx.join_errno);
  EXPECT_FALSE(ctx.done);

  drain_pipe_waiter(fds[1], &ctx);

  close(fds[0]);
  close(fds[1]);
}

TEST_F(RuntimeTest, ReleasingSuspendedTaskDetachesWithoutUseAfterFree)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));

  WaitContext ctx = {fds[0], -1, false};
  mqtt::runtime::TaskHandle task =
      mqtt::runtime::current_runtime().spawn(wait_readable_task, &ctx, 64 * 1024);

  ASSERT_TRUE(task.is_valid());
  EXPECT_FALSE(task.is_finished());
  task.release();
  EXPECT_FALSE(task.is_valid());

  // The detached task must still be resumable; reclaiming it is the event loop's
  // job once it has finished.
  mqtt::runtime::current_runtime().run_event_loop(stop_when_wait_done, &ctx);
  EXPECT_TRUE(ctx.done);
  EXPECT_EQ(0, ctx.result);

  close(fds[0]);
  close(fds[1]);
}

TEST_F(RuntimeTest, DetachedTasksAreReclaimedRepeatedly)
{
  // Detaching tasks repeatedly must not accumulate state: every round is
  // reclaimed by the owning thread before the next one is spawned.
  for (int round = 0; round < 50; ++round) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(0, pipe(fds));

    WaitContext ctx = {fds[0], -1, false};
    mqtt::runtime::TaskHandle task =
        mqtt::runtime::current_runtime().spawn(wait_readable_task, &ctx, 64 * 1024);
    ASSERT_TRUE(task.is_valid());
    task.release();

    mqtt::runtime::current_runtime().run_event_loop(stop_when_wait_done, &ctx);
    ASSERT_TRUE(ctx.done);

    close(fds[0]);
    close(fds[1]);
  }

  EXPECT_EQ(0, mqtt::runtime::current_runtime().reap_tasks());
}

TEST_F(RuntimeTest, MutexSerializesContendingCoroutines)
{
  mqtt::runtime::AsyncMutex mutex;
  std::string order;
  MutexContext contexts[2] = {{&mutex, &order, "a", 50, false},
                              {&mutex, &order, "b", 0, false}};

  mqtt::runtime::TaskHandle first =
      mqtt::runtime::current_runtime().spawn(mutex_task, &contexts[0], 64 * 1024);
  mqtt::runtime::TaskHandle second =
      mqtt::runtime::current_runtime().spawn(mutex_task, &contexts[1], 64 * 1024);
  ASSERT_TRUE(first.is_valid());
  ASSERT_TRUE(second.is_valid());

  // The first task holds the lock across a yield, so the second one must queue.
  EXPECT_EQ("a", order);
  EXPECT_FALSE(contexts[1].done);

  mqtt::runtime::current_runtime().run_event_loop(stop_when_both_done, contexts);
  EXPECT_EQ("ab", order);
  EXPECT_TRUE(contexts[0].done);
  EXPECT_TRUE(contexts[1].done);
}

TEST_F(RuntimeTest, ConditionWakesWaitingCoroutine)
{
  mqtt::runtime::AsyncCondition condition;
  ASSERT_TRUE(condition.is_valid());

  ConditionContext ctx = {&condition, -1, false};
  mqtt::runtime::TaskHandle task =
      mqtt::runtime::current_runtime().spawn(condition_wait_task, &ctx, 64 * 1024);
  ASSERT_TRUE(task.is_valid());
  ASSERT_FALSE(ctx.woken);

  mqtt::runtime::current_runtime().run_event_loop(stop_when_woken, &ctx);
  EXPECT_TRUE(ctx.woken);
  EXPECT_EQ(0, ctx.wait_result);
  EXPECT_TRUE(task.is_finished());
}

TEST_F(RuntimeTest, ConditionWaitOutsideCoroutineHonoursTimeout)
{
  mqtt::runtime::AsyncCondition condition;

  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  EXPECT_EQ(0, condition.wait(50));
  int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  EXPECT_GE(elapsed_ms, 40);

  errno = 0;
  EXPECT_EQ(-1, condition.wait(-1));
  EXPECT_EQ(EPERM, errno);
}

TEST_F(RuntimeTest, TaskLocalStorageRejectsOutOfRangeKeys)
{
  errno = 0;
  EXPECT_EQ(-1, mqtt::runtime::current_runtime().set_specific(mqtt::runtime::kMaxTaskLocalKeys, ""));
  EXPECT_EQ(EINVAL, errno);
  EXPECT_EQ(nullptr,
            mqtt::runtime::current_runtime().get_specific(mqtt::runtime::kMaxTaskLocalKeys));
}

TEST_F(RuntimeTest, TasksOwnedByEachThreadRunIndependently)
{
  const int kThreadCount = 4;
  std::vector<ThreadTaskResult> results(kThreadCount);
  std::vector<std::thread> threads;

  for (int i = 0; i < kThreadCount; ++i) {
    ThreadTaskResult* result = &results[i];
    threads.push_back(std::thread([result]() {
      mqtt::runtime::current_runtime().enable_hook();

      int fds[2] = {-1, -1};
      if (pipe(fds) != 0) {
        result->spawned = false;
        return;
      }

      WaitContext wait_ctx = {fds[0], -1, false};
      mqtt::runtime::TaskHandle task =
          mqtt::runtime::current_runtime().spawn(wait_readable_task, &wait_ctx, 64 * 1024);
      result->spawned = task.is_valid();
      result->join_result = task.join(2000);
      result->finished_after_join = task.is_finished();

      CounterContext counter_ctx = {0};
      mqtt::runtime::TaskHandle counter_task =
          mqtt::runtime::current_runtime().spawn(increment_task, &counter_ctx, 64 * 1024);
      counter_task.release();
      result->counter = counter_ctx.counter;

      close(fds[0]);
      close(fds[1]);
    }));
  }

  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].join();
  }

  for (int i = 0; i < kThreadCount; ++i) {
    EXPECT_TRUE(results[i].spawned) << "thread " << i;
    EXPECT_EQ(0, results[i].join_result) << "thread " << i;
    EXPECT_TRUE(results[i].finished_after_join) << "thread " << i;
    EXPECT_EQ(1, results[i].counter) << "thread " << i;
  }
}

// Waking a coroutine from another thread would queue it on that thread's run
// list, so the wakeup has to be dropped instead.
TEST_F(RuntimeTest, ForeignThreadWakeupsAreDropped)
{
  mqtt::runtime::AsyncMutex mutex;
  std::string order;
  MutexContext contexts[2] = {{&mutex, &order, "a", 50, false},
                              {&mutex, &order, "b", 0, false}};

  mqtt::runtime::TaskHandle first =
      mqtt::runtime::current_runtime().spawn(mutex_task, &contexts[0], 64 * 1024);
  mqtt::runtime::TaskHandle second =
      mqtt::runtime::current_runtime().spawn(mutex_task, &contexts[1], 64 * 1024);
  ASSERT_TRUE(first.is_valid());
  ASSERT_TRUE(second.is_valid());
  ASSERT_EQ("a", order);

  // The second task is queued on this thread's mutex; a foreign unlock/broadcast
  // must leave it there instead of resuming it.
  mqtt::runtime::AsyncCondition condition;
  ConditionContext condition_ctx = {&condition, -1, false};
  mqtt::runtime::TaskHandle waiter =
      mqtt::runtime::current_runtime().spawn(condition_wait_task, &condition_ctx, 64 * 1024);
  ASSERT_TRUE(waiter.is_valid());
  ASSERT_FALSE(condition_ctx.woken);

  std::thread foreign([&mutex, &condition]() {
    mutex.unlock();
    condition.broadcast();
    condition.signal();
  });
  foreign.join();

  EXPECT_FALSE(contexts[1].done);
  EXPECT_FALSE(condition_ctx.woken);
  EXPECT_EQ("a", order);

  // Undo the foreign unlock so the owning thread can still drain both tasks.
  mutex.lock();
  mqtt::runtime::current_runtime().run_event_loop(stop_when_both_done, contexts);
  EXPECT_EQ("ab", order);
  mutex.unlock();

  mqtt::runtime::current_runtime().run_event_loop(stop_when_woken, &condition_ctx);
  EXPECT_TRUE(condition_ctx.woken);
}

// A libco coroutine may only be resumed or freed by the thread that created it,
// so a foreign join is rejected and a foreign release only drops the handle.
TEST_F(RuntimeTest, ForeignThreadCannotJoinOrReclaimTask)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));

  WaitContext ctx = {fds[0], -1, false};
  mqtt::runtime::TaskHandle task =
      mqtt::runtime::current_runtime().spawn(long_wait_task, &ctx, 64 * 1024);
  ASSERT_TRUE(task.is_valid());
  ASSERT_FALSE(task.is_finished());

  int foreign_join_result = 0;
  int foreign_join_errno = 0;
  std::thread foreign([&task, &foreign_join_result, &foreign_join_errno]() {
    errno = 0;
    foreign_join_result = task.join(100);
    foreign_join_errno = errno;
  });
  foreign.join();

  EXPECT_EQ(-1, foreign_join_result);
  EXPECT_EQ(EPERM, foreign_join_errno);
  EXPECT_FALSE(task.is_finished());

  std::thread releaser([&task]() { task.release(); });
  releaser.join();
  EXPECT_FALSE(task.is_valid());

  // The live coroutine survived the foreign release and still runs to completion
  // on its owning thread.
  ASSERT_EQ(1, write(fds[1], "x", 1));
  mqtt::runtime::current_runtime().run_event_loop(stop_when_wait_done, &ctx);
  EXPECT_TRUE(ctx.done);

  close(fds[0]);
  close(fds[1]);
}

}  // namespace

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
