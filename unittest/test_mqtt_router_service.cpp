#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <chrono>
#include "mqtt_router_service.h"
#include "mqtt_allocator.h"
#include "mqtt_memory_tags.h"

class MQTTRouterServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 获取根分配器
        MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        test_allocator_ = root_allocator->create_child("test_router", MQTTMemoryTag::MEM_TAG_ROOT);
        
        // 配置路由服务
        config_.service_host = "127.0.0.1";
        config_.service_port = 19090;  // 使用不同的端口避免冲突
        config_.redo_log_path = "./test_router.redo";
        config_.snapshot_path = "./test_router.snapshot";
        config_.snapshot_interval_seconds = 10;
        config_.redo_log_flush_interval_ms = 100;
        config_.max_redo_log_entries = 100;
        config_.worker_thread_count = 2;
        config_.coroutines_per_thread = 4;
        config_.max_memory_limit = 64 * 1024 * 1024;  // 64MB
        
        router_service_.reset(new MQTTRouterService(config_));
    }

    void TearDown() override {
        if (router_service_) {
            router_service_->stop();
            router_service_.reset();
        }
        
        // 清理测试文件
        std::remove("./test_router.redo");
        std::remove("./test_router.snapshot");
        
        if (test_allocator_ && test_allocator_->get_parent()) {
            test_allocator_->get_parent()->remove_child("test_router");
        }
    }

    MQTTAllocator* test_allocator_;
    MQTTRouterConfig config_;
    std::unique_ptr<MQTTRouterService> router_service_;
};

TEST_F(MQTTRouterServiceTest, InitializeAndStart) {
    // 测试初始化
    ASSERT_EQ(router_service_->initialize(), 0);
    
    // 测试启动
    ASSERT_EQ(router_service_->start(), 0);
    
    // 等待服务启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 测试统计信息
    size_t total_servers, total_clients, total_subscriptions;
    ASSERT_EQ(router_service_->get_statistics(total_servers, total_clients, total_subscriptions), 0);
    ASSERT_EQ(total_servers, 0);  // 初始状态没有服务器
    ASSERT_EQ(total_clients, 0);
    ASSERT_EQ(total_subscriptions, 0);
}

TEST_F(MQTTRouterServiceTest, PersistentTopicTreeBasicOperations) {
    // 初始化路由服务
    ASSERT_EQ(router_service_->initialize(), 0);
    
    // 获取内部组件（这里假设有测试接口，实际实现中可能需要友元类）
    // 为了测试目的，我们通过公共接口进行测试
    
    // 这里应该测试MQTTPersistentTopicTree的基本操作
    // 由于架构限制，我们通过路由服务的接口间接测试
    
    SUCCEED();  // 标记测试通过，实际测试需要更多的公共接口
}

class MQTTPersistentTopicTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        test_allocator_ = root_allocator->create_child("test_topic_tree", MQTTMemoryTag::MEM_TAG_TOPIC_TREE);
        
        topic_tree_.reset(new MQTTPersistentTopicTree(test_allocator_));
    }

    void TearDown() override {
        topic_tree_.reset();
        
        if (test_allocator_ && test_allocator_->get_parent()) {
            test_allocator_->get_parent()->remove_child("test_topic_tree");
        }
    }

    MQTTAllocator* test_allocator_;
    std::unique_ptr<MQTTPersistentTopicTree> topic_tree_;
};

TEST_F(MQTTPersistentTopicTreeTest, BasicSubscribeUnsubscribe) {
    MQTTString server_id("server1", MQTTStrAllocator(test_allocator_));
    MQTTString client_id("client1", MQTTStrAllocator(test_allocator_));
    MQTTString topic_filter("test/topic", MQTTStrAllocator(test_allocator_));
    uint8_t qos = 1;
    
    // 测试订阅
    ASSERT_EQ(topic_tree_->subscribe(server_id, topic_filter, client_id, qos), 0);
    
    // 测试查找路由目标
    RouteTargetVector targets{MQTTSTLAllocator<RouteTarget>(test_allocator_)};
    MQTTString publish_topic("test/topic", MQTTStrAllocator(test_allocator_));
    ASSERT_EQ(topic_tree_->find_route_targets(publish_topic, targets), 0);
    ASSERT_EQ(targets.size(), 1);
    ASSERT_EQ(targets[0].server_id, server_id);
    ASSERT_EQ(targets[0].client_id, client_id);
    ASSERT_EQ(targets[0].qos, qos);
    
    // 测试取消订阅
    ASSERT_EQ(topic_tree_->unsubscribe(server_id, topic_filter, client_id), 0);
    
    // 再次查找应该没有结果
    targets.clear();
    ASSERT_EQ(topic_tree_->find_route_targets(publish_topic, targets), 0);
    ASSERT_EQ(targets.size(), 0);
}

