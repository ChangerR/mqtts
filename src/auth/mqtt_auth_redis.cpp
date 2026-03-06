#ifdef HAVE_HIREDIS

#include "mqtt_auth_redis.h"
#include "logger.h"
#include "mqtt_coroutine_utils.h"
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <iomanip>
#include <sstream>
#include <regex>
#include <thread>
#include <cstdarg>

namespace mqtt {
namespace auth {

//==============================================================================
// RedisConnection Implementation
//==============================================================================

RedisConnection::RedisConnection(redisContext* ctx, int id)
    : ctx_(ctx), id_(id), busy_(false), last_used_(std::chrono::steady_clock::now()) {
}

RedisConnection::~RedisConnection() {
    if (ctx_) {
        redisFree(ctx_);
    }
}

bool RedisConnection::is_connected() const {
    return ctx_ && ctx_->err == 0;
}

int RedisConnection::ping() {
    if (!is_connected()) {
        return MQ_ERR_CONNECT;
    }
    
    redisReply* reply = (redisReply*)redisCommand(ctx_, "PING");
    if (!reply) {
        LOG_ERROR("Redis PING failed: connection error");
        return MQ_ERR_CONNECT;
    }
    
    bool success = (reply->type == REDIS_REPLY_STATUS && 
                   strcmp(reply->str, "PONG") == 0);
    
    freeReplyObject(reply);
    return success ? MQ_SUCCESS : MQ_ERR_CONNECT;
}

int RedisConnection::select_database(int db) {
    if (!is_connected()) {
        return MQ_ERR_CONNECT;
    }
    
    redisReply* reply = (redisReply*)redisCommand(ctx_, "SELECT %d", db);
    if (!reply) {
        LOG_ERROR("Redis SELECT failed: connection error");
        return MQ_ERR_CONNECT;
    }
    
    bool success = (reply->type == REDIS_REPLY_STATUS && 
                   strcmp(reply->str, "OK") == 0);
    
    freeReplyObject(reply);
    return success ? MQ_SUCCESS : MQ_ERR_DATABASE;
}

//==============================================================================
// RedisConnectionPool Implementation
//==============================================================================

RedisConnectionPool::RedisConnectionPool(const RedisAuthConfig& config)
    : config_(config), initialized_(false) {
}

RedisConnectionPool::~RedisConnectionPool() {
    cleanup();
}

int RedisConnectionPool::initialize() {
    mqtt::CoroLockGuard lock(&pool_mutex_);

    if (initialized_) {
        return MQ_SUCCESS;
    }

    LOG_INFO("Initializing Redis connection pool with {} connections to {}:{}",
             config_.connection_pool_size, config_.host, config_.port);

    pool_.reserve(config_.connection_pool_size);

    for (int i = 0; i < config_.connection_pool_size; ++i) {
        redisContext* ctx = create_connection();
        if (!ctx) {
            LOG_ERROR("Failed to create Redis connection {}", i);
            cleanup();
            return MQ_ERR_CONNECT;
        }

        int ret = configure_connection(ctx);
        if (ret != MQ_SUCCESS) {
            LOG_ERROR("Failed to configure Redis connection {}: {}", i, ret);
            redisFree(ctx);
            cleanup();
            return ret;
        }

        auto conn = std::make_shared<RedisConnection>(ctx, i);
        pool_.push_back(conn);
        available_.push(conn);
    }

    initialized_ = true;
    LOG_INFO("Redis connection pool initialized successfully");
    return MQ_SUCCESS;
}

void RedisConnectionPool::cleanup() {
    mqtt::CoroLockGuard lock(&pool_mutex_);

    if (!initialized_) {
        return;
    }

    LOG_INFO("Cleaning up Redis connection pool");

    // 清空可用连接队列
    while (!available_.empty()) {
        available_.pop();
    }

    // 关闭所有连接
    pool_.clear();

    initialized_ = false;
}

std::shared_ptr<RedisConnection> RedisConnectionPool::acquire_connection() {
    while (true) {
        {
            mqtt::CoroLockGuard lock(&pool_mutex_);

            if (!initialized_) {
                LOG_ERROR("Connection pool not initialized");
                return nullptr;
            }

            if (!available_.empty()) {
                auto conn = available_.front();
                available_.pop();

                conn->set_busy(true);
                conn->update_last_used();

                // 检查连接健康状态
                if (!conn->is_connected()) {
                    LOG_WARN("Redis connection {} is disconnected, attempting reconnect", conn->get_id());

                    // 尝试重新连接
                    redisContext* new_ctx = create_connection();
                    if (new_ctx && configure_connection(new_ctx) == MQ_SUCCESS) {
                        // 替换连接
                        conn = std::make_shared<RedisConnection>(new_ctx, conn->get_id());
                        conn->set_busy(true);
                        conn->update_last_used();
                    } else {
                        if (new_ctx) {
                            redisFree(new_ctx);
                        }
                        LOG_ERROR("Failed to reconnect Redis connection {}", conn->get_id());
                        return nullptr;
                    }
                }

                return conn;
            }
        }
        // 锁已释放，等待可用连接
        pool_cv_.wait(100);  // 100ms超时
    }
}

void RedisConnectionPool::release_connection(std::shared_ptr<RedisConnection> conn) {
    if (!conn) {
        return;
    }

    {
        mqtt::CoroLockGuard lock(&pool_mutex_);

        conn->set_busy(false);
        conn->update_last_used();

        available_.push(conn);
    }
    pool_cv_.signal();
}

size_t RedisConnectionPool::get_active_connections() const {
    mqtt::CoroLockGuard lock(&pool_mutex_);
    return pool_.size() - available_.size();
}

bool RedisConnectionPool::test_connection() {
    auto conn = acquire_connection();
    if (!conn) {
        return false;
    }
    
    bool healthy = conn->ping() == MQ_SUCCESS;
    release_connection(conn);
    
    return healthy;
}

redisContext* RedisConnectionPool::create_connection() {
    struct timeval timeout = { config_.connect_timeout_ms / 1000, 
                              (config_.connect_timeout_ms % 1000) * 1000 };
    
    redisContext* ctx = redisConnectWithTimeout(config_.host.c_str(), config_.port, timeout);
    if (!ctx) {
        LOG_ERROR("Failed to allocate Redis context");
        return nullptr;
    }
    
    if (ctx->err) {
        LOG_ERROR("Failed to connect to Redis {}:{}: {}", config_.host, config_.port, ctx->errstr);
        redisFree(ctx);
        return nullptr;
    }
    
    // 设置命令超时
    timeout = { config_.command_timeout_ms / 1000, 
                (config_.command_timeout_ms % 1000) * 1000 };
    redisSetTimeout(ctx, timeout);
    
    return ctx;
}

int RedisConnectionPool::configure_connection(redisContext* ctx) {
    redisReply* reply = nullptr;
    
    // 认证
    if (!config_.password.empty()) {
        reply = (redisReply*)redisCommand(ctx, "AUTH %s", config_.password.c_str());
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            LOG_ERROR("Redis authentication failed: {}", reply ? reply->str : "connection error");
            if (reply) freeReplyObject(reply);
            return MQ_ERR_AUTH;
        }
        freeReplyObject(reply);
    }
    
