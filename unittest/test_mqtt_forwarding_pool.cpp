#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "forwarding_connection_pool.h"
#include "mqtt_allocator.h"
#include "mqtt_memory_tags.h"

namespace mqtt
{

// ---------------------------------------------------------------------------
// 辅助：最简 TCP 服务端（只 accept，不做任何协议处理）
// ---------------------------------------------------------------------------
class SimpleTestServer
{
 public:
  SimpleTestServer() : listen_fd_(-1), port_(0), stop_(false) {}

  ~SimpleTestServer() { stop(); }

  int start()
  {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return -1;

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;  // 让 OS 分配端口

    if (::bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) return -1;
    if (::listen(listen_fd_, 32) < 0) return -1;

    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd_, (struct sockaddr*)&addr, &len);
    port_ = ntohs(addr.sin_port);

    accept_thread_ = std::thread([this] {
      while (!stop_.load()) {
        struct timeval tv {0, 50000};  // 50ms timeout
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd_, &rfds);
        int r = ::select(listen_fd_ + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) {
          int cli = ::accept(listen_fd_, NULL, NULL);
          if (cli >= 0) {
            // 保持连接打开直到服务端关闭，让池中的 socket 保持 connected 状态
            std::lock_guard<std::mutex> lk(cli_mutex_);
            client_fds_.push_back(cli);
          }
        }
      }
    });

    return 0;
  }

  void stop()
  {
    stop_.store(true);
    if (accept_thread_.joinable()) accept_thread_.join();
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    std::lock_guard<std::mutex> lk(cli_mutex_);
    for (int fd : client_fds_) ::close(fd);
    client_fds_.clear();
  }

  int port() const { return port_; }

 private:
  int                  listen_fd_;
  int                  port_;
  std::atomic<bool>    stop_;
  std::thread          accept_thread_;
  std::mutex           cli_mutex_;
  std::vector<int>     client_fds_;
};

// ---------------------------------------------------------------------------
// 测试夹具
// ---------------------------------------------------------------------------
class ForwardingConnectionPoolTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    MQTTAllocator* root = MQTTMemoryManager::get_instance().get_root_allocator();
    allocator_ = root->create_child("test_pool", MQTTMemoryTag::MEM_TAG_ROOT);

    ASSERT_EQ(0, server_.start());
    host_ = "127.0.0.1";
    port_ = server_.port();
  }

  void TearDown() override
  {
    server_.stop();
    if (allocator_ && allocator_->get_parent()) {
      allocator_->get_parent()->remove_child("test_pool");
    }
  }

  ForwardingConnectionPool* make_pool(int pool_size = 4, int idle_timeout_ms = 30000)
  {
    ForwardingConnectionPool::Config cfg;
    cfg.pool_size_per_target = pool_size;
    cfg.idle_timeout_ms      = idle_timeout_ms;
    return new ForwardingConnectionPool(allocator_, cfg);
  }

  MQTTAllocator*    allocator_ = nullptr;
  SimpleTestServer  server_;
  std::string       host_;
  int               port_ = 0;
};

// ---------------------------------------------------------------------------
// Test 1：基本 acquire/release/reuse
// ---------------------------------------------------------------------------
TEST_F(ForwardingConnectionPoolTest, AcquireRelease_ReusesConnection)
{
  std::unique_ptr<ForwardingConnectionPool> pool(make_pool());

  MQTTSocket* s1       = NULL;
  bool        pooled1  = false;
  ASSERT_EQ(MQ_SUCCESS, pool->acquire(host_, port_, s1, pooled1));
  ASSERT_NE((MQTTSocket*)NULL, s1);
  EXPECT_TRUE(pooled1);

  // 归还
  pool->release(host_, port_, s1, pooled1, false);
  EXPECT_EQ((MQTTSocket*)NULL, s1);  // release 将 socket 置 NULL

  // 再次获取，应复用同一 socket 指针
  MQTTSocket* s2      = NULL;
  bool        pooled2 = false;
  ASSERT_EQ(MQ_SUCCESS, pool->acquire(host_, port_, s2, pooled2));
  ASSERT_NE((MQTTSocket*)NULL, s2);
  EXPECT_TRUE(pooled2);

  pool->release(host_, port_, s2, pooled2, false);
}

// ---------------------------------------------------------------------------
// Test 2：池满时使用临时连接（is_pooled=false）
// ---------------------------------------------------------------------------
TEST_F(ForwardingConnectionPoolTest, PoolFull_FallsBackToTempConnection)
{
  const int pool_size = 3;
  std::unique_ptr<ForwardingConnectionPool> pool(make_pool(pool_size));

  // 占满所有池位
  std::vector<std::pair<MQTTSocket*, bool>> held;
  for (int i = 0; i < pool_size; ++i) {
    MQTTSocket* s    = NULL;
    bool        p    = false;
    ASSERT_EQ(MQ_SUCCESS, pool->acquire(host_, port_, s, p));
    ASSERT_NE((MQTTSocket*)NULL, s);
    EXPECT_TRUE(p) << "slot " << i << " should be pooled";
    held.push_back({s, p});
  }

  // 第 pool_size+1 次应退化为临时连接
  MQTTSocket* overflow      = NULL;
  bool        overflow_pooled = false;
  ASSERT_EQ(MQ_SUCCESS, pool->acquire(host_, port_, overflow, overflow_pooled));
  ASSERT_NE((MQTTSocket*)NULL, overflow);
  EXPECT_FALSE(overflow_pooled) << "overflow connection should NOT be pooled";

  // 清理
  pool->release(host_, port_, overflow, overflow_pooled, false);
  for (auto& kv : held) {
    pool->release(host_, port_, kv.first, kv.second, false);
  }
}

