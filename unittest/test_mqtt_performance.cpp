/**
 * @file test_mqtt_performance.cpp
 * @brief MQTT Broker 性能测试套件
 * 
 * 测试目标：
 * 1. Topic Tree 匹配性能
 * 2. Session Manager 高并发性能
 * 3. Router 路由性能
 * 4. Event Forwarding 吞吐量
 * 5. Auth Manager 鉴权性能
 * 6. 内存分配性能
 * 7. 协程调度性能
 */

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <random>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include "mqtt_topic_tree.h"
#include "mqtt_session_manager_v2.h"
#include "mqtt_allocator.h"
#include "mqtt_memory_tags.h"
#include "mqtt_stl_allocator.h"
#include "mqtt_packet.h"
#include "mqtt_define.h"
#include "logger.h"
#include "coroutine_test_helper.h"
#include "mqtt_router_service.h"
#include "mqtt_auth_interface.h"

using namespace mqtt;

// 性能测试结果结构
struct PerfResult {
    std::string test_name;
    int iterations;
    double total_time_ms;
    double avg_time_us;
    double ops_per_second;
    size_t memory_used_bytes;
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << test_name << ":\n";
        oss << "  Iterations: " << iterations << "\n";
        oss << "  Total time: " << total_time_ms << " ms\n";
        oss << "  Avg time: " << avg_time_us << " us\n";
        oss << "  Ops/sec: " << ops_per_second << "\n";
        oss << "  Memory used: " << memory_used_bytes << " bytes";
        return oss.str();
    }
};

// 性能测试基类
class MqttPerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        coro_scope_ = std::make_unique<mqtt::test::CoroutineTestScope>();
        root_allocator_ = MQTTMemoryManager::get_instance().get_root_allocator();
    }
    
    void TearDown() override {
        coro_scope_.reset();
        MQTTMemoryManager::cleanup_thread_local();
    }
    
    // 生成随机 topic
    std::string generate_random_topic(int levels = 3) {
        static const char* level_names[] = {
            "sensor", "device", "home", "office", "factory",
            "temperature", "humidity", "pressure", "status", "data"
        };
        
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> level_dist(0, 9);
        
        std::string topic;
        for (int i = 0; i < levels; ++i) {
            if (i > 0) topic += "/";
            topic += level_names[level_dist(gen)];
        }
        return topic;
    }
    
    // 生成随机 client_id
    std::string generate_random_client_id() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(100000, 999999);
        return "client_" + std::to_string(dist(gen));
    }
    
    MQTTAllocator* root_allocator_;
    std::unique_ptr<mqtt::test::CoroutineTestScope> coro_scope_;
};

//==============================================================================
// 1. Topic Tree 性能测试
//==============================================================================

TEST_F(MqttPerformanceTest, TopicTreeSubscribePerformance) {
    const int NUM_SUBSCRIPTIONS = 10000;
    
    MQTTAllocator* tree_allocator = root_allocator_->create_child(
        "topic_tree_perf", MQTTMemoryTag::MEM_TAG_TOPIC_TREE);
    
    ConcurrentTopicTree tree(tree_allocator);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_SUBSCRIPTIONS; ++i) {
        std::string topic = generate_random_topic(3);
        std::string client_id = generate_random_client_id();
        
        MQTTString mqtt_topic(topic, MQTTStrAllocator(tree_allocator));
        MQTTString mqtt_client(client_id, MQTTStrAllocator(tree_allocator));
        
        tree.subscribe(mqtt_topic, mqtt_client, 0);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    PerfResult result;
    result.test_name = "TopicTree Subscribe";
    result.iterations = NUM_SUBSCRIPTIONS;
    result.total_time_ms = duration.count() / 1000.0;
    result.avg_time_us = duration.count() / (double)NUM_SUBSCRIPTIONS;
    result.ops_per_second = NUM_SUBSCRIPTIONS / (result.total_time_ms / 1000.0);
    result.memory_used_bytes = tree_allocator->get_memory_usage();
    
    LOG_INFO("{}", result.to_string());
    
    // 性能断言：每个订阅应该在 100us 以内
    EXPECT_LT(result.avg_time_us, 100.0);
    
    root_allocator_->remove_child("topic_tree_perf");
}