    // 选择数据库
    if (config_.database != 0) {
        reply = (redisReply*)redisCommand(ctx, "SELECT %d", config_.database);
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            LOG_ERROR("Redis SELECT database failed: {}", reply ? reply->str : "connection error");
            if (reply) freeReplyObject(reply);
            return MQ_ERR_DATABASE;
        }
        freeReplyObject(reply);
    }
    
    return MQ_SUCCESS;
}

//==============================================================================
// RedisAuthProvider Implementation
//==============================================================================

RedisAuthProvider::RedisAuthProvider(const RedisAuthConfig& config, MQTTAllocator* allocator)
    : config_(config), allocator_(allocator) {
    connection_pool_ = std::unique_ptr<RedisConnectionPool>(new RedisConnectionPool(config));
}

RedisAuthProvider::~RedisAuthProvider() {
    cleanup();
}

int RedisAuthProvider::initialize() {
    LOG_INFO("Initializing Redis authentication provider");
    
    int ret = connection_pool_->initialize();
    if (ret != MQ_SUCCESS) {
        LOG_ERROR("Failed to initialize Redis connection pool: {}", ret);
        return ret;
    }
    
    // 测试连接
    if (!connection_pool_->test_connection()) {
        LOG_ERROR("Redis connection test failed");
        return MQ_ERR_CONNECT;
    }
    
    LOG_INFO("Redis authentication provider initialized successfully");
    return MQ_SUCCESS;
}

