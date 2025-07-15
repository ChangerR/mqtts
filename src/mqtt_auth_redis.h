#pragma once

#include "mqtt_auth_interface.h"
#include <hiredis/hiredis.h>
#include <memory>
#include <shared_mutex>
#include <queue>
#include <condition_variable>

namespace mqtt {
namespace auth {

/**
 * @brief Redis认证提供者配置
 */
struct RedisAuthConfig {
  std::string host;                       // Redis服务器地址
  int port;                              // Redis服务器端口
  std::string password;                   // Redis密码
  int database;                          // Redis数据库索引
  int connection_pool_size;              // 连接池大小
  int max_retry_count;                   // 最大重试次数
  int retry_delay_ms;                    // 重试延迟（毫秒）
  int connect_timeout_ms;                // 连接超时（毫秒）
  int command_timeout_ms;                // 命令超时（毫秒）
  int keepalive_interval_s;              // 保活间隔（秒）
  std::string key_prefix;                // Redis键前缀
  int cache_ttl_seconds;                 // 缓存TTL（秒）
  
  RedisAuthConfig() 
    : host("127.0.0.1"),
      port(6379),
      password(""),
      database(0),
      connection_pool_size(10),
      max_retry_count(3),
      retry_delay_ms(100),
      connect_timeout_ms(5000),
      command_timeout_ms(5000),
      keepalive_interval_s(30),
      key_prefix("mqtt:auth:"),
      cache_ttl_seconds(300) {}
};

/**
 * @brief Redis连接包装器
 */
class RedisConnection {
public:
  RedisConnection(redisContext* ctx, int id);
  ~RedisConnection();
  
  redisContext* get_context() const { return ctx_; }
  int get_id() const { return id_; }
  bool is_busy() const { return busy_; }
  void set_busy(bool busy) { busy_ = busy; }
  bool is_connected() const;
  
  std::chrono::steady_clock::time_point get_last_used() const { return last_used_; }
  void update_last_used() { last_used_ = std::chrono::steady_clock::now(); }
  
  int ping();
  int select_database(int db);

private:
  redisContext* ctx_;
  int id_;
  bool busy_;
  std::chrono::steady_clock::time_point last_used_;
};

/**
 * @brief Redis连接池
 */
class RedisConnectionPool {
public:
  explicit RedisConnectionPool(const RedisAuthConfig& config);
  ~RedisConnectionPool();
  
  int initialize();
  void cleanup();
  
  std::shared_ptr<RedisConnection> acquire_connection();
  void release_connection(std::shared_ptr<RedisConnection> conn);
  
  size_t get_pool_size() const { return pool_.size(); }
  size_t get_active_connections() const;
  
  bool test_connection();

private:
  RedisAuthConfig config_;
  std::vector<std::shared_ptr<RedisConnection>> pool_;
  std::queue<std::shared_ptr<RedisConnection>> available_;
  mutable std::mutex pool_mutex_;
  std::condition_variable pool_cv_;
  bool initialized_;
  
  redisContext* create_connection();
  int configure_connection(redisContext* ctx);
};

/**
 * @brief Redis认证提供者实现
 * 
 * Redis数据结构设计：
 * 1. 用户信息：hash - mqtt:auth:user:{username}
 *    - password_hash: 密码哈希
 *    - is_super_user: 是否超级用户 (1/0)
 *    - created_at: 创建时间
 *    - updated_at: 更新时间
 * 
 * 2. 用户主题权限：set - mqtt:auth:permissions:{username}
 *    - 成员格式：{topic_pattern}:{permission}
 * 
 * 3. 主题权限索引：hash - mqtt:auth:topic_index:{topic_pattern}
 *    - {username}: {permission}
 * 
 * 特点：
 * 1. 连接池管理，提高并发性能
 * 2. 支持Redis集群和主从模式
 * 3. 自动重连和故障恢复
 * 4. 细粒度权限控制
 * 5. 高性能缓存机制
 */
class RedisAuthProvider : public IAuthProvider {
public:
  explicit RedisAuthProvider(const RedisAuthConfig& config, MQTTAllocator* allocator);
  ~RedisAuthProvider() override;

  // IAuthProvider接口实现
  int initialize() override;
  void cleanup() override;
  
  AuthResult authenticate_user(const MQTTString& username,
                              const MQTTString& password,
                              const MQTTString& client_id,
                              const MQTTString& client_ip,
                              uint16_t client_port,
                              UserInfo& user_info) override;
  
  AuthResult check_topic_access(const UserInfo& user_info,
                               const MQTTString& topic,
                               Permission permission) override;
  
  bool is_super_user(const MQTTString& username) override;
  
