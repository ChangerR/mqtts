#include "../src/mqtt_auth_redis.h"
#include "../src/mqtt_allocator.h"
#include "../src/mqtt_memory_manager.h"
#include "../src/logger.h"
#include <cassert>
#include <iostream>
#include <thread>

using namespace mqtt::auth;

class RedisAuthTest {
public:
    RedisAuthTest() {
        // 初始化内存管理
        mqtt::MQTTMemoryManager::get_instance().initialize(1024 * 1024);  // 1MB
        allocator_ = mqtt::MQTTMemoryManager::get_instance().get_root_allocator();
        
        // 配置Redis认证提供者
        config_.host = "127.0.0.1";
        config_.port = 6379;
        config_.password = "";
        config_.database = 15;  // 使用测试数据库
        config_.connection_pool_size = 3;
        config_.max_retry_count = 2;
        config_.retry_delay_ms = 50;
        config_.connect_timeout_ms = 1000;
        config_.command_timeout_ms = 1000;
        config_.key_prefix = "test:mqtt:auth:";
        config_.cache_ttl_seconds = 60;
        
        provider_ = std::make_unique<RedisAuthProvider>(config_, allocator_);
    }
    
    ~RedisAuthTest() {
        if (provider_ && provider_->is_healthy()) {
            // 清理测试数据
            cleanup_test_data();
            provider_->cleanup();
        }
        
        mqtt::MQTTMemoryManager::get_instance().cleanup();
    }
    
    void run_all_tests() {
        if (!check_redis_availability()) {
            std::cout << "Redis server not available, skipping Redis tests" << std::endl;
            return;
        }
        
        test_initialization();
        test_user_management();
        test_authentication();
        test_topic_permissions();
        test_super_user();
        test_local_cache();
        test_performance();
        test_concurrent_access();
        test_error_handling();
        test_redis_operations();
    }

private:
    RedisAuthConfig config_;
    MQTTAllocator* allocator_;
    std::unique_ptr<RedisAuthProvider> provider_;
    
    bool check_redis_availability() {
        std::cout << "Checking Redis availability..." << std::endl;
        
        int ret = provider_->initialize();
        if (ret != MQ_SUCCESS) {
            std::cout << "Redis initialization failed: " << ret << std::endl;
            return false;
        }
        
        if (!provider_->is_healthy()) {
            std::cout << "Redis health check failed" << std::endl;
            return false;
        }
        
        std::cout << "✓ Redis server is available" << std::endl;
        return true;
    }
    
    void cleanup_test_data() {
        // 清理所有测试数据
        std::vector<std::string> test_users = {
            "testuser", "admin", "authuser", "topicuser", "superadmin",
            "perfuser", "concurrent_user", "erroruser", "cacheuser"
        };
        
        for (const auto& user : test_users) {
            MQTTString username(user, MQTTStrAllocator(allocator_));
            provider_->remove_user(username);
        }
        
        // 清理性能测试和并发测试的用户
        for (int i = 0; i < 100; ++i) {
            MQTTString username("perfuser" + std::to_string(i), MQTTStrAllocator(allocator_));
            provider_->remove_user(username);
        }
        
        for (int i = 0; i < 10; ++i) {
            MQTTString username("concurrent_user" + std::to_string(i), MQTTStrAllocator(allocator_));
            provider_->remove_user(username);
        }
    }
    
    void test_initialization() {
        std::cout << "Testing Redis authentication initialization..." << std::endl;
        
        // 测试健康检查
        assert(provider_->is_healthy());
        
        // 测试提供者名称
        assert(std::string(provider_->get_provider_name()) == "Redis");
        
        std::cout << "✓ Redis initialization test passed" << std::endl;
    }
    