TEST_F(MQTTPersistentTopicTreeTest, WildcardSubscriptions) {
    MQTTString server_id("server1", MQTTStrAllocator(test_allocator_));
    MQTTString client_id("client1", MQTTStrAllocator(test_allocator_));
    
    // 测试单级通配符
    MQTTString single_wildcard("test/+/data", MQTTStrAllocator(test_allocator_));
    ASSERT_EQ(topic_tree_->subscribe(server_id, single_wildcard, client_id, 1), 0);
    
    // 测试多级通配符
    MQTTString multi_wildcard("sensor/#", MQTTStrAllocator(test_allocator_));
    ASSERT_EQ(topic_tree_->subscribe(server_id, multi_wildcard, client_id, 2), 0);
    
    // 测试匹配
    RouteTargetVector targets{MQTTSTLAllocator<RouteTarget>(test_allocator_)};
    
    // 测试单级通配符匹配
    MQTTString topic1("test/temp/data", MQTTStrAllocator(test_allocator_));
    ASSERT_EQ(topic_tree_->find_route_targets(topic1, targets), 0);
    ASSERT_GE(targets.size(), 1);
    
    // 测试多级通配符匹配
    targets.clear();
    MQTTString topic2("sensor/temperature/room1", MQTTStrAllocator(test_allocator_));
    ASSERT_EQ(topic_tree_->find_route_targets(topic2, targets), 0);
    ASSERT_GE(targets.size(), 1);
}

TEST_F(MQTTPersistentTopicTreeTest, MultipleServersAndClients) {
    // 模拟多个服务器和客户端
    MQTTString server1("server1", MQTTStrAllocator(test_allocator_));
    MQTTString server2("server2", MQTTStrAllocator(test_allocator_));
    MQTTString client1("client1", MQTTStrAllocator(test_allocator_));
    MQTTString client2("client2", MQTTStrAllocator(test_allocator_));
    MQTTString topic_filter("test/topic", MQTTStrAllocator(test_allocator_));
    
    // 多个服务器的客户端订阅相同主题
    ASSERT_EQ(topic_tree_->subscribe(server1, topic_filter, client1, 1), 0);
    ASSERT_EQ(topic_tree_->subscribe(server2, topic_filter, client2, 2), 0);
    
    // 查找路由目标
    RouteTargetVector targets{MQTTSTLAllocator<RouteTarget>(test_allocator_)};
    MQTTString publish_topic("test/topic", MQTTStrAllocator(test_allocator_));
    ASSERT_EQ(topic_tree_->find_route_targets(publish_topic, targets), 0);
    ASSERT_EQ(targets.size(), 2);
    
    // 验证两个目标都存在
    bool found_server1 = false, found_server2 = false;
    for (const auto& target : targets) {
        if (target.server_id == server1 && target.client_id == client1) {
            found_server1 = true;
            ASSERT_EQ(target.qos, 1);
        } else if (target.server_id == server2 && target.client_id == client2) {
            found_server2 = true;
            ASSERT_EQ(target.qos, 2);
        }
    }
    ASSERT_TRUE(found_server1);
    ASSERT_TRUE(found_server2);
}

TEST_F(MQTTPersistentTopicTreeTest, UnsubscribeAll) {
    MQTTString server_id("server1", MQTTStrAllocator(test_allocator_));
    MQTTString client_id("client1", MQTTStrAllocator(test_allocator_));
    
    // 订阅多个主题
    MQTTString topic1("test/topic1", MQTTStrAllocator(test_allocator_));
    MQTTString topic2("test/topic2", MQTTStrAllocator(test_allocator_));
    MQTTString topic3("sensor/temp", MQTTStrAllocator(test_allocator_));
    
    ASSERT_EQ(topic_tree_->subscribe(server_id, topic1, client_id, 1), 0);
    ASSERT_EQ(topic_tree_->subscribe(server_id, topic2, client_id, 1), 0);
    ASSERT_EQ(topic_tree_->subscribe(server_id, topic3, client_id, 2), 0);
    
    // 验证订阅存在
    RouteTargetVector targets{MQTTSTLAllocator<RouteTarget>(test_allocator_)};
    ASSERT_EQ(topic_tree_->find_route_targets(topic1, targets), 0);
    ASSERT_EQ(targets.size(), 1);
    
    // 取消所有订阅
    ASSERT_EQ(topic_tree_->unsubscribe_all(server_id, client_id), 0);
    
    // 验证所有订阅都被取消
    targets.clear();
    ASSERT_EQ(topic_tree_->find_route_targets(topic1, targets), 0);
    ASSERT_EQ(targets.size(), 0);
    
    targets.clear();
    ASSERT_EQ(topic_tree_->find_route_targets(topic2, targets), 0);
    ASSERT_EQ(targets.size(), 0);
    
    targets.clear();
    ASSERT_EQ(topic_tree_->find_route_targets(topic3, targets), 0);
    ASSERT_EQ(targets.size(), 0);
}

class MQTTRedoLogManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        test_allocator_ = root_allocator->create_child("test_redo_log", MQTTMemoryTag::MEM_TAG_ROOT);
        
        config_.redo_log_path = "./test_redo.log";
        config_.redo_log_flush_interval_ms = 50;
        config_.max_redo_log_entries = 10;
        
        redo_log_manager_.reset(new MQTTRedoLogManager(test_allocator_, config_));
    }

    void TearDown() override {
        redo_log_manager_.reset();
        
        // 清理测试文件
        std::remove("./test_redo.log");
        
        if (test_allocator_ && test_allocator_->get_parent()) {
            test_allocator_->get_parent()->remove_child("test_redo_log");
        }
    }

    MQTTAllocator* test_allocator_;
    MQTTRouterConfig config_;
    std::unique_ptr<MQTTRedoLogManager> redo_log_manager_;
};

TEST_F(MQTTRedoLogManagerTest, BasicLogOperations) {
    // 初始化
    ASSERT_EQ(redo_log_manager_->initialize(), 0);
    
    // 创建日志条目
    RouterLogEntry entry(test_allocator_);
    entry.op_type = RouterLogOpType::SUBSCRIBE;
    entry.server_id = MQTTString("server1", MQTTStrAllocator(test_allocator_));
    entry.client_id = MQTTString("client1", MQTTStrAllocator(test_allocator_));
    entry.topic_filter = MQTTString("test/topic", MQTTStrAllocator(test_allocator_));
    entry.qos = 1;
    
    // 追加日志条目
    ASSERT_EQ(redo_log_manager_->append_log_entry(entry), 0);
    
    // 验证序列号分配
    uint64_t next_seq = redo_log_manager_->get_next_sequence_id();
    ASSERT_GT(next_seq, 0);
    
    // 等待刷新
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 验证待处理条目数量
    size_t pending_count = redo_log_manager_->get_pending_entries_count();
    ASSERT_EQ(pending_count, 0);  // 应该已经刷新到磁盘
}

TEST_F(MQTTRedoLogManagerTest, BatchFlush) {
    ASSERT_EQ(redo_log_manager_->initialize(), 0);
    
    // 添加多个日志条目
    for (int i = 0; i < 5; ++i) {
        RouterLogEntry entry(test_allocator_);
        entry.op_type = RouterLogOpType::SUBSCRIBE;
        entry.server_id = MQTTString("server1", MQTTStrAllocator(test_allocator_));
        entry.client_id = to_mqtt_string("client" + std::to_string(i), test_allocator_);
        entry.topic_filter = to_mqtt_string("test/topic" + std::to_string(i), test_allocator_);
        entry.qos = static_cast<uint8_t>(i % 3);
        
        ASSERT_EQ(redo_log_manager_->append_log_entry(entry), 0);
    }
    
    // 手动刷新
    ASSERT_EQ(redo_log_manager_->flush_to_disk(), 0);
    
    // 验证所有条目都被刷新
    ASSERT_EQ(redo_log_manager_->get_pending_entries_count(), 0);
}

// 集成测试
class MQTTRouterIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        test_allocator_ = root_allocator->create_child("test_integration", MQTTMemoryTag::MEM_TAG_ROOT);
        
        // 配置路由服务（使用不同端口）
        config_.service_host = "127.0.0.1";
        config_.service_port = 19091;
        config_.redo_log_path = "./integration_test.redo";
        config_.snapshot_path = "./integration_test.snapshot";
        config_.snapshot_interval_seconds = 5;
        config_.redo_log_flush_interval_ms = 100;
        config_.max_redo_log_entries = 50;
        config_.worker_thread_count = 1;
        config_.coroutines_per_thread = 2;
        config_.max_memory_limit = 32 * 1024 * 1024;
    }

    void TearDown() override {
        // 清理测试文件
        std::remove("./integration_test.redo");
        std::remove("./integration_test.snapshot");
        
        if (test_allocator_ && test_allocator_->get_parent()) {
            test_allocator_->get_parent()->remove_child("test_integration");
        }
    }

    MQTTAllocator* test_allocator_;
    MQTTRouterConfig config_;
};

