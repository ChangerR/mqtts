#ifdef HAVE_SQLITE3

#include "mqtt_auth_sqlite.h"
#include "logger.h"
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <iomanip>
#include <sstream>
#include <regex>
#include <thread>

namespace mqtt {
namespace auth {

//==============================================================================
// SQLiteConnection Implementation
//==============================================================================

SQLiteConnection::SQLiteConnection(sqlite3* db, int id)
    : db_(db), id_(id), busy_(false), last_used_(std::chrono::steady_clock::now()) {
}

SQLiteConnection::~SQLiteConnection() {
    if (db_) {
        sqlite3_close(db_);
    }
}

//==============================================================================
// SQLiteConnectionPool Implementation
//==============================================================================

SQLiteConnectionPool::SQLiteConnectionPool(const SQLiteAuthConfig& config)
    : config_(config), initialized_(false) {
}

SQLiteConnectionPool::~SQLiteConnectionPool() {
    cleanup();
}

int SQLiteConnectionPool::initialize() {
  int __mq_ret = 0;
  do {
      std::unique_lock<std::mutex> lock(pool_mutex_);
      
      if (initialized_) {
          __mq_ret = MQ_SUCCESS;
          break;
      }
      
      LOG_INFO("Initializing SQLite connection pool with {} connections", config_.connection_pool_size);
      
      pool_.reserve(config_.connection_pool_size);
      
      for (int i = 0; i < config_.connection_pool_size; ++i) {
          sqlite3* db = nullptr;
          int ret = create_connection(&db);
          if (ret != MQ_SUCCESS) {
              LOG_ERROR("Failed to create SQLite connection {}: {}", i, ret);
              cleanup();
              __mq_ret = ret;
              break;
          }
          
          auto conn = std::make_shared<SQLiteConnection>(db, i);
          pool_.push_back(conn);
          available_.push(conn);
      }
      
      initialized_ = true;
      LOG_INFO("SQLite connection pool initialized successfully");
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

void SQLiteConnectionPool::cleanup() {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    
    if (!initialized_) {
        return;
    }
    
    LOG_INFO("Cleaning up SQLite connection pool");
    
    // 清空可用连接队列
    while (!available_.empty()) {
        available_.pop();
    }
    
    // 关闭所有连接
    pool_.clear();
    
    initialized_ = false;
}

std::shared_ptr<SQLiteConnection> SQLiteConnectionPool::acquire_connection() {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    
    if (!initialized_) {
        LOG_ERROR("Connection pool not initialized");
        return nullptr;
    }
    
    // 等待可用连接
    pool_cv_.wait(lock, [this] { return !available_.empty(); });
    
    auto conn = available_.front();
    available_.pop();
    
    conn->set_busy(true);
    conn->update_last_used();
    
    return conn;
}

void SQLiteConnectionPool::release_connection(std::shared_ptr<SQLiteConnection> conn) {
    if (!conn) {
        return;
    }
    
    std::unique_lock<std::mutex> lock(pool_mutex_);
    
    conn->set_busy(false);
    conn->update_last_used();
    
    available_.push(conn);
    pool_cv_.notify_one();
}

size_t SQLiteConnectionPool::get_active_connections() const {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    return pool_.size() - available_.size();
}

int SQLiteConnectionPool::create_connection(sqlite3** db) {
  int __mq_ret = 0;
  do {
      int ret = sqlite3_open(config_.db_path.c_str(), db);
      if (ret != SQLITE_OK) {
          LOG_ERROR("Failed to open SQLite database '{}': {}", config_.db_path, sqlite3_errmsg(*db));
          if (*db) {
              sqlite3_close(*db);
              *db = nullptr;
          }
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      configure_connection(*db);
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

void SQLiteConnectionPool::configure_connection(sqlite3* db) {
    char* err_msg = nullptr;
    
    // 设置超时
    sqlite3_busy_timeout(db, config_.query_timeout_ms);
    
    // 启用WAL模式
    if (config_.enable_wal_mode) {
        sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err_msg);
        if (err_msg) {
            LOG_WARN("Failed to set WAL mode: {}", err_msg);
            sqlite3_free(err_msg);
        }
    }
    
    // 启用外键约束
    if (config_.enable_foreign_keys) {
        sqlite3_exec(db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, &err_msg);
        if (err_msg) {
            LOG_WARN("Failed to enable foreign keys: {}", err_msg);
            sqlite3_free(err_msg);
        }
    }
    
    // 设置缓存大小
    std::string cache_pragma = "PRAGMA cache_size=-" + std::to_string(config_.cache_size_kb) + ";";
    sqlite3_exec(db, cache_pragma.c_str(), nullptr, nullptr, &err_msg);
    if (err_msg) {
        LOG_WARN("Failed to set cache size: {}", err_msg);
        sqlite3_free(err_msg);
    }
    
    // 设置同步模式为NORMAL（平衡性能和安全性）
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, &err_msg);
    if (err_msg) {
        LOG_WARN("Failed to set synchronous mode: {}", err_msg);
        sqlite3_free(err_msg);
    }
}

//==============================================================================
// SQLiteAuthProvider Implementation
//==============================================================================

SQLiteAuthProvider::SQLiteAuthProvider(const SQLiteAuthConfig& config, MQTTAllocator* allocator)
    : config_(config), allocator_(allocator) {
    connection_pool_ = std::unique_ptr<SQLiteConnectionPool>(new SQLiteConnectionPool(config));
}

SQLiteAuthProvider::~SQLiteAuthProvider() {
    cleanup();
}

int SQLiteAuthProvider::initialize() {
  int __mq_ret = 0;
  do {
      LOG_INFO("Initializing SQLite authentication provider");
      
      int ret = connection_pool_->initialize();
      if (ret != MQ_SUCCESS) {
          LOG_ERROR("Failed to initialize SQLite connection pool: {}", ret);
          __mq_ret = ret;
          break;
      }
      
      ret = create_tables();
      if (ret != MQ_SUCCESS) {
          LOG_ERROR("Failed to create SQLite tables: {}", ret);
          __mq_ret = ret;
          break;
      }
      
      ret = create_indexes();
      if (ret != MQ_SUCCESS) {
          LOG_ERROR("Failed to create SQLite indexes: {}", ret);
          __mq_ret = ret;
          break;
      }
      
      LOG_INFO("SQLite authentication provider initialized successfully");
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

void SQLiteAuthProvider::cleanup() {
    LOG_INFO("Cleaning up SQLite authentication provider");
    
    if (connection_pool_) {
        connection_pool_->cleanup();
    }
    
    std::unique_lock<mqtt::compat::shared_mutex> lock(topic_cache_mutex_);
    topic_cache_.user_permissions.clear();
}

AuthResult SQLiteAuthProvider::authenticate_user(const MQTTString& username,
                                                const MQTTString& password,
                                                const MQTTString& client_id,
                                                const MQTTString& client_ip,
                                                uint16_t client_port,
                                                UserInfo& user_info) {
    AuthResult result = authenticate_user_internal(username, password, user_info);
    
    // 填充用户信息
    if (result == AuthResult::SUCCESS) {
        user_info.client_id = client_id;
        user_info.client_ip = client_ip;
        user_info.client_port = client_port;
    }
    
    update_stats(result == AuthResult::SUCCESS, false);
    return result;
}

AuthResult SQLiteAuthProvider::check_topic_access(const UserInfo& user_info,
                                                 const MQTTString& topic,
                                                 Permission permission) {
    AuthResult result = check_topic_access_internal(user_info.username, topic, permission);
    update_stats(false, result == AuthResult::SUCCESS);
    return result;
}

bool SQLiteAuthProvider::is_super_user(const MQTTString& username) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire database connection");
        return false;
    }
    
    const char* sql = "SELECT is_super_user FROM users WHERE username = ?";
    sqlite3_stmt* stmt;
    
    int ret = sqlite3_prepare_v2(conn->get_db(), sql, -1, &stmt, nullptr);
    if (ret != SQLITE_OK) {
        LOG_ERROR("Failed to prepare SQL statement: {}", sqlite3_errmsg(conn->get_db()));
        connection_pool_->release_connection(conn);
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, from_mqtt_string(username).c_str(), -1, SQLITE_STATIC);
    
    bool is_super = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        is_super = sqlite3_column_int(stmt, 0) != 0;
    }
    
    sqlite3_finalize(stmt);
    connection_pool_->release_connection(conn);
    
    return is_super;
}

AuthStats SQLiteAuthProvider::get_stats() const {
    std::unique_lock<std::mutex> lock(stats_mutex_);
    return stats_;
}

void SQLiteAuthProvider::reset_stats() {
    std::unique_lock<std::mutex> lock(stats_mutex_);
    stats_ = AuthStats();
}

bool SQLiteAuthProvider::is_healthy() const {
    if (!connection_pool_) {
        return false;
    }
    
    // 尝试获取连接并执行简单查询
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        return false;
    }
    
    sqlite3_stmt* stmt;
    int ret = sqlite3_prepare_v2(conn->get_db(), "SELECT 1", -1, &stmt, nullptr);
    bool healthy = (ret == SQLITE_OK);
    
    if (healthy) {
        ret = sqlite3_step(stmt);
        healthy = (ret == SQLITE_ROW);
        sqlite3_finalize(stmt);
    }
    
    connection_pool_->release_connection(conn);
    return healthy;
}

int SQLiteAuthProvider::add_user(const MQTTString& username, const MQTTString& password, bool is_super_user) {
  int __mq_ret = 0;
  do {
      auto conn = connection_pool_->acquire_connection();
      if (!conn) {
          LOG_ERROR("Failed to acquire database connection");
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      std::string password_hash = hash_password(from_mqtt_string(password));
      
      const char* sql = "INSERT INTO users (username, password_hash, is_super_user, created_at) VALUES (?, ?, ?, datetime('now'))";
      sqlite3_stmt* stmt;
      
      int ret = sqlite3_prepare_v2(conn->get_db(), sql, -1, &stmt, nullptr);
      if (ret != SQLITE_OK) {
          LOG_ERROR("Failed to prepare SQL statement: {}", sqlite3_errmsg(conn->get_db()));
          connection_pool_->release_connection(conn);
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      sqlite3_bind_text(stmt, 1, from_mqtt_string(username).c_str(), -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, password_hash.c_str(), -1, SQLITE_STATIC);
      sqlite3_bind_int(stmt, 3, is_super_user ? 1 : 0);
      
      ret = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      connection_pool_->release_connection(conn);
      
      if (ret != SQLITE_DONE) {
          LOG_ERROR("Failed to insert user: {}", sqlite3_errmsg(conn->get_db()));
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      LOG_INFO("User '{}' added successfully", from_mqtt_string(username));
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

int SQLiteAuthProvider::remove_user(const MQTTString& username) {
  int __mq_ret = 0;
  do {
      auto conn = connection_pool_->acquire_connection();
      if (!conn) {
          LOG_ERROR("Failed to acquire database connection");
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      // 开始事务
      sqlite3_exec(conn->get_db(), "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
      
      // 删除用户的主题权限
      const char* delete_perms_sql = "DELETE FROM topic_permissions WHERE username = ?";
      sqlite3_stmt* stmt;
      
      int ret = sqlite3_prepare_v2(conn->get_db(), delete_perms_sql, -1, &stmt, nullptr);
      if (ret == SQLITE_OK) {
          sqlite3_bind_text(stmt, 1, from_mqtt_string(username).c_str(), -1, SQLITE_STATIC);
          sqlite3_step(stmt);
          sqlite3_finalize(stmt);
      }
      
      // 删除用户
      const char* delete_user_sql = "DELETE FROM users WHERE username = ?";
      ret = sqlite3_prepare_v2(conn->get_db(), delete_user_sql, -1, &stmt, nullptr);
      if (ret != SQLITE_OK) {
          LOG_ERROR("Failed to prepare SQL statement: {}", sqlite3_errmsg(conn->get_db()));
          sqlite3_exec(conn->get_db(), "ROLLBACK", nullptr, nullptr, nullptr);
          connection_pool_->release_connection(conn);
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      sqlite3_bind_text(stmt, 1, from_mqtt_string(username).c_str(), -1, SQLITE_STATIC);
      ret = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      
      if (ret != SQLITE_DONE) {
          LOG_ERROR("Failed to delete user: {}", sqlite3_errmsg(conn->get_db()));
          sqlite3_exec(conn->get_db(), "ROLLBACK", nullptr, nullptr, nullptr);
          connection_pool_->release_connection(conn);
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      // 提交事务
      sqlite3_exec(conn->get_db(), "COMMIT", nullptr, nullptr, nullptr);
      connection_pool_->release_connection(conn);
      
      LOG_INFO("User '{}' removed successfully", from_mqtt_string(username));
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

int SQLiteAuthProvider::update_user_password(const MQTTString& username, const MQTTString& new_password) {
  int __mq_ret = 0;
  do {
      auto conn = connection_pool_->acquire_connection();
      if (!conn) {
          LOG_ERROR("Failed to acquire database connection");
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      std::string password_hash = hash_password(from_mqtt_string(new_password));
      
      const char* sql = "UPDATE users SET password_hash = ?, updated_at = datetime('now') WHERE username = ?";
      sqlite3_stmt* stmt;
      
      int ret = sqlite3_prepare_v2(conn->get_db(), sql, -1, &stmt, nullptr);
      if (ret != SQLITE_OK) {
          LOG_ERROR("Failed to prepare SQL statement: {}", sqlite3_errmsg(conn->get_db()));
          connection_pool_->release_connection(conn);
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      sqlite3_bind_text(stmt, 1, password_hash.c_str(), -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, from_mqtt_string(username).c_str(), -1, SQLITE_STATIC);
      
      ret = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      connection_pool_->release_connection(conn);
      
      if (ret != SQLITE_DONE) {
          LOG_ERROR("Failed to update user password: {}", sqlite3_errmsg(conn->get_db()));
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      LOG_INFO("Password updated for user '{}'", from_mqtt_string(username));
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

int SQLiteAuthProvider::add_topic_permission(const MQTTString& username, const MQTTString& topic_pattern, Permission permission) {
  int __mq_ret = 0;
  do {
      auto conn = connection_pool_->acquire_connection();
      if (!conn) {
          LOG_ERROR("Failed to acquire database connection");
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      const char* sql = "INSERT OR REPLACE INTO topic_permissions (username, topic_pattern, permission, created_at) VALUES (?, ?, ?, datetime('now'))";
      sqlite3_stmt* stmt;
      
      int ret = sqlite3_prepare_v2(conn->get_db(), sql, -1, &stmt, nullptr);
      if (ret != SQLITE_OK) {
          LOG_ERROR("Failed to prepare SQL statement: {}", sqlite3_errmsg(conn->get_db()));
          connection_pool_->release_connection(conn);
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      sqlite3_bind_text(stmt, 1, from_mqtt_string(username).c_str(), -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, from_mqtt_string(topic_pattern).c_str(), -1, SQLITE_STATIC);
      sqlite3_bind_int(stmt, 3, static_cast<int>(permission));
      
      ret = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      connection_pool_->release_connection(conn);
      
      if (ret != SQLITE_DONE) {
          LOG_ERROR("Failed to insert topic permission: {}", sqlite3_errmsg(conn->get_db()));
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      // 使缓存失效
      std::unique_lock<mqtt::compat::shared_mutex> lock(topic_cache_mutex_);
      topic_cache_.user_permissions.erase(from_mqtt_string(username));
      
      LOG_INFO("Topic permission added for user '{}': {} -> {}", 
               from_mqtt_string(username), from_mqtt_string(topic_pattern), static_cast<int>(permission));
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

int SQLiteAuthProvider::remove_topic_permission(const MQTTString& username, const MQTTString& topic_pattern) {
  int __mq_ret = 0;
  do {
      auto conn = connection_pool_->acquire_connection();
      if (!conn) {
          LOG_ERROR("Failed to acquire database connection");
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      const char* sql = "DELETE FROM topic_permissions WHERE username = ? AND topic_pattern = ?";
      sqlite3_stmt* stmt;
      
      int ret = sqlite3_prepare_v2(conn->get_db(), sql, -1, &stmt, nullptr);
      if (ret != SQLITE_OK) {
          LOG_ERROR("Failed to prepare SQL statement: {}", sqlite3_errmsg(conn->get_db()));
          connection_pool_->release_connection(conn);
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      sqlite3_bind_text(stmt, 1, from_mqtt_string(username).c_str(), -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, from_mqtt_string(topic_pattern).c_str(), -1, SQLITE_STATIC);
      
      ret = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      connection_pool_->release_connection(conn);
      
      if (ret != SQLITE_DONE) {
          LOG_ERROR("Failed to delete topic permission: {}", sqlite3_errmsg(conn->get_db()));
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      // 使缓存失效
      std::unique_lock<mqtt::compat::shared_mutex> lock(topic_cache_mutex_);
      topic_cache_.user_permissions.erase(from_mqtt_string(username));
      
      LOG_INFO("Topic permission removed for user '{}': {}", 
               from_mqtt_string(username), from_mqtt_string(topic_pattern));
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

int SQLiteAuthProvider::clear_user_permissions(const MQTTString& username) {
  int __mq_ret = 0;
  do {
      auto conn = connection_pool_->acquire_connection();
      if (!conn) {
          LOG_ERROR("Failed to acquire database connection");
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      const char* sql = "DELETE FROM topic_permissions WHERE username = ?";
      sqlite3_stmt* stmt;
      
      int ret = sqlite3_prepare_v2(conn->get_db(), sql, -1, &stmt, nullptr);
      if (ret != SQLITE_OK) {
          LOG_ERROR("Failed to prepare SQL statement: {}", sqlite3_errmsg(conn->get_db()));
          connection_pool_->release_connection(conn);
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      sqlite3_bind_text(stmt, 1, from_mqtt_string(username).c_str(), -1, SQLITE_STATIC);
      
      ret = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      connection_pool_->release_connection(conn);
      
      if (ret != SQLITE_DONE) {
          LOG_ERROR("Failed to clear user permissions: {}", sqlite3_errmsg(conn->get_db()));
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      // 使缓存失效
      std::unique_lock<mqtt::compat::shared_mutex> lock(topic_cache_mutex_);
      topic_cache_.user_permissions.erase(from_mqtt_string(username));
      
      LOG_INFO("All permissions cleared for user '{}'", from_mqtt_string(username));
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

int SQLiteAuthProvider::create_tables() {
  int __mq_ret = 0;
  do {
      auto conn = connection_pool_->acquire_connection();
      if (!conn) {
          LOG_ERROR("Failed to acquire database connection");
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      const char* users_table = R"(
          CREATE TABLE IF NOT EXISTS users (
              username TEXT PRIMARY KEY,
              password_hash TEXT NOT NULL,
              is_super_user INTEGER DEFAULT 0,
              created_at TEXT NOT NULL,
              updated_at TEXT DEFAULT NULL
          )
      )";
      
      const char* topic_permissions_table = R"(
          CREATE TABLE IF NOT EXISTS topic_permissions (
              username TEXT NOT NULL,
              topic_pattern TEXT NOT NULL,
              permission INTEGER NOT NULL,
              created_at TEXT NOT NULL,
              PRIMARY KEY (username, topic_pattern),
              FOREIGN KEY (username) REFERENCES users(username) ON DELETE CASCADE
          )
      )";
      
      char* err_msg = nullptr;
      
      int ret = sqlite3_exec(conn->get_db(), users_table, nullptr, nullptr, &err_msg);
      if (ret != SQLITE_OK) {
          LOG_ERROR("Failed to create users table: {}", err_msg);
          sqlite3_free(err_msg);
          connection_pool_->release_connection(conn);
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      ret = sqlite3_exec(conn->get_db(), topic_permissions_table, nullptr, nullptr, &err_msg);
      if (ret != SQLITE_OK) {
          LOG_ERROR("Failed to create topic_permissions table: {}", err_msg);
          sqlite3_free(err_msg);
          connection_pool_->release_connection(conn);
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      connection_pool_->release_connection(conn);
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

int SQLiteAuthProvider::create_indexes() {
  int __mq_ret = 0;
  do {
      auto conn = connection_pool_->acquire_connection();
      if (!conn) {
          LOG_ERROR("Failed to acquire database connection");
          __mq_ret = MQ_ERR_DATABASE;
          break;
      }
      
      const char* indexes[] = {
          "CREATE INDEX IF NOT EXISTS idx_users_username ON users(username)",
          "CREATE INDEX IF NOT EXISTS idx_topic_permissions_username ON topic_permissions(username)",
          "CREATE INDEX IF NOT EXISTS idx_topic_permissions_pattern ON topic_permissions(topic_pattern)"
      };
      
      char* err_msg = nullptr;
      
      for (const char* index : indexes) {
          int ret = sqlite3_exec(conn->get_db(), index, nullptr, nullptr, &err_msg);
          if (ret != SQLITE_OK) {
              LOG_ERROR("Failed to create index: {}", err_msg);
              sqlite3_free(err_msg);
              connection_pool_->release_connection(conn);
              __mq_ret = MQ_ERR_DATABASE;
              break;
          }
      }
      
      connection_pool_->release_connection(conn);
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

std::string SQLiteAuthProvider::hash_password(const std::string& password) {
    // 使用SHA-256哈希密码
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, password.c_str(), password.length());
    SHA256_Final(hash, &sha256);
    
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    
    return ss.str();
}

bool SQLiteAuthProvider::verify_password(const std::string& password, const std::string& hash) {
    return hash_password(password) == hash;
}

AuthResult SQLiteAuthProvider::authenticate_user_internal(const MQTTString& username,
                                                         const MQTTString& password,
                                                         UserInfo& user_info) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire database connection");
        return AuthResult::INTERNAL_ERROR;
    }
    
    const char* sql = "SELECT password_hash, is_super_user FROM users WHERE username = ?";
    sqlite3_stmt* stmt;
    
    int ret = sqlite3_prepare_v2(conn->get_db(), sql, -1, &stmt, nullptr);
    if (ret != SQLITE_OK) {
        LOG_ERROR("Failed to prepare SQL statement: {}", sqlite3_errmsg(conn->get_db()));
        connection_pool_->release_connection(conn);
        return AuthResult::INTERNAL_ERROR;
    }
    
    sqlite3_bind_text(stmt, 1, from_mqtt_string(username).c_str(), -1, SQLITE_STATIC);
    
    AuthResult result = AuthResult::USER_NOT_FOUND;
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* stored_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        bool is_super = sqlite3_column_int(stmt, 1) != 0;
        
        if (verify_password(from_mqtt_string(password), stored_hash)) {
            result = AuthResult::SUCCESS;
            user_info.username = username;
            user_info.is_super_user = is_super;
        } else {
            result = AuthResult::INVALID_CREDENTIALS;
        }
    }
    
    sqlite3_finalize(stmt);
    connection_pool_->release_connection(conn);
    
    return result;
}

AuthResult SQLiteAuthProvider::check_topic_access_internal(const MQTTString& username,
                                                          const MQTTString& topic,
                                                          Permission permission) {
    // 检查缓存
    std::string username_str = from_mqtt_string(username);
    std::string topic_str = from_mqtt_string(topic);
    
    {
        mqtt::compat::shared_lock<mqtt::compat::shared_mutex> lock(topic_cache_mutex_);
        if (is_topic_cache_valid()) {
            auto it = topic_cache_.user_permissions.find(username_str);
            if (it != topic_cache_.user_permissions.end()) {
                // 检查权限
                for (const auto& perm : it->second) {
                    if (match_topic_pattern(topic_str, from_mqtt_string(perm.topic_pattern))) {
                        if ((perm.permission == Permission::READWRITE) ||
                            (perm.permission == permission)) {
                            return AuthResult::SUCCESS;
                        }
                    }
                }
                return AuthResult::TOPIC_ACCESS_DENIED;
            }
        }
    }
    
    // 缓存未命中，从数据库加载
    update_topic_cache(username);
    
    // 重新检查缓存
    {
        mqtt::compat::shared_lock<mqtt::compat::shared_mutex> lock(topic_cache_mutex_);
        auto it = topic_cache_.user_permissions.find(username_str);
        if (it != topic_cache_.user_permissions.end()) {
            for (const auto& perm : it->second) {
                if (match_topic_pattern(topic_str, from_mqtt_string(perm.topic_pattern))) {
                    if ((perm.permission == Permission::READWRITE) ||
                        (perm.permission == permission)) {
                        return AuthResult::SUCCESS;
                    }
                }
            }
        }
    }
    
    return AuthResult::TOPIC_ACCESS_DENIED;
}

void SQLiteAuthProvider::update_topic_cache(const MQTTString& username) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire database connection");
        return;
    }
    
    const char* sql = "SELECT topic_pattern, permission FROM topic_permissions WHERE username = ?";
    sqlite3_stmt* stmt;
    
    int ret = sqlite3_prepare_v2(conn->get_db(), sql, -1, &stmt, nullptr);
    if (ret != SQLITE_OK) {
        LOG_ERROR("Failed to prepare SQL statement: {}", sqlite3_errmsg(conn->get_db()));
        connection_pool_->release_connection(conn);
        return;
    }
    
    sqlite3_bind_text(stmt, 1, from_mqtt_string(username).c_str(), -1, SQLITE_STATIC);
    
    std::vector<TopicPermission> permissions;
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* topic_pattern = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        int permission = sqlite3_column_int(stmt, 1);
        
        TopicPermission perm(allocator_);
        perm.topic_pattern = MQTTString(topic_pattern, MQTTStrAllocator(allocator_));
        perm.permission = static_cast<Permission>(permission);
        permissions.push_back(std::move(perm));
    }
    
    sqlite3_finalize(stmt);
    connection_pool_->release_connection(conn);
    
    // 更新缓存
    std::unique_lock<mqtt::compat::shared_mutex> lock(topic_cache_mutex_);
    topic_cache_.user_permissions[from_mqtt_string(username)] = std::move(permissions);
    topic_cache_.last_update = std::chrono::steady_clock::now();
}

bool SQLiteAuthProvider::is_topic_cache_valid() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - topic_cache_.last_update);
    return elapsed.count() < TOPIC_CACHE_TTL_SECONDS;
}

bool SQLiteAuthProvider::match_topic_pattern(const std::string& topic, const std::string& pattern) {
    // 简单的通配符匹配实现
    // + 匹配单个层级
    // # 匹配多个层级
    
    if (pattern == "#") {
        return true;  // 匹配所有主题
    }
    
    if (pattern.find('#') == std::string::npos && pattern.find('+') == std::string::npos) {
        return topic == pattern;  // 精确匹配
    }
    
    // 转换为正则表达式
    std::string regex_pattern = pattern;
    
    // 转义特殊字符
    std::regex special_chars{R"([-[\]{}()*+?.,\^$|#\s])"};
    regex_pattern = std::regex_replace(regex_pattern, special_chars, R"(\$&)");
    
    // 还原通配符
    regex_pattern = std::regex_replace(regex_pattern, std::regex(R"(\\#)"), ".*");
    regex_pattern = std::regex_replace(regex_pattern, std::regex(R"(\\\+)"), "[^/]+");
    
    try {
        std::regex topic_regex(regex_pattern);
        return std::regex_match(topic, topic_regex);
    } catch (const std::regex_error& e) {
        LOG_ERROR("Invalid topic pattern regex: {}", e.what());
        return false;
    }
}

void SQLiteAuthProvider::update_stats(bool login_success, bool topic_access_granted) {
    std::unique_lock<std::mutex> lock(stats_mutex_);
    
    if (login_success) {
        stats_.total_login_attempts++;
        stats_.successful_logins++;
    } else {
        stats_.total_login_attempts++;
        stats_.failed_logins++;
    }
    
    if (topic_access_granted) {
        stats_.total_topic_checks++;
        stats_.topic_access_granted++;
    } else {
        stats_.total_topic_checks++;
        stats_.topic_access_denied++;
    }
}

int SQLiteAuthProvider::sqlite_exec_callback(void* data, int argc, char** argv, char** column_names) {
    // 简单的回调函数，用于调试
    for (int i = 0; i < argc; i++) {
        LOG_DEBUG("{}={}", column_names[i], argv[i] ? argv[i] : "NULL");
    }
    return 0;
}

} // namespace auth
} // namespace mqtt

#endif // HAVE_SQLITE3