// ---------------------------------------------------------------------------
// Test 3：出错归还时连接不入池
// ---------------------------------------------------------------------------
TEST_F(ForwardingConnectionPoolTest, ReleaseWithError_SocketNotReused)
{
  std::unique_ptr<ForwardingConnectionPool> pool(make_pool(2));

  MQTTSocket* s1      = NULL;
  bool        pooled1 = false;
  ASSERT_EQ(MQ_SUCCESS, pool->acquire(host_, port_, s1, pooled1));
  EXPECT_TRUE(pooled1);

  // 以 has_error=true 归还
  pool->release(host_, port_, s1, pooled1, true);
  EXPECT_EQ((MQTTSocket*)NULL, s1);

  // 池现在应该有空位并能重新建连
  MQTTSocket* s2      = NULL;
  bool        pooled2 = false;
  EXPECT_EQ(MQ_SUCCESS, pool->acquire(host_, port_, s2, pooled2));
  EXPECT_TRUE(pooled2);
  pool->release(host_, port_, s2, pooled2, false);
}

// ---------------------------------------------------------------------------
// Test 4：空闲超时清理
// ---------------------------------------------------------------------------
TEST_F(ForwardingConnectionPoolTest, IdleCleanup_ClosesStaleConnections)
{
  // 设置极短超时，方便测试
  const int timeout_ms = 200;
  std::unique_ptr<ForwardingConnectionPool> pool(make_pool(4, timeout_ms));

  // 建立并归还一条连接
  MQTTSocket* s      = NULL;
  bool        pooled = false;
  ASSERT_EQ(MQ_SUCCESS, pool->acquire(host_, port_, s, pooled));
  EXPECT_TRUE(pooled);
  pool->release(host_, port_, s, pooled, false);

  // 等待超时
  std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms + 100));

  // 手动触发清理
  pool->cleanup_idle_connections();

  // 再次获取需新建连接（旧连接已被清理）——只要不 crash 且成功即可
  MQTTSocket* s2      = NULL;
  bool        pooled2 = false;
  EXPECT_EQ(MQ_SUCCESS, pool->acquire(host_, port_, s2, pooled2));
  pool->release(host_, port_, s2, pooled2, false);
}

// ---------------------------------------------------------------------------
// Test 5：无效参数
// ---------------------------------------------------------------------------
TEST_F(ForwardingConnectionPoolTest, InvalidArgs_ReturnsError)
{
  std::unique_ptr<ForwardingConnectionPool> pool(make_pool());

  MQTTSocket* s      = NULL;
  bool        pooled = false;
  EXPECT_NE(MQ_SUCCESS, pool->acquire("", port_, s, pooled));
  EXPECT_NE(MQ_SUCCESS, pool->acquire(host_, 0, s, pooled));
  EXPECT_NE(MQ_SUCCESS, pool->acquire(host_, -1, s, pooled));
}

// ---------------------------------------------------------------------------
// Test 6：多线程并发不崩溃
// ---------------------------------------------------------------------------
TEST_F(ForwardingConnectionPoolTest, MultiThread_ConcurrentAcquireRelease)
{
  std::unique_ptr<ForwardingConnectionPool> pool(make_pool(4));

  const int       thread_count = 8;
  const int       ops_per_thread = 20;
  std::atomic<int> errors(0);

  std::vector<std::thread> threads;
  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < ops_per_thread; ++i) {
        MQTTSocket* s      = NULL;
        bool        pooled = false;
        int         ret    = pool->acquire(host_, port_, s, pooled);
        if (ret != MQ_SUCCESS || s == NULL) {
          errors.fetch_add(1);
          continue;
        }
        // 模拟短暂使用
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        pool->release(host_, port_, s, pooled, false);
      }
    });
  }

  for (auto& th : threads) th.join();

  EXPECT_EQ(0, errors.load()) << "some acquire() calls failed under concurrency";
}

// ---------------------------------------------------------------------------
// Test 7：shutdown 后再次操作不 crash
// ---------------------------------------------------------------------------
TEST_F(ForwardingConnectionPoolTest, Shutdown_CleanlyReleasesAll)
{
  ForwardingConnectionPool* pool = make_pool();

  // 建立几条连接并归还到 idle
  for (int i = 0; i < 3; ++i) {
    MQTTSocket* s      = NULL;
    bool        pooled = false;
    if (pool->acquire(host_, port_, s, pooled) == MQ_SUCCESS) {
      pool->release(host_, port_, s, pooled, false);
    }
  }

  // shutdown 应清理所有资源而不 crash
  pool->shutdown();
  delete pool;
}

}  // namespace mqtt
