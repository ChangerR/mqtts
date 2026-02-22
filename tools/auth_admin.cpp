#include "mqtt_allocator.h"
#include "mqtt_auth_interface.h"
#include "mqtt_stl_allocator.h"
#include "logger.h"
#include <iostream>
#include <string>
#include <vector>

#ifdef HAVE_SQLITE3
#include "mqtt_auth_sqlite.h"
using SQLiteAuthProvider = mqtt::auth::SQLiteAuthProvider;
using SQLiteAuthConfig = mqtt::auth::SQLiteAuthConfig;
#endif

#ifdef HAVE_HIREDIS
#include "mqtt_auth_redis.h"
using RedisAuthProvider = mqtt::auth::RedisAuthProvider;
using RedisAuthConfig = mqtt::auth::RedisAuthConfig;
#endif

using namespace mqtt;
using namespace mqtt::auth;

// C++11 compatibility: define make_unique if not available
#if __cplusplus < 201402L
namespace std {
    template<typename T, typename... Args>
    std::unique_ptr<T> make_unique(Args&&... args) {
        return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
    }
}
#endif

class AuthAdmin {
public:
    AuthAdmin() {
        // 获取根分配器
        allocator_ = MQTTMemoryManager::get_instance().get_root_allocator();
    }
    
    ~AuthAdmin() {
#ifdef HAVE_SQLITE3
        if (sqlite_provider_) {
            sqlite_provider_->cleanup();
        }
#endif
#ifdef HAVE_HIREDIS
        if (redis_provider_) {
            redis_provider_->cleanup();
        }
#endif
        // cleanup会在程序结束时自动调用
    }
    
    int init_sqlite(const std::string& db_path) {
#ifdef HAVE_SQLITE3
        SQLiteAuthConfig config;
        config.db_path = db_path;
        config.connection_pool_size = 3;
        
        sqlite_provider_ = std::make_unique<SQLiteAuthProvider>(config, allocator_);
        int ret = sqlite_provider_->initialize();
        if (ret != MQ_SUCCESS) {
            std::cerr << "Failed to initialize SQLite auth provider: " << ret << std::endl;
            return ret;
        }
        
        std::cout << "SQLite auth provider initialized successfully" << std::endl;
        return MQ_SUCCESS;
#else
        std::cerr << "SQLite support not compiled in. Please install libsqlite3-dev and recompile." << std::endl;
        return -1;
#endif
    }
    
    int init_redis(const std::string& host, int port, const std::string& password = "") {
#ifdef HAVE_HIREDIS
        RedisAuthConfig config;
        config.host = host;
        config.port = port;
        config.password = password;
        config.connection_pool_size = 3;
        
        redis_provider_ = std::make_unique<RedisAuthProvider>(config, allocator_);
        int ret = redis_provider_->initialize();
        if (ret != MQ_SUCCESS) {
            std::cerr << "Failed to initialize Redis auth provider: " << ret << std::endl;
            return ret;
        }
        
        std::cout << "Redis auth provider initialized successfully" << std::endl;
        return MQ_SUCCESS;
#else
        std::cerr << "Redis support not compiled in. Please install libhiredis-dev and recompile." << std::endl;
        return -1;
#endif
    }
    
    void show_help() {
        std::cout << "MQTT Authentication Admin Tool" << std::endl;
        std::cout << "Usage: auth_admin <provider> <command> [options]" << std::endl;
        std::cout << std::endl;
        std::cout << "Providers:" << std::endl;
        std::cout << "  sqlite <db_path>               Use SQLite provider" << std::endl;
        std::cout << "  redis <host> <port> [password] Use Redis provider" << std::endl;
        std::cout << std::endl;
        std::cout << "Commands:" << std::endl;
        std::cout << "  add-user <username> <password> [super]  Add a new user" << std::endl;
        std::cout << "  remove-user <username>                  Remove a user" << std::endl;
        std::cout << "  update-password <username> <password>   Update user password" << std::endl;
        std::cout << "  add-topic <username> <pattern> <perm>   Add topic permission (read/write/readwrite)" << std::endl;
        std::cout << "  remove-topic <username> <pattern>       Remove topic permission" << std::endl;
        std::cout << "  clear-topics <username>                 Clear all topic permissions" << std::endl;
        std::cout << "  list-stats                               Show authentication statistics" << std::endl;
        std::cout << "  test-auth <username> <password>         Test user authentication" << std::endl;
        std::cout << "  test-topic <username> <topic> <perm>    Test topic permission" << std::endl;
        std::cout << std::endl;
        std::cout << "Examples:" << std::endl;
        std::cout << "  auth_admin sqlite auth.db add-user admin password123 super" << std::endl;
        std::cout << "  auth_admin redis 127.0.0.1 6379 add-topic user1 \"sensor/+\" read" << std::endl;
    }
    