void RedisAuthProvider::cleanup() {
    LOG_INFO("Cleaning up Redis authentication provider");
    
    if (connection_pool_) {
        connection_pool_->cleanup();
    }
    
    std::unique_lock<mqtt::compat::shared_mutex> lock(local_cache_.mutex);
    local_cache_.users.clear();
}

AuthResult RedisAuthProvider::authenticate_user(const MQTTString& username,
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

AuthResult RedisAuthProvider::check_topic_access(const UserInfo& user_info,
                                                const MQTTString& topic,
                                                Permission permission) {
    AuthResult result = check_topic_access_internal(user_info.username, topic, permission);
    update_stats(false, result == AuthResult::SUCCESS);
    return result;
}

bool RedisAuthProvider::is_super_user(const MQTTString& username) {
    // 先检查本地缓存
    UserCache::UserInfo user_info;
    if (get_from_local_cache(username, user_info)) {
        update_stats(false, false, true);  // 缓存命中
        return user_info.is_super_user;
    }
    
    // 从Redis加载
    if (load_user_from_redis(username, user_info) == MQ_SUCCESS) {
        put_to_local_cache(username, user_info);
        return user_info.is_super_user;
    }
    
    return false;
}

AuthStats RedisAuthProvider::get_stats() const {
    mqtt::CoroLockGuard lock(&stats_mutex_);
    return stats_;
}

void RedisAuthProvider::reset_stats() {
    mqtt::CoroLockGuard lock(&stats_mutex_);
    stats_ = AuthStats();
}

bool RedisAuthProvider::is_healthy() const {
    return connection_pool_ && connection_pool_->test_connection();
}

int RedisAuthProvider::add_user(const MQTTString& username, const MQTTString& password, 
                               bool is_super_user, int ttl_seconds) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire Redis connection");
        return MQ_ERR_CONNECT;
    }
    
    std::string password_hash = hash_password(from_mqtt_string(password));
    std::string user_key = make_user_key(username);
    
    // 使用Redis事务确保原子性
    redisReply* reply = execute_command(conn, "MULTI");
    if (check_redis_reply(reply, "MULTI") != MQ_SUCCESS) {
        free_redis_reply(reply);
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    free_redis_reply(reply);
    
    // 设置用户信息
    reply = execute_command(conn, "HMSET %s password_hash %s is_super_user %d created_at %d updated_at %d",
                           user_key.c_str(), password_hash.c_str(), is_super_user ? 1 : 0,
                           static_cast<int>(time(nullptr)), static_cast<int>(time(nullptr)));
    if (check_redis_reply(reply, "HMSET") != MQ_SUCCESS) {
        free_redis_reply(reply);
        execute_command(conn, "DISCARD");
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    free_redis_reply(reply);
    
    // 设置TTL
    if (ttl_seconds > 0) {
        reply = execute_command(conn, "EXPIRE %s %d", user_key.c_str(), ttl_seconds);
        if (check_redis_reply(reply, "EXPIRE") != MQ_SUCCESS) {
            free_redis_reply(reply);
            execute_command(conn, "DISCARD");
            connection_pool_->release_connection(conn);
            return MQ_ERR_DATABASE;
        }
        free_redis_reply(reply);
    }
    
    // 执行事务
    reply = execute_command(conn, "EXEC");
    if (check_redis_reply(reply, "EXEC") != MQ_SUCCESS) {
        free_redis_reply(reply);
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    free_redis_reply(reply);
    
    connection_pool_->release_connection(conn);
    
    // 清除本地缓存
    remove_from_local_cache(username);
    
    LOG_INFO("User '{}' added to Redis successfully", from_mqtt_string(username));
    return MQ_SUCCESS;
}

int RedisAuthProvider::remove_user(const MQTTString& username) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire Redis connection");
        return MQ_ERR_CONNECT;
    }
    
    std::string user_key = make_user_key(username);
    std::string permissions_key = make_permissions_key(username);
    
    // 使用Redis事务
    redisReply* reply = execute_command(conn, "MULTI");
    if (check_redis_reply(reply, "MULTI") != MQ_SUCCESS) {
        free_redis_reply(reply);
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    free_redis_reply(reply);
    
    // 删除用户信息和权限
    reply = execute_command(conn, "DEL %s %s", user_key.c_str(), permissions_key.c_str());
    if (check_redis_reply(reply, "DEL") != MQ_SUCCESS) {
        free_redis_reply(reply);
        execute_command(conn, "DISCARD");
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    free_redis_reply(reply);
    
    // 执行事务
    reply = execute_command(conn, "EXEC");
    if (check_redis_reply(reply, "EXEC") != MQ_SUCCESS) {
        free_redis_reply(reply);
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    free_redis_reply(reply);
    
    connection_pool_->release_connection(conn);
    
    // 清除本地缓存
    remove_from_local_cache(username);
    
    LOG_INFO("User '{}' removed from Redis successfully", from_mqtt_string(username));
    return MQ_SUCCESS;
}

int RedisAuthProvider::update_user_password(const MQTTString& username, const MQTTString& new_password) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire Redis connection");
        return MQ_ERR_CONNECT;
    }
    
    std::string password_hash = hash_password(from_mqtt_string(new_password));
    std::string user_key = make_user_key(username);
    
    redisReply* reply = execute_command(conn, "HMSET %s password_hash %s updated_at %d",
                                       user_key.c_str(), password_hash.c_str(), 
                                       static_cast<int>(time(nullptr)));
    
    int result = check_redis_reply(reply, "HMSET");
    free_redis_reply(reply);
    connection_pool_->release_connection(conn);
    
    if (result == MQ_SUCCESS) {
        // 清除本地缓存
        remove_from_local_cache(username);
        LOG_INFO("Password updated for user '{}'", from_mqtt_string(username));
    }
    
    return result;
}

int RedisAuthProvider::add_topic_permission(const MQTTString& username, const MQTTString& topic_pattern, 
                                           Permission permission, int ttl_seconds) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire Redis connection");
        return MQ_ERR_CONNECT;
    }
    
    std::string permissions_key = make_permissions_key(username);
    std::string permission_value = from_mqtt_string(topic_pattern) + ":" + std::to_string(static_cast<int>(permission));
    
    redisReply* reply = execute_command(conn, "SADD %s %s", permissions_key.c_str(), permission_value.c_str());
    
    int result = check_redis_reply(reply, "SADD");
    free_redis_reply(reply);
    
    // 设置TTL
    if (result == MQ_SUCCESS && ttl_seconds > 0) {
        reply = execute_command(conn, "EXPIRE %s %d", permissions_key.c_str(), ttl_seconds);
        int ttl_result = check_redis_reply(reply, "EXPIRE");
        free_redis_reply(reply);
        if (ttl_result != MQ_SUCCESS) {
            LOG_WARN("Failed to set TTL for permissions key: {}", permissions_key);
        }
    }
    
    connection_pool_->release_connection(conn);
    
    if (result == MQ_SUCCESS) {
        // 清除本地缓存
        remove_from_local_cache(username);
        LOG_INFO("Topic permission added for user '{}': {} -> {}", 
                 from_mqtt_string(username), from_mqtt_string(topic_pattern), static_cast<int>(permission));
    }
    
    return result;
}

