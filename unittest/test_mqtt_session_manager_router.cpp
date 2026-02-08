#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <chrono>
#include "mqtt_session_manager_v2.h"
#include "mqtt_router_rpc_client.h"
#include "mqtt_allocator.h"
#include "mqtt_memory_tags.h"

using namespace mqtt;

// Mock RPC客户端用于测试
class MockRouterRpcClient : public MQTTRouterRpcClient {
public:
    MockRouterRpcClient(MQTTAllocator* allocator) 
        : MQTTRouterRpcClient(allocator, RpcClientConfig())
        , connected_(false)
        , subscribe_calls_(0)
        , unsubscribe_calls_(0)
        , connect_calls_(0)
        , disconnect_calls_(0)
        , publish_calls_(0)
        , last_subscribe_request_(allocator)
        , last_unsubscribe_request_(allocator)
        , last_connect_request_(allocator)
        , last_disconnect_request_(allocator)
        , last_publish_request_(allocator) {
    }
    
    // 重写父类方法
    int initialize() override { return 0; }
    int connect() override { 
        connected_ = true; 
        return 0; 
    }
    int disconnect() override { 
        connected_ = false; 
        return 0; 
    }
    
    bool is_connected() const override { 
        return connected_; 
    }
    
    int subscribe_async(const SubscribeRequest& request) override {
        subscribe_calls_++;
        last_subscribe_request_ = request;
        return 0;
    }
    
    int unsubscribe_async(const UnsubscribeRequest& request) override {
        unsubscribe_calls_++;
        last_unsubscribe_request_ = request;
        return 0;
    }
    
    int client_connect_async(const ClientConnectRequest& request) override {
        connect_calls_++;
        last_connect_request_ = request;
        return 0;
    }
    
    int client_disconnect_async(const ClientDisconnectRequest& request) override {
        disconnect_calls_++;
        last_disconnect_request_ = request;
        return 0;
    }
    
    int route_publish(const RoutePublishRequest& request, RouteTargetVector& targets) override {
        publish_calls_++;
        last_publish_request_ = request;
        
        // 模拟返回一些路由目标（使用默认分配器）
        RouteTarget target1;
        target1.server_id = MQTTString("remote_server1");
        target1.client_id = MQTTString("remote_client1");
        target1.qos = 1;
        
        RouteTarget target2;
        target2.server_id = MQTTString("remote_server2");
        target2.client_id = MQTTString("remote_client2");
        target2.qos = 2;
        
        targets.clear();
        targets.push_back(target1);
        targets.push_back(target2);
        
        return 0;
    }
    
    // 测试辅助方法
    int get_subscribe_calls() const { return subscribe_calls_; }
    int get_unsubscribe_calls() const { return unsubscribe_calls_; }
    int get_connect_calls() const { return connect_calls_; }
    int get_disconnect_calls() const { return disconnect_calls_; }
    int get_publish_calls() const { return publish_calls_; }
    
    const SubscribeRequest& get_last_subscribe_request() const { return last_subscribe_request_; }
    const UnsubscribeRequest& get_last_unsubscribe_request() const { return last_unsubscribe_request_; }
    const ClientConnectRequest& get_last_connect_request() const { return last_connect_request_; }
    const ClientDisconnectRequest& get_last_disconnect_request() const { return last_disconnect_request_; }
    const RoutePublishRequest& get_last_publish_request() const { return last_publish_request_; }
    
    void reset_counters() {
        subscribe_calls_ = 0;
        unsubscribe_calls_ = 0;
        connect_calls_ = 0;
        disconnect_calls_ = 0;
        publish_calls_ = 0;
    }

private:
    bool connected_;
    int subscribe_calls_;
    int unsubscribe_calls_;
    int connect_calls_;
    int disconnect_calls_;
    int publish_calls_;
    
    SubscribeRequest last_subscribe_request_;
    UnsubscribeRequest last_unsubscribe_request_;
    ClientConnectRequest last_connect_request_;
    ClientDisconnectRequest last_disconnect_request_;
    RoutePublishRequest last_publish_request_;
};

class GlobalSessionManagerRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建全局会话管理器
        session_manager_ = &GlobalSessionManagerInstance::instance();
        
        // 预注册线程
        ASSERT_EQ(session_manager_->pre_register_threads(2, 100), MQ_SUCCESS);
        
        // 创建Mock RPC客户端
        MQTTAllocator* root_allocator = session_manager_->get_allocator();
        test_allocator_ = root_allocator->create_child("test_session_router", MQTTMemoryTag::MEM_TAG_ROOT);
        
        mock_rpc_client_ = std::make_unique<MockRouterRpcClient>(test_allocator_);
        mock_rpc_client_->initialize();
        mock_rpc_client_->connect();
        
        // 设置路由客户端
        session_manager_->set_router_client(std::unique_ptr<MQTTRouterRpcClient>(mock_rpc_client_.release()));
        
        // 设置服务器ID
        session_manager_->set_server_id(MQTTString("test_server", MQTTStrAllocator(test_allocator_)));
        
        // 完成线程注册
        ASSERT_EQ(session_manager_->finalize_thread_registration(), MQ_SUCCESS);
    }

    void TearDown() override {
        // 清理
        session_manager_->set_router_client(nullptr);
        
        if (test_allocator_ && test_allocator_->get_parent()) {
            test_allocator_->get_parent()->remove_child("test_session_router");
        }
    }

    GlobalSessionManager* session_manager_;
    MQTTAllocator* test_allocator_;
    std::unique_ptr<MockRouterRpcClient> mock_rpc_client_;
};

TEST_F(GlobalSessionManagerRouterTest, SetAndGetServerID) {
    MQTTString test_id("test_server_123", MQTTStrAllocator(test_allocator_));
    session_manager_->set_server_id(test_id);
    
    ASSERT_EQ(session_manager_->get_server_id(), test_id);
}

TEST_F(GlobalSessionManagerRouterTest, RouterClientAvailability) {
    // 测试路由器可用性
    ASSERT_TRUE(session_manager_->is_router_available());
    
    // 获取路由客户端
    MQTTRouterRpcClient* client = session_manager_->get_router_client();
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(client->is_connected());
}

TEST_F(GlobalSessionManagerRouterTest, ClientConnectNotification) {
    MockRouterRpcClient* mock_client = static_cast<MockRouterRpcClient*>(session_manager_->get_router_client());
    ASSERT_NE(mock_client, nullptr);
    
    // 重置计数器
    mock_client->reset_counters();
    
    // 通知客户端连接
    MQTTString client_id("test_client", MQTTStrAllocator(test_allocator_));
    MQTTString username("test_user", MQTTStrAllocator(test_allocator_));
    
    ASSERT_EQ(session_manager_->notify_router_client_connect(client_id, username, 5, 60, true), MQ_SUCCESS);
    
    // 验证RPC调用
    ASSERT_EQ(mock_client->get_connect_calls(), 1);
    
    const auto& request = mock_client->get_last_connect_request();
    ASSERT_EQ(request.server_id, session_manager_->get_server_id());
    ASSERT_EQ(request.client_id, client_id);
    ASSERT_EQ(request.username, username);
    ASSERT_EQ(request.protocol_version, 5);
    ASSERT_EQ(request.keep_alive, 60);
    ASSERT_TRUE(request.clean_session);
}

TEST_F(GlobalSessionManagerRouterTest, ClientDisconnectNotification) {
    MockRouterRpcClient* mock_client = static_cast<MockRouterRpcClient*>(session_manager_->get_router_client());
    ASSERT_NE(mock_client, nullptr);
    
    // 重置计数器
    mock_client->reset_counters();
    
    // 通知客户端断开
    MQTTString client_id("test_client", MQTTStrAllocator(test_allocator_));
    MQTTString reason("normal disconnect", MQTTStrAllocator(test_allocator_));
    
    ASSERT_EQ(session_manager_->notify_router_client_disconnect(client_id, reason), MQ_SUCCESS);
    
    // 验证RPC调用
    ASSERT_EQ(mock_client->get_disconnect_calls(), 1);
    
    const auto& request = mock_client->get_last_disconnect_request();
    ASSERT_EQ(request.server_id, session_manager_->get_server_id());
    ASSERT_EQ(request.client_id, client_id);
    ASSERT_EQ(request.disconnect_reason, reason);
}

TEST_F(GlobalSessionManagerRouterTest, SubscribeWithRouter) {
    MockRouterRpcClient* mock_client = static_cast<MockRouterRpcClient*>(session_manager_->get_router_client());
    ASSERT_NE(mock_client, nullptr);
    
    // 重置计数器
    mock_client->reset_counters();
    
    // 测试带路由的订阅
    MQTTString topic_filter("test/topic/+", MQTTStrAllocator(test_allocator_));
    MQTTString client_id("test_client", MQTTStrAllocator(test_allocator_));
    uint8_t qos = 2;
    
    ASSERT_EQ(session_manager_->subscribe_topic_with_router(topic_filter, client_id, qos), MQ_SUCCESS);
    
    // 验证本地订阅
    std::vector<SubscriberInfo> subscribers;
    ASSERT_EQ(session_manager_->find_topic_subscribers(MQTTString("test/topic/temp", MQTTStrAllocator(test_allocator_)), subscribers), MQ_SUCCESS);
    
    // 验证RPC调用
    ASSERT_EQ(mock_client->get_subscribe_calls(), 1);
    
    const auto& request = mock_client->get_last_subscribe_request();
    ASSERT_EQ(request.server_id, session_manager_->get_server_id());
    ASSERT_EQ(request.client_id, client_id);
    ASSERT_EQ(request.topic_filter, topic_filter);
    ASSERT_EQ(request.qos, qos);
}