    int add_user(const std::string& username, const std::string& password, bool is_super_user = false) {
        MQTTString mqtt_username = to_mqtt_string(username, allocator_);
        MQTTString mqtt_password = to_mqtt_string(password, allocator_);
        
        int ret = -1;
        
#ifdef HAVE_SQLITE3
        if (sqlite_provider_) {
            ret = sqlite_provider_->add_user(mqtt_username, mqtt_password, is_super_user);
        }
#endif
#ifdef HAVE_HIREDIS
        if (redis_provider_) {
            ret = redis_provider_->add_user(mqtt_username, mqtt_password, is_super_user);
        }
#endif
        
        if (ret == MQ_SUCCESS) {
            std::cout << "User '" << username << "' added successfully" << 
                         (is_super_user ? " (super user)" : "") << std::endl;
        } else if (ret == -1) {
            std::cerr << "No provider initialized" << std::endl;
        } else {
            std::cerr << "Failed to add user '" << username << "': " << ret << std::endl;
        }
        
        return ret;
    }
    
    int remove_user(const std::string& username) {
        MQTTString mqtt_username = to_mqtt_string(username, allocator_);
        
        int ret = -1;
        
#ifdef HAVE_SQLITE3
        if (sqlite_provider_) {
            ret = sqlite_provider_->remove_user(mqtt_username);
        }
#endif
#ifdef HAVE_HIREDIS
        if (redis_provider_) {
            ret = redis_provider_->remove_user(mqtt_username);
        }
#endif
        
        if (ret == MQ_SUCCESS) {
            std::cout << "User '" << username << "' removed successfully" << std::endl;
        } else if (ret == -1) {
            std::cerr << "No provider initialized" << std::endl;
        } else {
            std::cerr << "Failed to remove user '" << username << "': " << ret << std::endl;
        }
        
        return ret;
    }
    
    int update_password(const std::string& username, const std::string& new_password) {
        MQTTString mqtt_username = to_mqtt_string(username, allocator_);
        MQTTString mqtt_password = to_mqtt_string(new_password, allocator_);
        
        int ret = -1;
        
#ifdef HAVE_SQLITE3
        if (sqlite_provider_) {
            ret = sqlite_provider_->update_user_password(mqtt_username, mqtt_password);
        }
#endif
#ifdef HAVE_HIREDIS
        if (redis_provider_) {
            ret = redis_provider_->update_user_password(mqtt_username, mqtt_password);
        }
#endif
        
        if (ret == MQ_SUCCESS) {
            std::cout << "Password updated for user '" << username << "'" << std::endl;
        } else if (ret == -1) {
            std::cerr << "No provider initialized" << std::endl;
        } else {
            std::cerr << "Failed to update password for user '" << username << "': " << ret << std::endl;
        }
        
        return ret;
    }
    
    int add_topic_permission(const std::string& username, const std::string& topic_pattern, const std::string& permission) {
        Permission perm;
        if (permission == "read") {
            perm = Permission::READ;
        } else if (permission == "write") {
            perm = Permission::WRITE;
        } else if (permission == "readwrite") {
            perm = Permission::READWRITE;
        } else {
            std::cerr << "Invalid permission: " << permission << " (use read/write/readwrite)" << std::endl;
            return -1;
        }
        
        MQTTString mqtt_username = to_mqtt_string(username, allocator_);
        MQTTString mqtt_topic = to_mqtt_string(topic_pattern, allocator_);
        
        int ret = -1;
        
#ifdef HAVE_SQLITE3
        if (sqlite_provider_) {
            ret = sqlite_provider_->add_topic_permission(mqtt_username, mqtt_topic, perm);
        }
#endif
#ifdef HAVE_HIREDIS
        if (redis_provider_) {
            ret = redis_provider_->add_topic_permission(mqtt_username, mqtt_topic, perm);
        }
#endif
        
        if (ret == MQ_SUCCESS) {
            std::cout << "Topic permission added: " << username << " -> " << topic_pattern 
                     << " (" << permission << ")" << std::endl;
        } else if (ret == -1) {
            std::cerr << "No provider initialized" << std::endl;
        } else {
            std::cerr << "Failed to add topic permission: " << ret << std::endl;
        }
        
        return ret;
    }
    
    int remove_topic_permission(const std::string& username, const std::string& topic_pattern) {
        MQTTString mqtt_username = to_mqtt_string(username, allocator_);
        MQTTString mqtt_topic = to_mqtt_string(topic_pattern, allocator_);
        
        int ret = -1;
        
#ifdef HAVE_SQLITE3
        if (sqlite_provider_) {
            ret = sqlite_provider_->remove_topic_permission(mqtt_username, mqtt_topic);
        }
#endif
#ifdef HAVE_HIREDIS
        if (redis_provider_) {
            ret = redis_provider_->remove_topic_permission(mqtt_username, mqtt_topic);
        }
#endif
        
        if (ret == MQ_SUCCESS) {
            std::cout << "Topic permission removed: " << username << " -> " << topic_pattern << std::endl;
        } else if (ret == -1) {
            std::cerr << "No provider initialized" << std::endl;
        } else {
            std::cerr << "Failed to remove topic permission: " << ret << std::endl;
        }
        
        return ret;
    }
    
