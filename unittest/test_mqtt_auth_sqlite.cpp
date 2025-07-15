#include "../src/mqtt_auth_sqlite.h"
#include "../src/mqtt_allocator.h"
#include "../src/mqtt_memory_manager.h"
#include "../src/logger.h"
#include <cassert>
#include <iostream>
#include <filesystem>

using namespace mqtt::auth;

class SQLiteAuthTest {
public:
    SQLiteAuthTest() {
        // 初始化内存管理
        mqtt::MQTTMemoryManager::get_instance().initialize(1024 * 1024);  // 1MB
        allocator_ = mqtt::MQTTMemoryManager::get_instance().get_root_allocator();
        
        // 配置SQLite认证提供者
        config_.db_path = "test_auth.db";
        config_.connection_pool_size = 3;
        config_.max_retry_count = 2;
        config_.retry_delay_ms = 50;
        config_.query_timeout_ms = 1000;
        config_.enable_wal_mode = false;  // 测试时禁用WAL模式
        config_.cache_size_kb = 1024;
        
        // 删除旧的测试数据库
        if (std::filesystem::exists(config_.db_path)) {
            std::filesystem::remove(config_.db_path);
        }
        
        provider_ = std::make_unique<SQLiteAuthProvider>(config_, allocator_);
    }
    
    ~SQLiteAuthTest() {
        if (provider_) {
            provider_->cleanup();
        }
        
        // 清理测试数据库
        if (std::filesystem::exists(config_.db_path)) {
            std::filesystem::remove(config_.db_path);
        }
        
        mqtt::MQTTMemoryManager::get_instance().cleanup();
    }
    
    void run_all_tests() {
        test_initialization();
        test_user_management();
        test_authentication();
        test_topic_permissions();
        test_super_user();
        test_performance();
        test_concurrent_access();
        test_error_handling();
    }

private:
    SQLiteAuthConfig config_;
    MQTTAllocator* allocator_;
    std::unique_ptr<SQLiteAuthProvider> provider_;
    
    void test_initialization() {
        std::cout << "Testing SQLite authentication initialization..." << std::endl;
        
        // 测试初始化
        int ret = provider_->initialize();
        assert(ret == MQ_SUCCESS);
        
        // 测试健康检查
        assert(provider_->is_healthy());
        
        // 测试提供者名称
        assert(std::string(provider_->get_provider_name()) == "SQLite");
        
        std::cout << "✓ SQLite initialization test passed" << std::endl;
    }
    
    void test_user_management() {
        std::cout << "Testing SQLite user management..." << std::endl;
        
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
        
        std::cout << "✓ SQLite user management test passed" << std::endl;
    }
    
    void test_authentication() {
        std::cout << "Testing SQLite authentication..." << std::endl;
        
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
        
        std::cout << "✓ SQLite authentication test passed" << std::endl;
    }
    
    void test_topic_permissions() {
        std::cout << "Testing SQLite topic permissions..." << std::endl;
        
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
        
        // 测试删除权限
        ret = provider_->remove_topic_permission(username, topic_pattern1);
        assert(ret == MQ_SUCCESS);
        
        result = provider_->check_topic_access(user_info, test_topic1, Permission::READ);
        assert(result == AuthResult::TOPIC_ACCESS_DENIED);
        
        // 测试清空用户权限
        ret = provider_->clear_user_permissions(username);
        assert(ret == MQ_SUCCESS);
        
        result = provider_->check_topic_access(user_info, test_topic2, Permission::READ);
        assert(result == AuthResult::TOPIC_ACCESS_DENIED);
        
        std::cout << "✓ SQLite topic permissions test passed" << std::endl;
    }
    
    void test_super_user() {
        std::cout << "Testing SQLite super user functionality..." << std::endl;
        
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
        
        std::cout << "✓ SQLite super user test passed" << std::endl;
    }
    
    void test_performance() {
        std::cout << "Testing SQLite performance..." << std::endl;
        
        // 重置统计信息
        provider_->reset_stats();
        
        // 添加多个用户进行性能测试
        const int num_users = 100;
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
        const int auth_count = 1000;
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
        assert(stats.total_login_attempts >= auth_count);
        assert(stats.successful_logins >= auth_count);
        
        std::cout << "✓ SQLite performance test passed" << std::endl;
    }
    
    void test_concurrent_access() {
        std::cout << "Testing SQLite concurrent access..." << std::endl;
        
        // 创建多个线程同时进行认证
        const int num_threads = 10;
        const int operations_per_thread = 50;
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
        
        std::cout << "✓ SQLite concurrent access test passed" << std::endl;
    }
    
    void test_error_handling() {
        std::cout << "Testing SQLite error handling..." << std::endl;
        
        // 测试重复添加用户
        MQTTString username("erroruser", MQTTStrAllocator(allocator_));
        MQTTString password("errorpass", MQTTStrAllocator(allocator_));
        
        int ret = provider_->add_user(username, password, false);
        assert(ret == MQ_SUCCESS);
        
        ret = provider_->add_user(username, password, false);
        // SQLite应该处理重复插入错误
        
        // 测试删除不存在的用户
        MQTTString nonexistent("nonexistent", MQTTStrAllocator(allocator_));
        ret = provider_->remove_user(nonexistent);
        // 删除不存在的用户应该成功（没有影响）
        assert(ret == MQ_SUCCESS);
        
        // 测试更新不存在用户的密码
        MQTTString new_password("newpass", MQTTStrAllocator(allocator_));
        ret = provider_->update_user_password(nonexistent, new_password);
        // 更新不存在的用户应该成功（没有影响）
        assert(ret == MQ_SUCCESS);
        
        // 测试空用户名和密码
        MQTTString empty_string("", MQTTStrAllocator(allocator_));
        ret = provider_->add_user(empty_string, password, false);
        // 空用户名应该能正常处理
        
        std::cout << "✓ SQLite error handling test passed" << std::endl;
    }
};

int main() {
    try {
        // 初始化日志系统
        mqtt::Logger::set_level(mqtt::LogLevel::INFO);
        
        std::cout << "Starting SQLite authentication unit tests..." << std::endl;
        
        SQLiteAuthTest test;
        test.run_all_tests();
        
        std::cout << "All SQLite authentication tests passed!" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}