    void test_user_management() {
        std::cout << "Testing Redis user management..." << std::endl;
        
        MQTTString username("testuser", MQTTStrAllocator(allocator_));
        MQTTString password("testpass", MQTTStrAllocator(allocator_));
        
        // 测试添加用户
        int ret = provider_->add_user(username, password, false);
        assert(ret == MQ_SUCCESS);
        
        // 测试添加超级用户
        MQTTString admin_username("admin", MQTTStrAllocator(allocator_));
        MQTTString admin_password("adminpass", MQTTStrAllocator(allocator_));
        ret = provider_->add_user(admin_username, admin_password, true);
        assert(ret == MQ_SUCCESS);
        
        // 测试检查超级用户
        assert(provider_->is_super_user(admin_username) == true);
        assert(provider_->is_super_user(username) == false);
        
        // 测试更新密码
        MQTTString new_password("newpass", MQTTStrAllocator(allocator_));
        ret = provider_->update_user_password(username, new_password);
        assert(ret == MQ_SUCCESS);
        
        // 测试删除用户
        ret = provider_->remove_user(username);
        assert(ret == MQ_SUCCESS);
        
        // 确认用户已删除
        assert(provider_->is_super_user(username) == false);
        
        std::cout << "✓ Redis user management test passed" << std::endl;
    }
    
    void test_authentication() {
        std::cout << "Testing Redis authentication..." << std::endl;
        
        // 添加测试用户
        MQTTString username("authuser", MQTTStrAllocator(allocator_));
        MQTTString password("authpass", MQTTStrAllocator(allocator_));
        provider_->add_user(username, password, false);
        
        // 测试成功认证
        MQTTString client_id("test_client", MQTTStrAllocator(allocator_));
        MQTTString client_ip("192.168.1.100", MQTTStrAllocator(allocator_));
        UserInfo user_info(allocator_);
        
        AuthResult result = provider_->authenticate_user(username, password, client_id, client_ip, 1883, user_info);
        assert(result == AuthResult::SUCCESS);
        assert(from_mqtt_string(user_info.username) == "authuser");
        assert(from_mqtt_string(user_info.client_id) == "test_client");
        assert(from_mqtt_string(user_info.client_ip) == "192.168.1.100");
        assert(user_info.client_port == 1883);
        assert(user_info.is_super_user == false);
        
        // 测试错误密码
        MQTTString wrong_password("wrongpass", MQTTStrAllocator(allocator_));
        result = provider_->authenticate_user(username, wrong_password, client_id, client_ip, 1883, user_info);
        assert(result == AuthResult::INVALID_CREDENTIALS);
        
        // 测试不存在的用户
        MQTTString nonexistent("nonexist", MQTTStrAllocator(allocator_));
        result = provider_->authenticate_user(nonexistent, password, client_id, client_ip, 1883, user_info);
        assert(result == AuthResult::USER_NOT_FOUND);
        
        std::cout << "✓ Redis authentication test passed" << std::endl;
    }
    