    int clear_topics(const std::string& username) {
        MQTTString mqtt_username = to_mqtt_string(username, allocator_);
        
        int ret = -1;
        
#ifdef HAVE_SQLITE3
        if (sqlite_provider_) {
            ret = sqlite_provider_->clear_user_permissions(mqtt_username);
        }
#endif
#ifdef HAVE_HIREDIS
        if (redis_provider_) {
            ret = redis_provider_->clear_user_permissions(mqtt_username);
        }
#endif
        
        if (ret == MQ_SUCCESS) {
            std::cout << "All topic permissions cleared for user '" << username << "'" << std::endl;
        } else if (ret == -1) {
            std::cerr << "No provider initialized" << std::endl;
        } else {
            std::cerr << "Failed to clear topic permissions: " << ret << std::endl;
        }
        
        return ret;
    }
    
    void show_stats() {
        auto provider = get_current_provider();
        if (!provider) {
            std::cerr << "No provider initialized" << std::endl;
            return;
        }
        
        AuthStats stats = provider->get_stats();
        
        std::cout << "Authentication Statistics (" << provider->get_provider_name() << "):" << std::endl;
        std::cout << "  Total login attempts: " << stats.total_login_attempts << std::endl;
        std::cout << "  Successful logins: " << stats.successful_logins << std::endl;
        std::cout << "  Failed logins: " << stats.failed_logins << std::endl;
        std::cout << "  Total topic checks: " << stats.total_topic_checks << std::endl;
        std::cout << "  Topic access granted: " << stats.topic_access_granted << std::endl;
        std::cout << "  Topic access denied: " << stats.topic_access_denied << std::endl;
        std::cout << "  Cache hits: " << stats.cache_hits << std::endl;
        std::cout << "  Cache misses: " << stats.cache_misses << std::endl;
        std::cout << "  Provider healthy: " << (provider->is_healthy() ? "Yes" : "No") << std::endl;
    }
    
    int test_authentication(const std::string& username, const std::string& password) {
        auto provider = get_current_provider();
        if (!provider) {
            std::cerr << "No provider initialized" << std::endl;
            return -1;
        }
        
        MQTTString mqtt_username = to_mqtt_string(username, allocator_);
        MQTTString mqtt_password = to_mqtt_string(password, allocator_);
        MQTTString client_id = to_mqtt_string("test_client", allocator_);
        MQTTString client_ip = to_mqtt_string("127.0.0.1", allocator_);
        
        UserInfo user_info(allocator_);
        AuthResult result = provider->authenticate_user(mqtt_username, mqtt_password, 
                                                       client_id, client_ip, 1883, user_info);
        
        std::cout << "Authentication test for user '" << username << "': ";
        switch (result) {
            case AuthResult::SUCCESS:
                std::cout << "SUCCESS" << std::endl;
                std::cout << "  User info:" << std::endl;
                std::cout << "    Username: " << from_mqtt_string(user_info.username) << std::endl;
                std::cout << "    Super user: " << (user_info.is_super_user ? "Yes" : "No") << std::endl;
                break;
            case AuthResult::INVALID_CREDENTIALS:
                std::cout << "FAILED - Invalid credentials" << std::endl;
                break;
            case AuthResult::USER_NOT_FOUND:
                std::cout << "FAILED - User not found" << std::endl;
                break;
            case AuthResult::ACCESS_DENIED:
                std::cout << "FAILED - Access denied" << std::endl;
                break;
            default:
                std::cout << "FAILED - Internal error" << std::endl;
                break;
        }
        
        return result == AuthResult::SUCCESS ? 0 : -1;
    }
    
