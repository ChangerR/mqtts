#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_define.h"
#include "mqtt_socket.h"

namespace mqtt
{

/**
 * @brief 每个远端地址的连接池
 *
 * 维护最多 max_size 条持久 TCP 连接。acquire() 和 release() 开销极低
 * （只锁 bookkeeping，I/O 在锁外进行），可安全地被多个 OS 线程并发调用。
 */
class ForwardingConnectionPool
{
 public:
  struct Config {
    int pool_size_per_target = 4;      // 每个远端最多保持的连接数
    int idle_timeout_ms     = 30000;   // 空闲超过此值的连接将被关闭
  };

  explicit ForwardingConnectionPool(MQTTAllocator* allocator, const Config& config);
  ~ForwardingConnectionPool();

  // 禁止拷贝
  ForwardingConnectionPool(const ForwardingConnectionPool&) = delete;
  ForwardingConnectionPool& operator=(const ForwardingConnectionPool&) = delete;

  /**
   * @brief 获取一条到 host:port 的已连接 socket。
   *
   * 优先复用空闲连接；池未满时新建并纳入池管理（is_pooled=true）；
   * 池已满时新建临时连接（is_pooled=false）。
   * 连接操作在锁外进行，不阻塞其他调用者。
   *
   * @param is_pooled  出参：true 表示连接来自或将归还到池
   * @return MQ_SUCCESS 或错误码
   */
  int acquire(const std::string& host, int port, MQTTSocket*& socket, bool& is_pooled);

  /**
   * @brief 归还连接。
   *
   * is_pooled=true 且 has_error=false → 放回 idle 队列；
   * 其他情况 → 直接销毁。
   */
  void release(const std::string& host, int port,
               MQTTSocket*& socket, bool is_pooled, bool has_error);

  /**
   * @brief 手动触发空闲连接清理（通常由内部后台线程自动调用）。
   */
  void cleanup_idle_connections();

  /**
   * @brief 关闭所有连接并停止后台线程（析构时自动调用）。
   */
  void shutdown();

 private:
  struct PooledConn {
    MQTTSocket* socket      = nullptr;
    int64_t     last_used_ms = 0;
  };

  struct RemotePool {
    std::vector<PooledConn> idle;
    int                     in_use_count = 0;
    int                     max_size;
    std::mutex              mutex;

    explicit RemotePool(int max_sz) : max_size(max_sz) {}
  };

  RemotePool* get_or_create_pool(const std::string& key);
  RemotePool* get_pool(const std::string& key);
  void        destroy_socket_internal(MQTTSocket* socket);
  int64_t     now_ms() const;
  void        cleanup_thread_main();

  MQTTAllocator* allocator_;
  Config         config_;

  std::unordered_map<std::string, RemotePool*> pools_;
  std::mutex                                    pools_map_mutex_;

  std::thread             cleanup_thread_;
  std::atomic<bool>       stop_cleanup_;
  std::mutex              cleanup_cv_mutex_;
  std::condition_variable cleanup_cv_;
};

}  // namespace mqtt