  AuthStats get_stats() const override;
  void reset_stats() override;
  
  const char* get_provider_name() const override { return "Redis"; }
  bool is_healthy() const override;

  // Redis特有方法
  /**
   * @brief 添加用户
   * @param username 用户名
   * @param password 密码（明文，会自动哈希）
   * @param is_super_user 是否为超级用户
   * @param ttl_seconds TTL（秒），0表示永不过期
   * @return MQ_SUCCESS成功，其他值失败
   */
  int add_user(const MQTTString& username, const MQTTString& password, 
               bool is_super_user = false, int ttl_seconds = 0);
  
  /**
   * @brief 删除用户
   * @param username 用户名
   * @return MQ_SUCCESS成功，其他值失败
   */
  int remove_user(const MQTTString& username);
  
  /**
   * @brief 更新用户密码
   * @param username 用户名
   * @param new_password 新密码（明文）
   * @return MQ_SUCCESS成功，其他值失败
   */
  int update_user_password(const MQTTString& username, const MQTTString& new_password);
  
  /**
   * @brief 添加主题权限
   * @param username 用户名
   * @param topic_pattern 主题模式（支持通配符）
   * @param permission 权限
   * @param ttl_seconds TTL（秒），0表示永不过期
   * @return MQ_SUCCESS成功，其他值失败
   */
  int add_topic_permission(const MQTTString& username, const MQTTString& topic_pattern, 
                          Permission permission, int ttl_seconds = 0);
  
  /**
   * @brief 删除主题权限
   * @param username 用户名
   * @param topic_pattern 主题模式
   * @return MQ_SUCCESS成功，其他值失败
   */
  int remove_topic_permission(const MQTTString& username, const MQTTString& topic_pattern);
  
  /**
   * @brief 清空用户的所有主题权限
   * @param username 用户名
   * @return MQ_SUCCESS成功，其他值失败
   */
  int clear_user_permissions(const MQTTString& username);
  
  /**
   * @brief 获取用户的所有主题权限
   * @param username 用户名
   * @param permissions 输出权限列表
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_user_permissions(const MQTTString& username, std::vector<TopicPermission>& permissions);
  
  /**
   * @brief 批量设置用户权限
   * @param username 用户名
   * @param permissions 权限列表
   * @param ttl_seconds TTL（秒），0表示永不过期
   * @return MQ_SUCCESS成功，其他值失败
   */
  int set_user_permissions(const MQTTString& username, 
                          const std::vector<TopicPermission>& permissions,
                          int ttl_seconds = 0);

private:
  RedisAuthConfig config_;
  MQTTAllocator* allocator_;
  std::unique_ptr<RedisConnectionPool> connection_pool_;
  mutable AuthStats stats_;
  mutable std::mutex stats_mutex_;
  
  // 本地缓存（减少Redis查询）
  struct UserCache {
    struct UserInfo {
      std::string password_hash;
      bool is_super_user;
      std::vector<TopicPermission> permissions;
      std::chrono::steady_clock::time_point expire_time;
    };
    
    std::unordered_map<std::string, UserInfo> users;
    mutable std::shared_mutex mutex;
  };
  
  mutable UserCache local_cache_;
  
  std::string hash_password(const std::string& password);
  bool verify_password(const std::string& password, const std::string& hash);
  
  std::string make_user_key(const MQTTString& username);
  std::string make_permissions_key(const MQTTString& username);
  std::string make_topic_index_key(const MQTTString& topic_pattern);
  
  AuthResult authenticate_user_internal(const MQTTString& username,
                                       const MQTTString& password,
                                       UserInfo& user_info);
  
  AuthResult check_topic_access_internal(const MQTTString& username,
                                        const MQTTString& topic,
                                        Permission permission);
  
  int load_user_from_redis(const MQTTString& username, UserCache::UserInfo& user_info);
  int load_user_permissions_from_redis(const MQTTString& username, std::vector<TopicPermission>& permissions);
  
  bool get_from_local_cache(const MQTTString& username, UserCache::UserInfo& user_info);
  void put_to_local_cache(const MQTTString& username, const UserCache::UserInfo& user_info);
  void remove_from_local_cache(const MQTTString& username);
  void cleanup_expired_local_cache();
  
  bool match_topic_pattern(const std::string& topic, const std::string& pattern);
  
  void update_stats(bool login_success, bool topic_access_granted, bool cache_hit = false);
  
  // Redis操作辅助方法
  redisReply* execute_command(std::shared_ptr<RedisConnection> conn, const char* format, ...);
  int check_redis_reply(redisReply* reply, const char* operation);
  void free_redis_reply(redisReply* reply);
};

} // namespace auth
} // namespace mqtt