    int test_topic_permission(const std::string& username, const std::string& topic, const std::string& permission) {
        auto provider = get_current_provider();
        if (!provider) {
            std::cerr << "No provider initialized" << std::endl;
            return -1;
        }
        
        Permission perm;
        if (permission == "read") {
            perm = Permission::READ;
        } else if (permission == "write") {
            perm = Permission::WRITE;
        } else if (permission == "readwrite") {
            perm = Permission::READWRITE;
        } else {
            std::cerr << "Invalid permission: " << permission << " (use read/write/readwrite)" << std::endl;
            return -1;
        }
        
        UserInfo user_info(allocator_);
        user_info.username = to_mqtt_string(username, allocator_);
        user_info.is_super_user = provider->is_super_user(user_info.username);
        
        MQTTString mqtt_topic = to_mqtt_string(topic, allocator_);
        
        AuthResult result = provider->check_topic_access(user_info, mqtt_topic, perm);
        
        std::cout << "Topic permission test for user '" << username << "' on topic '" 
                 << topic << "' with permission '" << permission << "': ";
        
        switch (result) {
            case AuthResult::SUCCESS:
                std::cout << "GRANTED" << std::endl;
                break;
            case AuthResult::TOPIC_ACCESS_DENIED:
                std::cout << "DENIED" << std::endl;
                break;
            case AuthResult::USER_NOT_FOUND:
                std::cout << "USER NOT FOUND" << std::endl;
                break;
            default:
                std::cout << "ERROR" << std::endl;
                break;
        }
        
        return result == AuthResult::SUCCESS ? 0 : -1;
    }

private:
    MQTTAllocator* allocator_;
#ifdef HAVE_SQLITE3
    std::unique_ptr<SQLiteAuthProvider> sqlite_provider_;
#endif
#ifdef HAVE_HIREDIS
    std::unique_ptr<RedisAuthProvider> redis_provider_;
#endif
    
    IAuthProvider* get_current_provider() {
#ifdef HAVE_SQLITE3
        if (sqlite_provider_) return sqlite_provider_.get();
#endif
#ifdef HAVE_HIREDIS
        if (redis_provider_) return redis_provider_.get();
#endif
        return nullptr;
    }
};

int main(int argc, char* argv[]) {
    // 日志系统通过单例自动初始化
    
    AuthAdmin admin;
    
    if (argc < 3) {
        admin.show_help();
        return 1;
    }
    
    std::string provider_type = argv[1];
    std::string command = argv[2];
    
    // 初始化提供者
    if (provider_type == "sqlite") {
        if (argc < 4) {
            std::cerr << "SQLite provider requires database path" << std::endl;
            return 1;
        }
        std::string db_path = argv[3];
        if (admin.init_sqlite(db_path) != MQ_SUCCESS) {
            return 1;
        }
        // 跳过provider参数
        argv += 2;
        argc -= 2;
    } else if (provider_type == "redis") {
        if (argc < 5) {
            std::cerr << "Redis provider requires host and port" << std::endl;
            return 1;
        }
        std::string host = argv[3];
        int port = std::atoi(argv[4]);
        std::string password = (argc > 5) ? argv[5] : "";
        
        if (admin.init_redis(host, port, password) != MQ_SUCCESS) {
            return 1;
        }
        
        // 跳过provider参数
        int skip = password.empty() ? 3 : 4;
        argv += skip;
        argc -= skip;
    } else {
        std::cerr << "Unknown provider: " << provider_type << std::endl;
        admin.show_help();
        return 1;
    }
    
    // 执行命令
    try {
        if (command == "add-user") {
            if (argc < 4) {
                std::cerr << "add-user requires username and password" << std::endl;
                return 1;
            }
            std::string username = argv[2];
            std::string password = argv[3];
            bool is_super = (argc > 4 && std::string(argv[4]) == "super");
            return admin.add_user(username, password, is_super);
            
        } else if (command == "remove-user") {
            if (argc < 3) {
                std::cerr << "remove-user requires username" << std::endl;
                return 1;
            }
            return admin.remove_user(argv[2]);
            
        } else if (command == "update-password") {
            if (argc < 4) {
                std::cerr << "update-password requires username and new password" << std::endl;
                return 1;
            }
            return admin.update_password(argv[2], argv[3]);
            
        } else if (command == "add-topic") {
            if (argc < 5) {
                std::cerr << "add-topic requires username, topic pattern, and permission" << std::endl;
                return 1;
            }
            return admin.add_topic_permission(argv[2], argv[3], argv[4]);
            
        } else if (command == "remove-topic") {
            if (argc < 4) {
                std::cerr << "remove-topic requires username and topic pattern" << std::endl;
                return 1;
            }
            return admin.remove_topic_permission(argv[2], argv[3]);
            
        } else if (command == "clear-topics") {
            if (argc < 3) {
                std::cerr << "clear-topics requires username" << std::endl;
                return 1;
            }
            return admin.clear_topics(argv[2]);
            
        } else if (command == "list-stats") {
            admin.show_stats();
            return 0;
            
        } else if (command == "test-auth") {
            if (argc < 4) {
                std::cerr << "test-auth requires username and password" << std::endl;
                return 1;
            }
            return admin.test_authentication(argv[2], argv[3]);
            
        } else if (command == "test-topic") {
            if (argc < 5) {
                std::cerr << "test-topic requires username, topic, and permission" << std::endl;
                return 1;
            }
            return admin.test_topic_permission(argv[2], argv[3], argv[4]);
            
        } else {
            std::cerr << "Unknown command: " << command << std::endl;
            admin.show_help();
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}