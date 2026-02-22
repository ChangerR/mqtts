#include "mqtt_auth_interface.h"
#include "mqtt_auth_manager.cpp"
#include "mqtt_allocator.h"
#include "mqtt_string_utils.h"
#include "mqtt_stl_allocator.h"
#include "logger.h"
#include <cassert>
#include <iostream>
#include <memory>

using namespace mqtt::auth;
using mqtt::MQTTString;
using mqtt::MQTTStrAllocator;
using mqtt::from_mqtt_string;
using mqtt::to_mqtt_string;

// Mock认证提供者用于测试
class MockAuthProvider : public IAuthProvider {
public:
    MockAuthProvider(const std::string& name, MQTTAllocator* allocator)
        : name_(name), allocator_(allocator), initialized_(false), stats_() {
        // 添加一些测试用户
        test_users_["admin"] = {"admin_hash", true};
        test_users_["user1"] = {"user1_hash", false};
        test_users_["user2"] = {"user2_hash", false};
        
        // 添加一些测试权限
        TopicPermission perm1(allocator);
        perm1.topic_pattern = MQTTString("test/+", MQTTStrAllocator(allocator));
        perm1.permission = Permission::READWRITE;
        test_permissions_["user1"].push_back(perm1);
        
        TopicPermission perm2(allocator);
        perm2.topic_pattern = MQTTString("home/#", MQTTStrAllocator(allocator));
        perm2.permission = Permission::READ;
        test_permissions_["user2"].push_back(perm2);
    }

    int initialize() override {
        initialized_ = true;
        return MQ_SUCCESS;
    }

    void cleanup() override {
        initialized_ = false;
    }

    AuthResult authenticate_user(const MQTTString& username,
                                const MQTTString& password,
                                const MQTTString& client_id,
                                const MQTTString& client_ip,
                                uint16_t client_port,
                                UserInfo& user_info) override {
        stats_.total_login_attempts++;
        
        std::string user_str = from_mqtt_string(username);
        std::string pass_str = from_mqtt_string(password);
        
        auto it = test_users_.find(user_str);
        if (it == test_users_.end()) {
            stats_.failed_logins++;
            return AuthResult::USER_NOT_FOUND;
        }
        
        // 简单的密码验证（明文比较用于测试）
        if (pass_str != user_str + "_password") {
            stats_.failed_logins++;
            return AuthResult::INVALID_CREDENTIALS;
        }
        
        user_info.username = username;
        user_info.client_id = client_id;
        user_info.client_ip = client_ip;
        user_info.client_port = client_port;
        user_info.is_super_user = it->second.is_super_user;
        
        stats_.successful_logins++;
        return AuthResult::SUCCESS;
    }

    AuthResult check_topic_access(const UserInfo& user_info,
                                 const MQTTString& topic,
                                 Permission permission) override {
        stats_.total_topic_checks++;
        
        if (user_info.is_super_user) {
            stats_.topic_access_granted++;
            return AuthResult::SUCCESS;
        }
        
        std::string user_str = from_mqtt_string(user_info.username);
        std::string topic_str = from_mqtt_string(topic);
        
        auto it = test_permissions_.find(user_str);
        if (it == test_permissions_.end()) {
            stats_.topic_access_denied++;
            return AuthResult::TOPIC_ACCESS_DENIED;
        }
        
        for (const auto& perm : it->second) {
            if (match_topic_pattern(topic_str, from_mqtt_string(perm.topic_pattern))) {
                if (perm.permission == Permission::READWRITE || perm.permission == permission) {
                    stats_.topic_access_granted++;
                    return AuthResult::SUCCESS;
                }
            }
        }
        
        stats_.topic_access_denied++;
        return AuthResult::TOPIC_ACCESS_DENIED;
    }

    bool is_super_user(const MQTTString& username) override {
        std::string user_str = from_mqtt_string(username);
        auto it = test_users_.find(user_str);
        return it != test_users_.end() && it->second.is_super_user;
    }

    AuthStats get_stats() const override {
        return stats_;
    }

    void reset_stats() override {
        stats_ = AuthStats();
    }

    const char* get_provider_name() const override {
        return name_.c_str();
    }

    bool is_healthy() const override {
        return initialized_;
    }

private:
    struct TestUser {
        std::string password_hash;
        bool is_super_user;
    };
    
    std::string name_;
    MQTTAllocator* allocator_;
    bool initialized_;
    AuthStats stats_;
    std::map<std::string, TestUser> test_users_;
    std::map<std::string, std::vector<TopicPermission>> test_permissions_;
    
    bool match_topic_pattern(const std::string& topic, const std::string& pattern) {
        if (pattern == "#") return true;
        if (pattern.find('+') == std::string::npos && pattern.find('#') == std::string::npos) {
            return topic == pattern;
        }
        
        // 简单的通配符匹配
        if (pattern == "test/+") {
            return topic.substr(0, 5) == "test/" && topic.find('/', 5) == std::string::npos;
        }
        if (pattern == "home/#") {
            return topic.substr(0, 5) == "home/";
        }
        
        return false;
    }
};