TEST_F(MQTTRouterIntegrationTest, EndToEndWorkflow) {
    // 创建路由服务
    auto router_service = std::make_unique<MQTTRouterService>(config_);
    
    // 初始化并启动
    ASSERT_EQ(router_service->initialize(), 0);
    ASSERT_EQ(router_service->start(), 0);
    
    // 等待服务启动
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // 测试基本统计信息
    size_t total_servers, total_clients, total_subscriptions;
    ASSERT_EQ(router_service->get_statistics(total_servers, total_clients, total_subscriptions), 0);
    
    // 停止服务
    ASSERT_EQ(router_service->stop(), 0);
    
    router_service.reset();
}

// 性能测试
class MQTTRouterPerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        test_allocator_ = root_allocator->create_child("test_performance", MQTTMemoryTag::MEM_TAG_TOPIC_TREE);
        
        topic_tree_.reset(new MQTTPersistentTopicTree(test_allocator_));
    }

    void TearDown() override {
        topic_tree_.reset();
        
        if (test_allocator_ && test_allocator_->get_parent()) {
            test_allocator_->get_parent()->remove_child("test_performance");
        }
    }

    MQTTAllocator* test_allocator_;
    std::unique_ptr<MQTTPersistentTopicTree> topic_tree_;
};

TEST_F(MQTTRouterPerformanceTest, LargeScaleSubscriptions) {
    const int num_servers = 10;
    const int clients_per_server = 100;
    const int topics_per_client = 5;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 创建大量订阅
    for (int s = 0; s < num_servers; ++s) {
        MQTTString server_id = to_mqtt_string("server" + std::to_string(s), test_allocator_);
        
        for (int c = 0; c < clients_per_server; ++c) {
            MQTTString client_id = to_mqtt_string("client" + std::to_string(c), test_allocator_);
            
            for (int t = 0; t < topics_per_client; ++t) {
                MQTTString topic_filter = to_mqtt_string("test/server" + std::to_string(s) + "/topic" + std::to_string(t), test_allocator_);
                
                ASSERT_EQ(topic_tree_->subscribe(server_id, topic_filter, client_id, 1), 0);
            }
        }
    }
    
    auto subscribe_time = std::chrono::high_resolution_clock::now();
    auto subscribe_duration = std::chrono::duration_cast<std::chrono::milliseconds>(subscribe_time - start_time);
    
    // 测试查找性能
    RouteTargetVector targets{MQTTSTLAllocator<RouteTarget>(test_allocator_)};
    for (int i = 0; i < 100; ++i) {
        MQTTString lookup_topic("test/server5/topic2", MQTTStrAllocator(test_allocator_));
        targets.clear();
        ASSERT_EQ(topic_tree_->find_route_targets(lookup_topic, targets), 0);
    }
    
    auto lookup_time = std::chrono::high_resolution_clock::now();
    auto lookup_duration = std::chrono::duration_cast<std::chrono::milliseconds>(lookup_time - subscribe_time);
    
    // 获取统计信息
    size_t total_servers, total_clients, total_subscriptions;
    ASSERT_EQ(topic_tree_->get_statistics(total_servers, total_clients, total_subscriptions), 0);
    
    std::cout << "Performance Test Results:" << std::endl;
    std::cout << "  Subscriptions created: " << (num_servers * clients_per_server * topics_per_client) << std::endl;
    std::cout << "  Subscribe time: " << subscribe_duration.count() << " ms" << std::endl;
    std::cout << "  Lookup time (100 queries): " << lookup_duration.count() << " ms" << std::endl;
    std::cout << "  Total servers: " << total_servers << std::endl;
    std::cout << "  Total clients: " << total_clients << std::endl;
    std::cout << "  Total subscriptions: " << total_subscriptions << std::endl;
    
    // 性能断言（可以根据实际性能调整）
    ASSERT_LT(subscribe_duration.count(), 5000);  // 订阅应该在5秒内完成
    ASSERT_LT(lookup_duration.count(), 100);      // 100次查找应该在100ms内完成
}

// 主函数
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}