TEST_F(GlobalSessionManagerRouterTest, UnsubscribeWithRouter) {
    MockRouterRpcClient* mock_client = static_cast<MockRouterRpcClient*>(session_manager_->get_router_client());
    ASSERT_NE(mock_client, nullptr);
    
    // 先订阅
    MQTTString topic_filter("test/topic", MQTTStrAllocator(test_allocator_));
    MQTTString client_id("test_client", MQTTStrAllocator(test_allocator_));
    ASSERT_EQ(session_manager_->subscribe_topic_with_router(topic_filter, client_id, 1), MQ_SUCCESS);
    
    // 重置计数器
    mock_client->reset_counters();
    
    // 测试带路由的取消订阅
    ASSERT_EQ(session_manager_->unsubscribe_topic_with_router(topic_filter, client_id), MQ_SUCCESS);
    
    // 验证本地取消订阅
    std::vector<SubscriberInfo> subscribers;
    ASSERT_EQ(session_manager_->find_topic_subscribers(topic_filter, subscribers), MQ_SUCCESS);
    // 应该没有订阅者了
    bool found = false;
    for (const auto& sub : subscribers) {
        if (sub.client_id == client_id) {
            found = true;
            break;
        }
    }
    ASSERT_FALSE(found);
    
    // 验证RPC调用
    ASSERT_EQ(mock_client->get_unsubscribe_calls(), 1);
    
    const auto& request = mock_client->get_last_unsubscribe_request();
    ASSERT_EQ(request.server_id, session_manager_->get_server_id());
    ASSERT_EQ(request.client_id, client_id);
    ASSERT_EQ(request.topic_filter, topic_filter);
}

TEST_F(GlobalSessionManagerRouterTest, PublishViaRouter) {
    MockRouterRpcClient* mock_client = static_cast<MockRouterRpcClient*>(session_manager_->get_router_client());
    ASSERT_NE(mock_client, nullptr);
    
    // 重置计数器
    mock_client->reset_counters();
    
    // 创建发布包
    PublishPacket packet;
    packet.topic_name = MQTTString("test/topic", MQTTStrAllocator(test_allocator_));
    packet.qos = 1;
    packet.retain = true;
    
    std::string payload_data = "test message";
    packet.payload.assign(payload_data.begin(), payload_data.end());
    
    MQTTString sender_client_id("publisher", MQTTStrAllocator(test_allocator_));
    
    // 测试通过路由器转发发布
    int result = session_manager_->forward_publish_via_router(packet.topic_name, packet, sender_client_id);
    ASSERT_GE(result, 0);  // 应该返回转发的本地订阅者数量
    
    // 验证RPC调用
    ASSERT_EQ(mock_client->get_publish_calls(), 1);
    
    const auto& request = mock_client->get_last_publish_request();
    ASSERT_EQ(request.topic, packet.topic_name);
    ASSERT_EQ(request.qos, packet.qos);
    ASSERT_EQ(request.retain, packet.retain);
    ASSERT_EQ(request.publisher_server_id, session_manager_->get_server_id());
    ASSERT_EQ(request.publisher_client_id, sender_client_id);
    ASSERT_EQ(request.payload.size(), payload_data.length());
}

TEST_F(GlobalSessionManagerRouterTest, RegisterWithRouter) {
    // 测试向路由器注册
    ASSERT_EQ(session_manager_->register_with_router(), MQ_SUCCESS);
}

TEST_F(GlobalSessionManagerRouterTest, RouterUnavailable) {
    // 测试路由器不可用的情况
    session_manager_->set_router_client(nullptr);
    
    ASSERT_FALSE(session_manager_->is_router_available());
    ASSERT_EQ(session_manager_->get_router_client(), nullptr);
    
    // 在没有路由器的情况下，操作应该成功但不会有路由功能
    MQTTString topic_filter("test/topic", MQTTStrAllocator(test_allocator_));
    MQTTString client_id("test_client", MQTTStrAllocator(test_allocator_));
    
    ASSERT_EQ(session_manager_->subscribe_topic_with_router(topic_filter, client_id, 1), MQ_SUCCESS);
    ASSERT_EQ(session_manager_->unsubscribe_topic_with_router(topic_filter, client_id), MQ_SUCCESS);
    
    ASSERT_EQ(session_manager_->notify_router_client_connect(client_id), MQ_SUCCESS);
    ASSERT_EQ(session_manager_->notify_router_client_disconnect(client_id), MQ_SUCCESS);
}