int RedisAuthProvider::remove_topic_permission(const MQTTString& username, const MQTTString& topic_pattern) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire Redis connection");
        return MQ_ERR_CONNECT;
    }
    
    std::string permissions_key = make_permissions_key(username);
    
    // 需要找到匹配的权限条目并删除
    redisReply* reply = execute_command(conn, "SMEMBERS %s", permissions_key.c_str());
    if (check_redis_reply(reply, "SMEMBERS") != MQ_SUCCESS) {
        free_redis_reply(reply);
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    
    std::string topic_pattern_str = from_mqtt_string(topic_pattern);
    std::vector<std::string> to_remove;
    
    for (size_t i = 0; i < reply->elements; ++i) {
        std::string member = reply->element[i]->str;
        if (member.substr(0, member.find(':')) == topic_pattern_str) {
            to_remove.push_back(member);
        }
    }
    
    free_redis_reply(reply);
    
    // 删除匹配的权限
    for (const auto& member : to_remove) {
        reply = execute_command(conn, "SREM %s %s", permissions_key.c_str(), member.c_str());
        if (check_redis_reply(reply, "SREM") != MQ_SUCCESS) {
            LOG_WARN("Failed to remove permission: {}", member);
        }
        free_redis_reply(reply);
    }
    
    connection_pool_->release_connection(conn);
    
    // 清除本地缓存
    remove_from_local_cache(username);
    
    LOG_INFO("Topic permission removed for user '{}': {}", 
             from_mqtt_string(username), from_mqtt_string(topic_pattern));
    return MQ_SUCCESS;
}