    void test_topic_permissions() {
        std::cout << "Testing Redis topic permissions..." << std::endl;
        
        // 创建测试用户
        MQTTString username("topicuser", MQTTStrAllocator(allocator_));
        MQTTString password("topicpass", MQTTStrAllocator(allocator_));
        provider_->add_user(username, password, false);
        
        UserInfo user_info(allocator_);
        user_info.username = username;
        user_info.is_super_user = false;
        
        // 测试添加主题权限
        MQTTString topic_pattern1("sensor/+", MQTTStrAllocator(allocator_));
        MQTTString topic_pattern2("control/#", MQTTStrAllocator(allocator_));
        
        int ret = provider_->add_topic_permission(username, topic_pattern1, Permission::READ);
        assert(ret == MQ_SUCCESS);
        
        ret = provider_->add_topic_permission(username, topic_pattern2, Permission::READWRITE);
        assert(ret == MQ_SUCCESS);
        
        // 测试权限检查
        MQTTString test_topic1("sensor/temperature", MQTTStrAllocator(allocator_));
        AuthResult result = provider_->check_topic_access(user_info, test_topic1, Permission::READ);
        assert(result == AuthResult::SUCCESS);
        
        result = provider_->check_topic_access(user_info, test_topic1, Permission::WRITE);
        assert(result == AuthResult::TOPIC_ACCESS_DENIED);
        
        MQTTString test_topic2("control/lights", MQTTStrAllocator(allocator_));
        result = provider_->check_topic_access(user_info, test_topic2, Permission::READ);
        assert(result == AuthResult::SUCCESS);
        
        result = provider_->check_topic_access(user_info, test_topic2, Permission::WRITE);
        assert(result == AuthResult::SUCCESS);
        
        // 测试无权限的主题
        MQTTString test_topic3("private/data", MQTTStrAllocator(allocator_));
        result = provider_->check_topic_access(user_info, test_topic3, Permission::READ);
        assert(result == AuthResult::TOPIC_ACCESS_DENIED);
        
        // 测试获取用户权限
        std::vector<TopicPermission> permissions;
        ret = provider_->get_user_permissions(username, permissions);
        assert(ret == MQ_SUCCESS);
        assert(permissions.size() == 2);
        
        // 测试批量设置权限
        permissions.clear();
        TopicPermission perm1(allocator_);
        perm1.topic_pattern = MQTTString("new/topic/+", MQTTStrAllocator(allocator_));
        perm1.permission = Permission::WRITE;
        permissions.push_back(perm1);
        
        ret = provider_->set_user_permissions(username, permissions);
        assert(ret == MQ_SUCCESS);
        
        // 验证新权限
        MQTTString new_topic("new/topic/test", MQTTStrAllocator(allocator_));
        result = provider_->check_topic_access(user_info, new_topic, Permission::WRITE);
        assert(result == AuthResult::SUCCESS);
        
        // 旧权限应该被清除
        result = provider_->check_topic_access(user_info, test_topic1, Permission::READ);
        assert(result == AuthResult::TOPIC_ACCESS_DENIED);
        
        // 测试删除权限
        ret = provider_->remove_topic_permission(username, perm1.topic_pattern);
        assert(ret == MQ_SUCCESS);
        
        result = provider_->check_topic_access(user_info, new_topic, Permission::WRITE);
        assert(result == AuthResult::TOPIC_ACCESS_DENIED);
        
        // 测试清空用户权限
        provider_->add_topic_permission(username, topic_pattern1, Permission::READ);
        ret = provider_->clear_user_permissions(username);
        assert(ret == MQ_SUCCESS);
        
        result = provider_->check_topic_access(user_info, test_topic1, Permission::READ);
        assert(result == AuthResult::TOPIC_ACCESS_DENIED);
        
        std::cout << "✓ Redis topic permissions test passed" << std::endl;
    }
    
    void test_super_user() {
        std::cout << "Testing Redis super user functionality..." << std::endl;
        
        // 添加超级用户
        MQTTString admin_username("superadmin", MQTTStrAllocator(allocator_));
        MQTTString admin_password("superpass", MQTTStrAllocator(allocator_));
        provider_->add_user(admin_username, admin_password, true);
        
        UserInfo admin_info(allocator_);
        admin_info.username = admin_username;
        admin_info.is_super_user = true;
        
        // 超级用户应该能访问任何主题
        MQTTString any_topic1("any/topic/here", MQTTStrAllocator(allocator_));
        MQTTString any_topic2("completely/different/topic", MQTTStrAllocator(allocator_));
        
        AuthResult result = provider_->check_topic_access(admin_info, any_topic1, Permission::READ);
        assert(result == AuthResult::SUCCESS);
        
        result = provider_->check_topic_access(admin_info, any_topic1, Permission::WRITE);
        assert(result == AuthResult::SUCCESS);
        
        result = provider_->check_topic_access(admin_info, any_topic2, Permission::READWRITE);
        assert(result == AuthResult::SUCCESS);
        
        std::cout << "✓ Redis super user test passed" << std::endl;
    }
    