// 多线程测试
class GlobalSessionManagerRouterMultiThreadTest : public ::testing::Test {
protected:
    void SetUp() override {
        session_manager_ = &GlobalSessionManagerInstance::instance();
        
        // 预注册线程
        ASSERT_EQ(session_manager_->pre_register_threads(4, 1000), MQ_SUCCESS);
        
        // 创建Mock RPC客户端
        MQTTAllocator* root_allocator = session_manager_->get_allocator();
        test_allocator_ = root_allocator->create_child("test_multithread_router", MQTTMemoryTag::MEM_TAG_ROOT);
        
        mock_rpc_client_ = std::make_unique<MockRouterRpcClient>(test_allocator_);
        mock_rpc_client_->initialize();
        mock_rpc_client_->connect();
        
        session_manager_->set_router_client(std::unique_ptr<MQTTRouterRpcClient>(mock_rpc_client_.release()));
        session_manager_->set_server_id(MQTTString("test_server_mt", MQTTStrAllocator(test_allocator_)));
        
        ASSERT_EQ(session_manager_->finalize_thread_registration(), MQ_SUCCESS);
    }

    void TearDown() override {
        session_manager_->set_router_client(nullptr);
        
        if (test_allocator_ && test_allocator_->get_parent()) {
            test_allocator_->get_parent()->remove_child("test_multithread_router");
        }
    }

    GlobalSessionManager* session_manager_;
    MQTTAllocator* test_allocator_;
    std::unique_ptr<MockRouterRpcClient> mock_rpc_client_;
};

TEST_F(GlobalSessionManagerRouterMultiThreadTest, ConcurrentSubscriptions) {
    const int num_threads = 4;
    const int subs_per_thread = 100;
    
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    
    // 启动多个线程并发订阅
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, subs_per_thread, &success_count]() {
            for (int i = 0; i < subs_per_thread; ++i) {
                std::string topic_str = "test/thread" + std::to_string(t) + "/topic" + std::to_string(i);
                MQTTString topic_filter(topic_str.c_str(), MQTTStrAllocator(test_allocator_));
                std::string client_str = "client_t" + std::to_string(t) + "_" + std::to_string(i);
                MQTTString client_id(client_str.c_str(), MQTTStrAllocator(test_allocator_));
                
                if (session_manager_->subscribe_topic_with_router(topic_filter, client_id, 1) == MQ_SUCCESS) {
                    success_count++;
                }
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }
    
    // 验证结果
    ASSERT_EQ(success_count.load(), num_threads * subs_per_thread);
    
    // 验证RPC调用次数
    MockRouterRpcClient* mock_client = static_cast<MockRouterRpcClient*>(session_manager_->get_router_client());
    ASSERT_NE(mock_client, nullptr);
    ASSERT_EQ(mock_client->get_subscribe_calls(), num_threads * subs_per_thread);
}

TEST_F(GlobalSessionManagerRouterMultiThreadTest, ConcurrentClientNotifications) {
    const int num_threads = 4;
    const int clients_per_thread = 50;
    
    std::vector<std::thread> threads;
    std::atomic<int> connect_success_count(0);
    std::atomic<int> disconnect_success_count(0);
    
    // 启动多个线程并发通知客户端连接和断开
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, clients_per_thread, &connect_success_count, &disconnect_success_count]() {
            for (int i = 0; i < clients_per_thread; ++i) {
                std::string client_str = "client_t" + std::to_string(t) + "_" + std::to_string(i);
                MQTTString client_id(client_str.c_str(), MQTTStrAllocator(test_allocator_));
                
                // 通知连接
                if (session_manager_->notify_router_client_connect(client_id) == MQ_SUCCESS) {
                    connect_success_count++;
                }
                
                // 短暂延迟
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                
                // 通知断开
                if (session_manager_->notify_router_client_disconnect(client_id) == MQ_SUCCESS) {
                    disconnect_success_count++;
                }
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }
    
    // 验证结果
    ASSERT_EQ(connect_success_count.load(), num_threads * clients_per_thread);
    ASSERT_EQ(disconnect_success_count.load(), num_threads * clients_per_thread);
    
    // 验证RPC调用次数
    MockRouterRpcClient* mock_client = static_cast<MockRouterRpcClient*>(session_manager_->get_router_client());
    ASSERT_NE(mock_client, nullptr);
    ASSERT_EQ(mock_client->get_connect_calls(), num_threads * clients_per_thread);
    ASSERT_EQ(mock_client->get_disconnect_calls(), num_threads * clients_per_thread);
}

// 主函数
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}