int RedisAuthProvider::clear_user_permissions(const MQTTString& username) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire Redis connection");
        return MQ_ERR_CONNECT;
    }
    
    std::string permissions_key = make_permissions_key(username);
    
    redisReply* reply = execute_command(conn, "DEL %s", permissions_key.c_str());
    int result = check_redis_reply(reply, "DEL");
    free_redis_reply(reply);
    
    connection_pool_->release_connection(conn);
    
    if (result == MQ_SUCCESS) {
        // 清除本地缓存
        remove_from_local_cache(username);
        LOG_INFO("All permissions cleared for user '{}'", from_mqtt_string(username));
    }
    
    return result;
}

int RedisAuthProvider::get_user_permissions(const MQTTString& username, std::vector<TopicPermission>& permissions) {
    // 先检查本地缓存
    UserCache::UserInfo user_info;
    if (get_from_local_cache(username, user_info)) {
        permissions = user_info.permissions;
        update_stats(false, false, true);  // 缓存命中
        return MQ_SUCCESS;
    }
    
    // 从Redis加载
    int result = load_user_permissions_from_redis(username, permissions);
    if (result == MQ_SUCCESS) {
        // 更新本地缓存
        user_info.permissions = permissions;
        user_info.expire_time = std::chrono::steady_clock::now() + 
                               std::chrono::seconds(config_.cache_ttl_seconds);
        put_to_local_cache(username, user_info);
    }
    
    return result;
}

