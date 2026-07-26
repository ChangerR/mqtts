#include "mqtt_runtime.h"

#include <cerrno>
#include <gtest/gtest.h>
#include <unistd.h>

namespace {

struct TaskContext
{
  int counter;
};

struct WaitContext
{
  int fd;
  int result;
  bool done;
};

void* increment_task(void* arg)
{
  TaskContext* ctx = static_cast<TaskContext*>(arg);
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

int stop_when_wait_done(void* arg)
{
  WaitContext* ctx = static_cast<WaitContext*>(arg);
  return ctx->done ? -1 : 0;
}

class RuntimeTest : public ::testing::Test
{
 protected:
  void SetUp() override { mqtt::runtime::current_runtime().enable_hook(); }
};

TEST_F(RuntimeTest, SpawnRunsTaskAndTracksFinishedState)
{
  TaskContext ctx = {0};
  mqtt::runtime::TaskHandle task =
      mqtt::runtime::current_runtime().spawn(increment_task, &ctx, 64 * 1024);

  ASSERT_TRUE(task.is_valid());
  EXPECT_EQ(1, ctx.counter);
  EXPECT_TRUE(task.is_finished());
  EXPECT_EQ(0, task.join(0));
}

TEST_F(RuntimeTest, ConditionFacadeCanSignalAndBroadcast)
{
  mqtt::runtime::AsyncCondition condition;
  EXPECT_TRUE(condition.is_valid());
  condition.signal();
  condition.broadcast();
}

TEST_F(RuntimeTest, ZeroTimeoutUsesNonBlockingPollOutsideCoroutine)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));

  int wait_ret = mqtt::runtime::current_runtime().io_waiter().wait_readable(fds[0], 0);
  EXPECT_EQ(0, wait_ret);

  close(fds[0]);
  close(fds[1]);
}

TEST_F(RuntimeTest, BlockingWaitOutsideCoroutineReturnsError)
{
  int fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(fds));

  errno = 0;
  int wait_ret = mqtt::runtime::current_runtime().io_waiter().wait_readable(fds[0], 1);
  EXPECT_EQ(-1, wait_ret);
  EXPECT_EQ(EINVAL, errno);

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

  mqtt::runtime::current_runtime().run_event_loop(stop_when_wait_done, &ctx);
  EXPECT_TRUE(ctx.done);
  EXPECT_EQ(0, ctx.result);

  close(fds[0]);
  close(fds[1]);
}

}  // namespace

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