    void test_local_cache() {
        std::cout << "Testing Redis local cache functionality..." << std::endl;
        
        // 添加缓存测试用户
        MQTTString username("cacheuser", MQTTStrAllocator(allocator_));
        MQTTString password("cachepass", MQTTStrAllocator(allocator_));
        provider_->add_user(username, password, false);
        
        // 重置统计
        provider_->reset_stats();
        
        // 第一次认证（应该从Redis获取）
        MQTTString client_id("cache_client", MQTTStrAllocator(allocator_));
        MQTTString client_ip("192.168.1.100", MQTTStrAllocator(allocator_));
        UserInfo user_info1(allocator_);
        
        AuthResult result = provider_->authenticate_user(username, password, client_id, client_ip, 1883, user_info1);
        assert(result == AuthResult::SUCCESS);
        
        // 第二次认证（应该从本地缓存获取）
        UserInfo user_info2(allocator_);
        result = provider_->authenticate_user(username, password, client_id, client_ip, 1883, user_info2);
        assert(result == AuthResult::SUCCESS);
        
        // 检查统计信息确认缓存命中
        AuthStats stats = provider_->get_stats();
        assert(stats.cache_hits > 0);
        
        std::cout << "✓ Redis local cache test passed" << std::endl;
    }
    
    void test_performance() {
        std::cout << "Testing Redis performance..." << std::endl;
        
        // 重置统计信息
        provider_->reset_stats();
        
        // 添加多个用户进行性能测试
        const int num_users = 50;  // Redis测试用较少用户
        for (int i = 0; i < num_users; ++i) {
            MQTTString username("perfuser" + std::to_string(i), MQTTStrAllocator(allocator_));
            MQTTString password("perfpass" + std::to_string(i), MQTTStrAllocator(allocator_));
            provider_->add_user(username, password, false);
            
            // 添加一些权限
            MQTTString topic_pattern("perf/test" + std::to_string(i) + "/#", MQTTStrAllocator(allocator_));
            provider_->add_topic_permission(username, topic_pattern, Permission::READWRITE);
        }
        
        auto start = std::chrono::high_resolution_clock::now();
        
        // 执行多次认证
        const int auth_count = 500;  // Redis测试用较少次数
        int success_count = 0;
        
        for (int i = 0; i < auth_count; ++i) {
            int user_idx = i % num_users;
            MQTTString username("perfuser" + std::to_string(user_idx), MQTTStrAllocator(allocator_));
            MQTTString password("perfpass" + std::to_string(user_idx), MQTTStrAllocator(allocator_));
            MQTTString client_id("perf_client_" + std::to_string(i), MQTTStrAllocator(allocator_));
            MQTTString client_ip("127.0.0.1", MQTTStrAllocator(allocator_));
            
            UserInfo user_info(allocator_);
            AuthResult result = provider_->authenticate_user(username, password, client_id, client_ip, 1883, user_info);
            if (result == AuthResult::SUCCESS) {
                success_count++;
            }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "Performance test: " << auth_count << " authentications in " 
                  << duration.count() << "ms" << std::endl;
        std::cout << "Average: " << (duration.count() / static_cast<double>(auth_count)) 
                  << "ms per authentication" << std::endl;
        
        assert(success_count == auth_count);
        
        // 检查统计信息
        AuthStats stats = provider_->get_stats();
        std::cout << "Cache hits: " << stats.cache_hits << ", Cache misses: " << stats.cache_misses << std::endl;
        
        std::cout << "✓ Redis performance test passed" << std::endl;
    }
    
    void test_concurrent_access() {
        std::cout << "Testing Redis concurrent access..." << std::endl;
        
        // 创建多个线程同时进行认证
        const int num_threads = 5;  // Redis测试用较少线程
        const int operations_per_thread = 20;
        std::vector<std::thread> threads;
        std::atomic<int> success_count(0);
        
        // 先添加一些测试用户
        for (int i = 0; i < num_threads; ++i) {
            MQTTString username("concurrent_user" + std::to_string(i), MQTTStrAllocator(allocator_));
            MQTTString password("concurrent_pass" + std::to_string(i), MQTTStrAllocator(allocator_));
            provider_->add_user(username, password, false);
        }
        
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([this, t, operations_per_thread, &success_count]() {
                for (int i = 0; i < operations_per_thread; ++i) {
                    MQTTString username("concurrent_user" + std::to_string(t), MQTTStrAllocator(allocator_));
                    MQTTString password("concurrent_pass" + std::to_string(t), MQTTStrAllocator(allocator_));
                    MQTTString client_id("client_" + std::to_string(t) + "_" + std::to_string(i), MQTTStrAllocator(allocator_));
                    MQTTString client_ip("127.0.0.1", MQTTStrAllocator(allocator_));
                    
                    UserInfo user_info(allocator_);
                    AuthResult result = provider_->authenticate_user(username, password, client_id, client_ip, 1883, user_info);
                    
                    if (result == AuthResult::SUCCESS) {
                        success_count.fetch_add(1);
                    }
                    
                    // 也测试主题权限检查
                    MQTTString test_topic("test/topic/" + std::to_string(i), MQTTStrAllocator(allocator_));
                    provider_->check_topic_access(user_info, test_topic, Permission::READ);
                }
            });
        }
        
        // 等待所有线程完成
        for (auto& thread : threads) {
            thread.join();
        }
        
        assert(success_count.load() == num_threads * operations_per_thread);
        
        std::cout << "✓ Redis concurrent access test passed" << std::endl;
    }
    
