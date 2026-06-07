#include "forwarding_connection_pool.h"
#include <chrono>
#include "logger.h"

namespace mqtt
{

ForwardingConnectionPool::ForwardingConnectionPool(MQTTAllocator* allocator, const Config& config)
    : allocator_(allocator), config_(config), stop_cleanup_(false)
{
  cleanup_thread_ = std::thread(&ForwardingConnectionPool::cleanup_thread_main, this);
}

ForwardingConnectionPool::~ForwardingConnectionPool()
{
  shutdown();
}

void ForwardingConnectionPool::shutdown()
{
  {
    std::lock_guard<std::mutex> lk(cleanup_cv_mutex_);
    stop_cleanup_.store(true);
  }
  cleanup_cv_.notify_all();
  if (cleanup_thread_.joinable()) {
    cleanup_thread_.join();
  }

  // 销毁所有剩余 idle 连接及 RemotePool 对象
  std::lock_guard<std::mutex> map_lk(pools_map_mutex_);
  for (auto& kv : pools_) {
    RemotePool* pool = kv.second;
    {
      std::lock_guard<std::mutex> pool_lk(pool->mutex);
      for (auto& conn : pool->idle) {
        destroy_socket_internal(conn.socket);
      }
      pool->idle.clear();
    }
    delete pool;
  }
  pools_.clear();
}

// ---------------------------------------------------------------------------
// acquire
// ---------------------------------------------------------------------------

int ForwardingConnectionPool::acquire(const std::string& host,
                                      int                port,
                                      MQTTSocket*&       socket,
                                      bool&              is_pooled)
{
  socket    = NULL;
  is_pooled = false;

  if (host.empty() || port <= 0) {
    return MQ_ERR_FORWARD_TARGET_INVALID;
  }

  const std::string key = host + ":" + std::to_string(port);
  RemotePool*       pool = get_or_create_pool(key);

  bool need_connect = false;  // true = 新建连接并纳入池（slot 已被 in_use_count 占位）
  {
    std::lock_guard<std::mutex> lk(pool->mutex);

    // 1. 尝试从 idle 队列取一条健康连接
    while (!pool->idle.empty()) {
      PooledConn conn = pool->idle.back();
      pool->idle.pop_back();
      if (conn.socket != NULL && conn.socket->is_connected()) {
        pool->in_use_count++;
        is_pooled = true;
        socket    = conn.socket;
        return MQ_SUCCESS;
      }
      // 连接已断开，丢弃
      destroy_socket_internal(conn.socket);
    }

    // 2. idle 为空：池未满则占位新建连接
    if (pool->in_use_count < pool->max_size) {
      pool->in_use_count++;
      is_pooled    = true;
      need_connect = true;
    }
    // 3. 池已满：退出锁，建临时连接（is_pooled=false）
  }

  // 在锁外建立 TCP 连接
  MQTTSocket* new_socket = NULL;
  int         ret        = MQTTSocket::create_tcp_socket(new_socket);
  if (MQ_FAIL(ret)) {
    if (is_pooled) {
      std::lock_guard<std::mutex> lk(pool->mutex);
      pool->in_use_count--;
    }
    is_pooled = false;
    return MQ_ERR_FORWARD_CONNECT;
  }

  ret = new_socket->connect(host.c_str(), port);
  if (MQ_FAIL(ret)) {
    destroy_socket_internal(new_socket);
    if (is_pooled) {
      std::lock_guard<std::mutex> lk(pool->mutex);
      pool->in_use_count--;
    }
    is_pooled = false;
    return MQ_ERR_FORWARD_CONNECT;
  }

  (void)need_connect;  // 仅用于注释说明
  socket = new_socket;
  return MQ_SUCCESS;
}

// ---------------------------------------------------------------------------
// release
// ---------------------------------------------------------------------------

void ForwardingConnectionPool::release(const std::string& host,
                                       int                port,
                                       MQTTSocket*&       socket,
                                       bool               is_pooled,
                                       bool               has_error)
{
  if (socket == NULL) {
    return;
  }

  if (!is_pooled || has_error) {
    destroy_socket_internal(socket);
    socket = NULL;
    if (is_pooled) {
      // 归还占位
      const std::string key  = host + ":" + std::to_string(port);
      RemotePool*       pool = get_pool(key);
      if (pool != NULL) {
        std::lock_guard<std::mutex> lk(pool->mutex);
        pool->in_use_count--;
      }
    }
    return;
  }

  // 健康连接归还到 idle 队列
  const std::string key  = host + ":" + std::to_string(port);
  RemotePool*       pool = get_or_create_pool(key);
  {
    std::lock_guard<std::mutex> lk(pool->mutex);
    pool->in_use_count--;
    PooledConn conn;
    conn.socket       = socket;
    conn.last_used_ms = now_ms();
    pool->idle.push_back(conn);
  }
  socket = NULL;
}

// ---------------------------------------------------------------------------
// cleanup
// ---------------------------------------------------------------------------

void ForwardingConnectionPool::cleanup_idle_connections()
{
  const int64_t now      = now_ms();
  const int64_t deadline = static_cast<int64_t>(config_.idle_timeout_ms);

  std::lock_guard<std::mutex> map_lk(pools_map_mutex_);
  for (auto& kv : pools_) {
    RemotePool*                 pool = kv.second;
    std::lock_guard<std::mutex> lk(pool->mutex);

    std::vector<PooledConn> survivors;
    for (auto& conn : pool->idle) {
      if (now - conn.last_used_ms > deadline) {
        LOG_DEBUG("ForwardingConnectionPool: closing idle connection to {}", kv.first);
        destroy_socket_internal(conn.socket);
      } else {
        survivors.push_back(conn);
      }
    }
    pool->idle.swap(survivors);
  }
}

void ForwardingConnectionPool::cleanup_thread_main()
{
  const int interval_ms = config_.idle_timeout_ms / 2;

  while (!stop_cleanup_.load()) {
    std::unique_lock<std::mutex> lk(cleanup_cv_mutex_);
    cleanup_cv_.wait_for(lk, std::chrono::milliseconds(interval_ms),
                         [this] { return stop_cleanup_.load(); });
    lk.unlock();

    if (!stop_cleanup_.load()) {
      cleanup_idle_connections();
    }
  }
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

ForwardingConnectionPool::RemotePool*
ForwardingConnectionPool::get_or_create_pool(const std::string& key)
{
  {
    std::lock_guard<std::mutex> lk(pools_map_mutex_);
    auto it = pools_.find(key);
    if (it != pools_.end()) {
      return it->second;
    }
    RemotePool* pool = new RemotePool(config_.pool_size_per_target);
    pools_[key]      = pool;
    return pool;
  }
}

ForwardingConnectionPool::RemotePool*
ForwardingConnectionPool::get_pool(const std::string& key)
{
  std::lock_guard<std::mutex> lk(pools_map_mutex_);
  auto it = pools_.find(key);
  return (it != pools_.end()) ? it->second : NULL;
}

void ForwardingConnectionPool::destroy_socket_internal(MQTTSocket* socket)
{
  MQTTAllocator* root = MQTTMemoryManager::get_instance().get_root_allocator();
  if (socket != NULL) {
    socket->close();
    socket->~MQTTSocket();
    if (root != NULL) {
      root->deallocate(socket, sizeof(MQTTSocket));
    }
  }
}

int64_t ForwardingConnectionPool::now_ms() const
{
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace mqtt