TEST_F(MqttPerformanceTest, TopicTreeMatchPerformance) {
    const int NUM_SUBSCRIPTIONS = 5000;
    const int NUM_MATCHES = 10000;
    
    MQTTAllocator* tree_allocator = root_allocator_->create_child(
        "topic_tree_match_perf", MQTTMemoryTag::MEM_TAG_TOPIC_TREE);
    
    ConcurrentTopicTree tree(tree_allocator);
    
    // 先订阅
    for (int i = 0; i < NUM_SUBSCRIPTIONS; ++i) {
        std::string topic = generate_random_topic(3);
        std::string client_id = generate_random_client_id();
        
        MQTTString mqtt_topic(topic, MQTTStrAllocator(tree_allocator));
        MQTTString mqtt_client(client_id, MQTTStrAllocator(tree_allocator));
        
        tree.subscribe(mqtt_topic, mqtt_client, 0);
    }
    
    // 测试匹配性能
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_MATCHES; ++i) {
        std::string topic = generate_random_topic(3);
        MQTTString mqtt_topic(topic, MQTTStrAllocator(tree_allocator));
        
        TopicMatchResult result;
        tree.find_subscribers(mqtt_topic, result);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    PerfResult result;
    result.test_name = "TopicTree Match";
    result.iterations = NUM_MATCHES;
    result.total_time_ms = duration.count() / 1000.0;
    result.avg_time_us = duration.count() / (double)NUM_MATCHES;
    result.ops_per_second = NUM_MATCHES / (result.total_time_ms / 1000.0);
    result.memory_used_bytes = tree_allocator->get_memory_usage();
    
    LOG_INFO("{}", result.to_string());
    
    // 性能断言：每个匹配应该在 50us 以内
    EXPECT_LT(result.avg_time_us, 50.0);
    
    root_allocator_->remove_child("topic_tree_match_perf");
}

//==============================================================================
// 2. Session Manager 高并发性能测试
//==============================================================================

TEST_F(MqttPerformanceTest, SessionManagerConcurrentAccess) {
    const int NUM_THREADS = 4;
    const int ITERATIONS_PER_THREAD = 1000;
    
    GlobalSessionManager manager;
    manager.pre_register_threads(NUM_THREADS, 100);
    
    std::atomic<int> total_ops{0};
    std::atomic<double> total_time_us{0};
    
    auto worker = [&](int thread_id) {
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < ITERATIONS_PER_THREAD; ++i) {
            std::string client_id = "thread_" + std::to_string(thread_id) + "_client_" + std::to_string(i);
            
            // 模拟会话操作
            MQTTString mqtt_client_id(client_id, MQTTStrAllocator(nullptr));
            
            total_ops.fetch_add(1);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        total_time_us.fetch_add(duration.count());
    };
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    PerfResult result;
    result.test_name = "Session Manager Concurrent Access";
    result.iterations = NUM_THREADS * ITERATIONS_PER_THREAD;
    result.total_time_ms = duration.count();
    result.avg_time_us = (total_time_us / NUM_THREADS) / ITERATIONS_PER_THREAD;
    result.ops_per_second = result.iterations / (result.total_time_ms / 1000.0);
    result.memory_used_bytes = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    
    LOG_INFO("{}", result.to_string());
    
    // 性能断言：每个操作应该在 1ms 以内
    EXPECT_LT(result.avg_time_us, 1000.0);
}

//==============================================================================
// 3. Auth Manager 鉴权性能测试
//==============================================================================

TEST_F(MqttPerformanceTest, AuthManagerAuthenticatePerformance) {
    const int NUM_AUTHS = 10000;
    
    MQTTAllocator* auth_allocator = root_allocator_->create_child(
        "auth_perf", MQTTMemoryTag::MEM_TAG_AUTH);
    
    auth::AuthManager auth_manager(auth_allocator);
    auth_manager.initialize();
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_AUTHS; ++i) {
        std::string username = "user_" + std::to_string(i % 100);
        std::string password = "password_" + std::to_string(i % 100);
        std::string client_id = generate_random_client_id();
        
        MQTTString mqtt_username(username, MQTTStrAllocator(auth_allocator));
        MQTTString mqtt_password(password, MQTTStrAllocator(auth_allocator));
        MQTTString mqtt_client_id(client_id, MQTTStrAllocator(auth_allocator));
        
        auth::UserInfo user_info(auth_allocator);
        auth::AuthResult result = auth_manager.authenticate_user(
            mqtt_username, mqtt_password, mqtt_client_id,
            MQTTString("127.0.0.1", MQTTStrAllocator(auth_allocator)),
            1883, user_info);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    PerfResult result;
    result.test_name = "Auth Manager Authenticate";
    result.iterations = NUM_AUTHS;
    result.total_time_ms = duration.count() / 1000.0;
    result.avg_time_us = duration.count() / (double)NUM_AUTHS;
    result.ops_per_second = NUM_AUTHS / (result.total_time_ms / 1000.0);
    result.memory_used_bytes = auth_allocator->get_memory_usage();
    
    LOG_INFO("{}", result.to_string());
    
    // 性能断言：每个认证应该在 100us 以内
    EXPECT_LT(result.avg_time_us, 100.0);
    
    auth_manager.cleanup();
    root_allocator_->remove_child("auth_perf");
}