void test_auth_manager_basic() {
    std::cout << "Testing AuthManager basic functionality..." << std::endl;
    
    // 初始化内存管理
    MQTTAllocator* allocator = MQTTMemoryManager::get_instance().get_root_allocator();
    
    AuthManager auth_manager(allocator);
    
    // 测试初始化
    int ret = auth_manager.initialize();
    assert(ret == MQ_SUCCESS);
    
    // 测试添加提供者
    auto provider1 = std::make_unique<MockAuthProvider>("mock1", allocator);
    auto provider2 = std::make_unique<MockAuthProvider>("mock2", allocator);
    
    ret = auth_manager.add_provider(std::move(provider1), 10);
    assert(ret == MQ_SUCCESS);
    
    ret = auth_manager.add_provider(std::move(provider2), 20);
    assert(ret == MQ_SUCCESS);
    
    // 测试认证
    MQTTString username("admin", MQTTStrAllocator(allocator));
    MQTTString password("admin_password", MQTTStrAllocator(allocator));
    MQTTString client_id("test_client", MQTTStrAllocator(allocator));
    MQTTString client_ip("192.168.1.100", MQTTStrAllocator(allocator));
    
    UserInfo user_info(allocator);
    AuthResult result = auth_manager.authenticate_user(username, password, client_id, client_ip, 1883, user_info);
    
    assert(result == AuthResult::SUCCESS);
    assert(from_mqtt_string(user_info.username) == "admin");
    assert(user_info.is_super_user == true);
    assert(user_info.client_port == 1883);
    
    // 测试错误密码
    MQTTString wrong_password("wrong", MQTTStrAllocator(allocator));
    result = auth_manager.authenticate_user(username, wrong_password, client_id, client_ip, 1883, user_info);
    assert(result == AuthResult::INVALID_CREDENTIALS);
    
    // 测试不存在的用户
    MQTTString nonexistent_user("nonexistent", MQTTStrAllocator(allocator));
    result = auth_manager.authenticate_user(nonexistent_user, password, client_id, client_ip, 1883, user_info);
    assert(result == AuthResult::USER_NOT_FOUND);
    
    auth_manager.cleanup();
    std::cout << "✓ AuthManager basic functionality test passed" << std::endl;
}

void test_auth_manager_topic_access() {
    std::cout << "Testing AuthManager topic access control..." << std::endl;
    
    MQTTAllocator* allocator = MQTTMemoryManager::get_instance().get_root_allocator();
    AuthManager auth_manager(allocator);
    
    auth_manager.initialize();
    
    auto provider = std::make_unique<MockAuthProvider>("test_provider", allocator);
    auth_manager.add_provider(std::move(provider), 10);
    
    // 测试超级用户权限
    UserInfo admin_info(allocator);
    admin_info.username = MQTTString("admin", MQTTStrAllocator(allocator));
    admin_info.is_super_user = true;
    
    MQTTString test_topic("any/topic", MQTTStrAllocator(allocator));
    AuthResult result = auth_manager.check_topic_access(admin_info, test_topic, Permission::WRITE);
    assert(result == AuthResult::SUCCESS);
    
    // 测试普通用户权限
    UserInfo user1_info(allocator);
    user1_info.username = MQTTString("user1", MQTTStrAllocator(allocator));
    user1_info.is_super_user = false;
    
    // 有权限的主题
    MQTTString allowed_topic("test/sensor1", MQTTStrAllocator(allocator));
    result = auth_manager.check_topic_access(user1_info, allowed_topic, Permission::READ);
    assert(result == AuthResult::SUCCESS);
    
    // 无权限的主题
    MQTTString denied_topic("private/data", MQTTStrAllocator(allocator));
    result = auth_manager.check_topic_access(user1_info, denied_topic, Permission::READ);
    assert(result == AuthResult::TOPIC_ACCESS_DENIED);
    
    // 测试只读权限
    UserInfo user2_info(allocator);
    user2_info.username = MQTTString("user2", MQTTStrAllocator(allocator));
    user2_info.is_super_user = false;
    
    MQTTString readonly_topic("home/temperature", MQTTStrAllocator(allocator));
    result = auth_manager.check_topic_access(user2_info, readonly_topic, Permission::READ);
    assert(result == AuthResult::SUCCESS);
    
    result = auth_manager.check_topic_access(user2_info, readonly_topic, Permission::WRITE);
    assert(result == AuthResult::TOPIC_ACCESS_DENIED);
    
    auth_manager.cleanup();
    std::cout << "✓ AuthManager topic access control test passed" << std::endl;
}

