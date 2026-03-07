#pragma once

#include "mqtt_auth_interface.h"

#ifdef HAVE_SQLITE3
#include <sqlite3.h>
#include <memory>
#include "mqtt_shared_mutex_compat.h"
#include "mqtt_coroutine_utils.h"
#include <unordered_map>
#include <chrono>
#include <queue>

namespace mqtt {
namespace auth {

/**
 * @brief SQLite认证提供者配置
 */
struct SQLiteAuthConfig {
  std::string db_path;                    // 数据库文件路径
  int connection_pool_size;               // 连接池大小
  int max_retry_count;                    // 最大重试次数
  int retry_delay_ms;                     // 重试延迟（毫秒）
  int query_timeout_ms;                   // 查询超时（毫秒）
  bool enable_wal_mode;                   // 是否启用WAL模式
  bool enable_foreign_keys;               // 是否启用外键约束
  int cache_size_kb;                      // SQLite缓存大小（KB）
  
  SQLiteAuthConfig() 
    : db_path("auth.db"),
      connection_pool_size(10),
      max_retry_count(3),
      retry_delay_ms(100),
      query_timeout_ms(5000),
      enable_wal_mode(true),
      enable_foreign_keys(true),
      cache_size_kb(2048) {}
};

/**
 * @brief SQLite连接池中的连接
 */
class SQLiteConnection {
public:
  SQLiteConnection(sqlite3* db, int id);
  ~SQLiteConnection();
  
  sqlite3* get_db() const { return db_; }
  int get_id() const { return id_; }
  bool is_busy() const { return busy_; }
  void set_busy(bool busy) { busy_ = busy; }
  
  std::chrono::steady_clock::time_point get_last_used() const { return last_used_; }
  void update_last_used() { last_used_ = std::chrono::steady_clock::now(); }

private:
  sqlite3* db_;
  int id_;
  bool busy_;
  std::chrono::steady_clock::time_point last_used_;
};

/**
 * @brief SQLite连接池
 */
class SQLiteConnectionPool {
public:
  explicit SQLiteConnectionPool(const SQLiteAuthConfig& config);
  ~SQLiteConnectionPool();
  
  int initialize();
  void cleanup();
  
  std::shared_ptr<SQLiteConnection> acquire_connection();
  void release_connection(std::shared_ptr<SQLiteConnection> conn);
  
  size_t get_pool_size() const { return pool_.size(); }
  size_t get_active_connections() const;

private:
  SQLiteAuthConfig config_;
  std::vector<std::shared_ptr<SQLiteConnection>> pool_;
  std::queue<std::shared_ptr<SQLiteConnection>> available_;
  mutable mqtt::CoroMutex pool_mutex_;
  mqtt::CoroCondition pool_cv_;
  bool initialized_;

  int create_connection(sqlite3** db);
  void configure_connection(sqlite3* db);
};

/**
 * @brief SQLite认证提供者实现
 * 
 * 特点：
 * 1. 连接池管理，避免频繁创建/销毁连接
 * 2. 细粒度锁定，提高并发性能
 * 3. 支持用户和主题权限管理
 * 4. 密码哈希存储
 * 5. 完整的统计信息
 */
class SQLiteAuthProvider : public IAuthProvider {
public:
  explicit SQLiteAuthProvider(const SQLiteAuthConfig& config, MQTTAllocator* allocator);
  ~SQLiteAuthProvider() override;

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
  
  const char* get_provider_name() const override { return "SQLite"; }
  bool is_healthy() const override;

  // SQLite特有方法
  /**
   * @brief 添加用户
   * @param username 用户名
   * @param password 密码（明文，会自动哈希）
   * @param is_super_user 是否为超级用户
   * @return MQ_SUCCESS成功，其他值失败
   */
  int add_user(const MQTTString& username, const MQTTString& password, bool is_super_user = false);
  
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
   * @return MQ_SUCCESS成功，其他值失败
   */
  int add_topic_permission(const MQTTString& username, const MQTTString& topic_pattern, Permission permission);
  
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

private:
  SQLiteAuthConfig config_;
  MQTTAllocator* allocator_;
  std::unique_ptr<SQLiteConnectionPool> connection_pool_;
  mutable AuthStats stats_;
  mutable mqtt::CoroMutex stats_mutex_;
  
  // 主题权限缓存
  struct TopicPermissionCache {
    std::unordered_map<std::string, std::vector<TopicPermission>> user_permissions;
    std::chrono::steady_clock::time_point last_update;
  };
  mutable TopicPermissionCache topic_cache_;
  mutable mqtt::compat::shared_mutex topic_cache_mutex_;
  static const int TOPIC_CACHE_TTL_SECONDS = 300;  // 5分钟缓存
  
  int create_tables();
  int create_indexes();
  
  std::string hash_password(const std::string& password);
  bool verify_password(const std::string& password, const std::string& hash);
  
  AuthResult authenticate_user_internal(const MQTTString& username,
                                       const MQTTString& password,
                                       UserInfo& user_info);
  
  AuthResult check_topic_access_internal(const MQTTString& username,
                                        const MQTTString& topic,
                                        Permission permission);
  
  void update_topic_cache(const MQTTString& username);
  bool is_topic_cache_valid() const;
  
  bool match_topic_pattern(const std::string& topic, const std::string& pattern);
  
  void update_stats(bool login_success, bool topic_access_granted);
  
  static int sqlite_exec_callback(void* data, int argc, char** argv, char** column_names);
};

} // namespace auth
} // namespace mqtt

#else // HAVE_SQLITE3

// Dummy declarations when SQLite3 is not available
namespace mqtt {
namespace auth {
  // Empty namespace when SQLite3 is not available
}
}

#endif // HAVE_SQLITE3