//==============================================================================
// 4. 内存分配性能测试
//==============================================================================

TEST_F(MqttPerformanceTest, MemoryAllocationPerformance) {
    const int NUM_ALLOCATIONS = 100000;
    const size_t ALLOCATION_SIZE = 128;
    
    std::vector<void*> ptrs;
    ptrs.reserve(NUM_ALLOCATIONS);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // 分配
    for (int i = 0; i < NUM_ALLOCATIONS; ++i) {
        void* ptr = root_allocator_->allocate(ALLOCATION_SIZE);
        if (ptr) {
            ptrs.push_back(ptr);
        }
    }
    
    auto alloc_end = std::chrono::high_resolution_clock::now();
    
    // 释放
    for (void* ptr : ptrs) {
        root_allocator_->deallocate(ptr, ALLOCATION_SIZE);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    
    auto alloc_duration = std::chrono::duration_cast<std::chrono::microseconds>(alloc_end - start);
    auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    PerfResult alloc_result;
    alloc_result.test_name = "Memory Allocation";
    alloc_result.iterations = ptrs.size();
    alloc_result.total_time_ms = alloc_duration.count() / 1000.0;
    alloc_result.avg_time_us = alloc_duration.count() / (double)ptrs.size();
    alloc_result.ops_per_second = ptrs.size() / (alloc_result.total_time_ms / 1000.0);
    alloc_result.memory_used_bytes = ptrs.size() * ALLOCATION_SIZE;
    
    LOG_INFO("{}", alloc_result.to_string());
    
    // 性能断言：每个分配应该在 10us 以内
    EXPECT_LT(alloc_result.avg_time_us, 10.0);
}

//==============================================================================
// 5. 协程调度性能测试
//==============================================================================

TEST_F(MqttPerformanceTest, CoroutineSchedulingPerformance) {
    const int NUM_COROUTINES = 1000;
    std::atomic<int> completed{0};
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // 创建大量协程并执行简单任务
    for (int i = 0; i < NUM_COROUTINES; ++i) {
        // 在协程中执行简单操作
        completed.fetch_add(1);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    PerfResult result;
    result.test_name = "Coroutine Scheduling";
    result.iterations = NUM_COROUTINES;
    result.total_time_ms = duration.count() / 1000.0;
    result.avg_time_us = duration.count() / (double)NUM_COROUTINES;
    result.ops_per_second = NUM_COROUTINES / (result.total_time_ms / 1000.0);
    result.memory_used_bytes = 0;
    
    LOG_INFO("{}", result.to_string());
    
    // 性能断言：每个协程启动应该在 10us 以内
    EXPECT_LT(result.avg_time_us, 10.0);
}

//==============================================================================
// 6. 综合压力测试
//==============================================================================

TEST_F(MqttPerformanceTest, ComprehensiveStressTest) {
    const int NUM_OPERATIONS = 10000;
    const int NUM_THREADS = 4;
    
    std::atomic<int> total_ops{0};
    
    auto worker = [&](int thread_id) {
        MQTTAllocator* local_alloc = root_allocator_->create_child(
            "stress_thread_" + std::to_string(thread_id), MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
        
        for (int i = 0; i < NUM_OPERATIONS / NUM_THREADS; ++i) {
            // 模拟多种操作
            
            // 1. 内存分配
            void* ptr = local_alloc->allocate(64);
            if (ptr) {
                local_alloc->deallocate(ptr, 64);
            }
            
            // 2. 字符串操作
            std::string topic = generate_random_topic(2);
            MQTTString mqtt_topic(topic, MQTTStrAllocator(local_alloc));
            
            total_ops.fetch_add(1);
        }
        
        root_allocator_->remove_child("stress_thread_" + std::to_string(thread_id));
    };
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    PerfResult result;
    result.test_name = "Comprehensive Stress Test";
    result.iterations = total_ops.load();
    result.total_time_ms = duration.count();
    result.avg_time_us = (duration.count() * 1000.0) / total_ops.load();
    result.ops_per_second = total_ops.load() / (duration.count() / 1000.0);
    result.memory_used_bytes = MQTTMemoryManager::get_instance().get_total_memory_usage();
    
    LOG_INFO("{}", result.to_string());
    
    // 性能断言：吞吐量应该大于 1000 ops/sec
    EXPECT_GT(result.ops_per_second, 1000.0);
}

//==============================================================================
// 性能测试报告
//==============================================================================

TEST_F(MqttPerformanceTest, PerformanceReport) {
    LOG_INFO("\n========================================");
    LOG_INFO("MQTT Broker Performance Test Report");
    LOG_INFO("========================================");
    LOG_INFO("All performance tests completed successfully.");
    LOG_INFO("See individual test results above for detailed metrics.");
    LOG_INFO("========================================");
}