void test_auth_manager_multiple_providers() {
    std::cout << "Testing AuthManager with multiple providers..." << std::endl;
    
    MQTTAllocator* allocator = MQTTMemoryManager::get_instance().get_root_allocator();
    AuthManager auth_manager(allocator);
    
    auth_manager.initialize();
    
    // 添加多个提供者，测试优先级
    auto provider1 = std::make_unique<MockAuthProvider>("high_priority", allocator);
    auto provider2 = std::make_unique<MockAuthProvider>("low_priority", allocator);
    
    auth_manager.add_provider(std::move(provider1), 5);   // 高优先级
    auth_manager.add_provider(std::move(provider2), 10);  // 低优先级
    
    // 测试认证会优先使用高优先级提供者
    MQTTString username("admin", MQTTStrAllocator(allocator));
    MQTTString password("admin_password", MQTTStrAllocator(allocator));
    MQTTString client_id("test_client", MQTTStrAllocator(allocator));
    MQTTString client_ip("192.168.1.100", MQTTStrAllocator(allocator));
    
    UserInfo user_info(allocator);
    AuthResult result = auth_manager.authenticate_user(username, password, client_id, client_ip, 1883, user_info);
    assert(result == AuthResult::SUCCESS);
    
    // 获取统计信息
    auto all_stats = auth_manager.get_all_stats();
    assert(all_stats.size() == 2);
    assert(all_stats.find("high_priority") != all_stats.end());
    assert(all_stats.find("low_priority") != all_stats.end());
    
    // 高优先级提供者应该有认证记录
    assert(all_stats["high_priority"].total_login_attempts > 0);
    
    auth_manager.cleanup();
    std::cout << "✓ AuthManager multiple providers test passed" << std::endl;
}

void test_auth_manager_cache() {
    std::cout << "Testing AuthManager cache functionality..." << std::endl;
    
    MQTTAllocator* allocator = MQTTMemoryManager::get_instance().get_root_allocator();
    AuthManager auth_manager(allocator);
    
    // 启用缓存
    auth_manager.set_cache_enabled(true, 60);  // 60秒TTL
    
    auth_manager.initialize();
    
    auto provider = std::make_unique<MockAuthProvider>("cache_test", allocator);
    auth_manager.add_provider(std::move(provider), 10);
    
    MQTTString username("user1", MQTTStrAllocator(allocator));
    MQTTString password("user1_password", MQTTStrAllocator(allocator));
    MQTTString client_id("test_client", MQTTStrAllocator(allocator));
    MQTTString client_ip("192.168.1.100", MQTTStrAllocator(allocator));
    
    // 第一次认证（应该从提供者获取）
    UserInfo user_info(allocator);
    AuthResult result = auth_manager.authenticate_user(username, password, client_id, client_ip, 1883, user_info);
    assert(result == AuthResult::SUCCESS);
    
    // 第二次认证（应该从缓存获取）
    UserInfo user_info2(allocator);
    result = auth_manager.authenticate_user(username, password, client_id, client_ip, 1883, user_info2);
    assert(result == AuthResult::SUCCESS);
    
    // 禁用缓存
    auth_manager.set_cache_enabled(false);
    
    auth_manager.cleanup();
    std::cout << "✓ AuthManager cache functionality test passed" << std::endl;
}

void test_auth_manager_error_handling() {
    std::cout << "Testing AuthManager error handling..." << std::endl;
    
    MQTTAllocator* allocator = MQTTMemoryManager::get_instance().get_root_allocator();
    AuthManager auth_manager(allocator);
    
    // 测试没有提供者的情况
    auth_manager.initialize();
    
    MQTTString username("user", MQTTStrAllocator(allocator));
    MQTTString password("password", MQTTStrAllocator(allocator));
    MQTTString client_id("client", MQTTStrAllocator(allocator));
    MQTTString client_ip("127.0.0.1", MQTTStrAllocator(allocator));
    
    UserInfo user_info(allocator);
    AuthResult result = auth_manager.authenticate_user(username, password, client_id, client_ip, 1883, user_info);
    assert(result == AuthResult::ACCESS_DENIED);
    
    // 测试空指针提供者
    int ret = auth_manager.add_provider(nullptr, 10);
    assert(ret == MQ_ERR_INVALID_ARGS);
    
    // 测试重复提供者
    auto provider1 = std::make_unique<MockAuthProvider>("duplicate", allocator);
    auto provider2 = std::make_unique<MockAuthProvider>("duplicate", allocator);
    
    ret = auth_manager.add_provider(std::move(provider1), 10);
    assert(ret == MQ_SUCCESS);
    
    ret = auth_manager.add_provider(std::move(provider2), 10);
    assert(ret == MQ_ERR_INTERNAL);
    
    // 测试移除不存在的提供者
    ret = auth_manager.remove_provider("nonexistent");
    assert(ret == MQ_ERR_NOT_FOUND_V2);
    
    auth_manager.cleanup();
    std::cout << "✓ AuthManager error handling test passed" << std::endl;
}

int main() {
    try {
        // 初始化日志系统
        // mqtt::Logger::set_level(mqtt::LogLevel::INFO);
        
        std::cout << "Starting AuthManager unit tests..." << std::endl;
        
        test_auth_manager_basic();
        test_auth_manager_topic_access();
        test_auth_manager_multiple_providers();
        test_auth_manager_cache();
        test_auth_manager_error_handling();
        
        std::cout << "All AuthManager tests passed!" << std::endl;
        
        // 清理内存管理
        // mqtt::MQTTMemoryManager::get_instance().cleanup();
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}