int RedisAuthProvider::set_user_permissions(const MQTTString& username, 
                                           const std::vector<TopicPermission>& permissions,
                                           int ttl_seconds) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire Redis connection");
        return MQ_ERR_CONNECT;
    }
    
    std::string permissions_key = make_permissions_key(username);
    
    // 使用事务确保原子性
    redisReply* reply = execute_command(conn, "MULTI");
    if (check_redis_reply(reply, "MULTI") != MQ_SUCCESS) {
        free_redis_reply(reply);
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    free_redis_reply(reply);
    
    // 清空现有权限
    reply = execute_command(conn, "DEL %s", permissions_key.c_str());
    if (check_redis_reply(reply, "DEL") != MQ_SUCCESS) {
        free_redis_reply(reply);
        execute_command(conn, "DISCARD");
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    free_redis_reply(reply);
    
    // 添加新权限
    for (const auto& perm : permissions) {
        std::string permission_value = from_mqtt_string(perm.topic_pattern) + ":" + 
                                     std::to_string(static_cast<int>(perm.permission));
        reply = execute_command(conn, "SADD %s %s", permissions_key.c_str(), permission_value.c_str());
        if (check_redis_reply(reply, "SADD") != MQ_SUCCESS) {
            free_redis_reply(reply);
            execute_command(conn, "DISCARD");
            connection_pool_->release_connection(conn);
            return MQ_ERR_DATABASE;
        }
        free_redis_reply(reply);
    }
    
    // 设置TTL
    if (ttl_seconds > 0) {
        reply = execute_command(conn, "EXPIRE %s %d", permissions_key.c_str(), ttl_seconds);
        if (check_redis_reply(reply, "EXPIRE") != MQ_SUCCESS) {
            free_redis_reply(reply);
            execute_command(conn, "DISCARD");
            connection_pool_->release_connection(conn);
            return MQ_ERR_DATABASE;
        }
        free_redis_reply(reply);
    }
    
    // 执行事务
    reply = execute_command(conn, "EXEC");
    if (check_redis_reply(reply, "EXEC") != MQ_SUCCESS) {
        free_redis_reply(reply);
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    free_redis_reply(reply);
    
    connection_pool_->release_connection(conn);
    
    // 清除本地缓存
    remove_from_local_cache(username);
    
    LOG_INFO("Set {} permissions for user '{}'", permissions.size(), from_mqtt_string(username));
    return MQ_SUCCESS;
}

std::string RedisAuthProvider::hash_password(const std::string& password) {
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

bool RedisAuthProvider::verify_password(const std::string& password, const std::string& hash) {
    return hash_password(password) == hash;
}

std::string RedisAuthProvider::make_user_key(const MQTTString& username) {
    return config_.key_prefix + "user:" + from_mqtt_string(username);
}

std::string RedisAuthProvider::make_permissions_key(const MQTTString& username) {
    return config_.key_prefix + "permissions:" + from_mqtt_string(username);
}

std::string RedisAuthProvider::make_topic_index_key(const MQTTString& topic_pattern) {
    return config_.key_prefix + "topic_index:" + from_mqtt_string(topic_pattern);
}

AuthResult RedisAuthProvider::authenticate_user_internal(const MQTTString& username,
                                                        const MQTTString& password,
                                                        UserInfo& user_info) {
    // 先检查本地缓存
    UserCache::UserInfo cached_user;
    if (get_from_local_cache(username, cached_user)) {
        if (verify_password(from_mqtt_string(password), cached_user.password_hash)) {
            user_info.username = username;
            user_info.is_super_user = cached_user.is_super_user;
            update_stats(true, false, true);  // 登录成功，缓存命中
            return AuthResult::SUCCESS;
        } else {
            update_stats(false, false, true);  // 登录失败，缓存命中
            return AuthResult::INVALID_CREDENTIALS;
        }
    }
    
    // 从Redis加载用户信息
    if (load_user_from_redis(username, cached_user) != MQ_SUCCESS) {
        return AuthResult::USER_NOT_FOUND;
    }
    
    // 验证密码
    if (verify_password(from_mqtt_string(password), cached_user.password_hash)) {
        user_info.username = username;
        user_info.is_super_user = cached_user.is_super_user;
        
        // 缓存用户信息
        put_to_local_cache(username, cached_user);
        
        return AuthResult::SUCCESS;
    } else {
        return AuthResult::INVALID_CREDENTIALS;
    }
}

AuthResult RedisAuthProvider::check_topic_access_internal(const MQTTString& username,
                                                         const MQTTString& topic,
                                                         Permission permission) {
    std::vector<TopicPermission> permissions;
    int result = get_user_permissions(username, permissions);
    if (result != MQ_SUCCESS) {
        return AuthResult::USER_NOT_FOUND;
    }
    
    std::string topic_str = from_mqtt_string(topic);
    
    // 检查权限
    for (const auto& perm : permissions) {
        if (match_topic_pattern(topic_str, from_mqtt_string(perm.topic_pattern))) {
            if ((perm.permission == Permission::READWRITE) ||
                (perm.permission == permission)) {
                return AuthResult::SUCCESS;
            }
        }
    }
    
    return AuthResult::TOPIC_ACCESS_DENIED;
}

int RedisAuthProvider::load_user_from_redis(const MQTTString& username, UserCache::UserInfo& user_info) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire Redis connection");
        return MQ_ERR_CONNECT;
    }
    
    std::string user_key = make_user_key(username);
    
    redisReply* reply = execute_command(conn, "HMGET %s password_hash is_super_user", user_key.c_str());
    if (check_redis_reply(reply, "HMGET") != MQ_SUCCESS) {
        free_redis_reply(reply);
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    
    if (reply->elements < 2 || 
        reply->element[0]->type == REDIS_REPLY_NIL || 
        reply->element[1]->type == REDIS_REPLY_NIL) {
        free_redis_reply(reply);
        connection_pool_->release_connection(conn);
        return MQ_ERR_NOT_FOUND_V2;
    }
    
    user_info.password_hash = reply->element[0]->str;
    user_info.is_super_user = (std::atoi(reply->element[1]->str) != 0);
    user_info.expire_time = std::chrono::steady_clock::now() + 
                           std::chrono::seconds(config_.cache_ttl_seconds);
    
    free_redis_reply(reply);
    connection_pool_->release_connection(conn);
    
    return MQ_SUCCESS;
}

int RedisAuthProvider::load_user_permissions_from_redis(const MQTTString& username, std::vector<TopicPermission>& permissions) {
    auto conn = connection_pool_->acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire Redis connection");
        return MQ_ERR_CONNECT;
    }
    
    std::string permissions_key = make_permissions_key(username);
    
    redisReply* reply = execute_command(conn, "SMEMBERS %s", permissions_key.c_str());
    if (check_redis_reply(reply, "SMEMBERS") != MQ_SUCCESS) {
        free_redis_reply(reply);
        connection_pool_->release_connection(conn);
        return MQ_ERR_DATABASE;
    }
    
    permissions.clear();
    
    for (size_t i = 0; i < reply->elements; ++i) {
        std::string member = reply->element[i]->str;
        size_t colon_pos = member.rfind(':');
        if (colon_pos != std::string::npos) {
            std::string topic_pattern = member.substr(0, colon_pos);
            int perm_value = std::atoi(member.substr(colon_pos + 1).c_str());
            
            TopicPermission perm(allocator_);
            perm.topic_pattern = to_mqtt_string(topic_pattern, allocator_);
            perm.permission = static_cast<Permission>(perm_value);
            permissions.push_back(std::move(perm));
        }
    }
    
    free_redis_reply(reply);
    connection_pool_->release_connection(conn);
    
    return MQ_SUCCESS;
}

bool RedisAuthProvider::get_from_local_cache(const MQTTString& username, UserCache::UserInfo& user_info) {
    mqtt::compat::shared_lock<mqtt::compat::shared_mutex> lock(local_cache_.mutex);
    
    auto it = local_cache_.users.find(from_mqtt_string(username));
    if (it == local_cache_.users.end()) {
        return false;
    }
    
    if (std::chrono::steady_clock::now() > it->second.expire_time) {
        return false;
    }
    
    user_info = it->second;
    return true;
}

void RedisAuthProvider::put_to_local_cache(const MQTTString& username, const UserCache::UserInfo& user_info) {
    std::unique_lock<mqtt::compat::shared_mutex> lock(local_cache_.mutex);
    local_cache_.users[from_mqtt_string(username)] = user_info;
}

void RedisAuthProvider::remove_from_local_cache(const MQTTString& username) {
    std::unique_lock<mqtt::compat::shared_mutex> lock(local_cache_.mutex);
    local_cache_.users.erase(from_mqtt_string(username));
}

void RedisAuthProvider::cleanup_expired_local_cache() {
    std::unique_lock<mqtt::compat::shared_mutex> lock(local_cache_.mutex);
    
    auto now = std::chrono::steady_clock::now();
    auto it = local_cache_.users.begin();
    while (it != local_cache_.users.end()) {
        if (now > it->second.expire_time) {
            it = local_cache_.users.erase(it);
        } else {
            ++it;
        }
    }
}

bool RedisAuthProvider::match_topic_pattern(const std::string& topic, const std::string& pattern) {
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

void RedisAuthProvider::update_stats(bool login_success, bool topic_access_granted, bool cache_hit) {
    mqtt::CoroLockGuard lock(&stats_mutex_);

    if (login_success) {
        stats_.total_login_attempts++;
        stats_.successful_logins++;
    } else if (!topic_access_granted) {  // 只有在非topic检查时才统计登录失败
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

    if (cache_hit) {
        stats_.cache_hits++;
    } else {
        stats_.cache_misses++;
    }
}

redisReply* RedisAuthProvider::execute_command(std::shared_ptr<RedisConnection> conn, const char* format, ...) {
    va_list args;
    va_start(args, format);
    redisReply* reply = (redisReply*)redisvCommand(conn->get_context(), format, args);
    va_end(args);
    return reply;
}

int RedisAuthProvider::check_redis_reply(redisReply* reply, const char* operation) {
    if (!reply) {
        LOG_ERROR("Redis {} failed: connection error", operation);
        return MQ_ERR_CONNECT;
    }
    
    if (reply->type == REDIS_REPLY_ERROR) {
        LOG_ERROR("Redis {} failed: {}", operation, reply->str);
        return MQ_ERR_DATABASE;
    }
    
    return MQ_SUCCESS;
}

void RedisAuthProvider::free_redis_reply(redisReply* reply) {
    if (reply) {
        freeReplyObject(reply);
    }
}

} // namespace auth
} // namespace mqtt

#endif // HAVE_HIREDIS