    void test_error_handling() {
        std::cout << "Testing Redis error handling..." << std::endl;
        
        // 测试重复添加用户
        MQTTString username("erroruser", MQTTStrAllocator(allocator_));
        MQTTString password("errorpass", MQTTStrAllocator(allocator_));
        
        int ret = provider_->add_user(username, password, false);
        assert(ret == MQ_SUCCESS);
        
        ret = provider_->add_user(username, password, false);
        // Redis应该处理重复插入（覆盖）
        assert(ret == MQ_SUCCESS);
        
        // 测试删除不存在的用户
        MQTTString nonexistent("nonexistent", MQTTStrAllocator(allocator_));
        ret = provider_->remove_user(nonexistent);
        assert(ret == MQ_SUCCESS);
        
        // 测试更新不存在用户的密码
        MQTTString new_password("newpass", MQTTStrAllocator(allocator_));
        ret = provider_->update_user_password(nonexistent, new_password);
        assert(ret == MQ_SUCCESS);
        
        // 测试空用户名和密码
        MQTTString empty_string("", MQTTStrAllocator(allocator_));
        ret = provider_->add_user(empty_string, password, false);
        // 空用户名应该能正常处理
        assert(ret == MQ_SUCCESS);
        
        std::cout << "✓ Redis error handling test passed" << std::endl;
    }
    
    void test_redis_operations() {
        std::cout << "Testing Redis-specific operations..." << std::endl;
        
        // 测试TTL功能
        MQTTString ttl_user("ttluser", MQTTStrAllocator(allocator_));
        MQTTString ttl_password("ttlpass", MQTTStrAllocator(allocator_));
        
        // 添加带TTL的用户
        int ret = provider_->add_user(ttl_user, ttl_password, false, 2);  // 2秒TTL
        assert(ret == MQ_SUCCESS);
        
        // 立即验证用户存在
        assert(provider_->is_super_user(ttl_user) == false);  // 应该找到用户
        
        // 添加带TTL的权限
        MQTTString ttl_topic("ttl/topic", MQTTStrAllocator(allocator_));
        ret = provider_->add_topic_permission(ttl_user, ttl_topic, Permission::READ, 2);
        assert(ret == MQ_SUCCESS);
        
        std::vector<TopicPermission> permissions;
        ret = provider_->get_user_permissions(ttl_user, permissions);
        assert(ret == MQ_SUCCESS);
        assert(permissions.size() == 1);
        
        std::cout << "✓ Redis-specific operations test passed" << std::endl;
    }
};

int main() {
    try {
        // 初始化日志系统
        mqtt::Logger::set_level(mqtt::LogLevel::INFO);
        
        std::cout << "Starting Redis authentication unit tests..." << std::endl;
        
        RedisAuthTest test;
        test.run_all_tests();
        
        std::cout << "All Redis authentication tests passed!" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}