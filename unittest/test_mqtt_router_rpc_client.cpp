#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "mqtt_router_rpc_client.h"
#include "mqtt_allocator.h"
#include "mqtt_memory_tags.h"
#include "mqtt_stl_allocator.h"

class MQTTRouterRpcClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 获取根分配器
        MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        test_allocator_ = root_allocator->create_child("test_rpc_client", MQTTMemoryTag::MEM_TAG_ROOT);
        
        // 配置RPC客户端
        config_.router_host = "127.0.0.1";
        config_.router_port = 19092;  // 使用测试端口
        config_.connect_timeout_ms = 1000;
        config_.request_timeout_ms = 2000;
        config_.max_retry_count = 2;
        config_.retry_delay_ms = 100;
        config_.enable_heartbeat = false;  // 测试时禁用心跳
        config_.max_pending_requests = 100;
        
        rpc_client_.reset(new MQTTRouterRpcClient(test_allocator_, config_));
    }

    void TearDown() override {
        if (rpc_client_) {
            rpc_client_->disconnect();
            rpc_client_.reset();
        }
        
        if (test_allocator_ && test_allocator_->get_parent()) {
            test_allocator_->get_parent()->remove_child("test_rpc_client");
        }
    }

    MQTTAllocator* test_allocator_;
    MQTTRouterRpcClient::RpcClientConfig config_;
    std::unique_ptr<MQTTRouterRpcClient> rpc_client_;
};

TEST_F(MQTTRouterRpcClientTest, InitializeAndConnectionFail) {
    // 测试初始化
    ASSERT_EQ(rpc_client_->initialize(), 0);
    
    // 测试连接到不存在的服务器（应该失败）
    ASSERT_NE(rpc_client_->connect(), 0);
    
    // 验证连接状态
    ASSERT_FALSE(rpc_client_->is_connected());
    
    // 测试统计信息
    ASSERT_EQ(rpc_client_->get_total_requests(), 0);
    ASSERT_EQ(rpc_client_->get_successful_requests(), 0);
    ASSERT_EQ(rpc_client_->get_failed_requests(), 0);
}

TEST_F(MQTTRouterRpcClientTest, RequestStructures) {
    // 测试订阅请求结构
    MQTTRouterRpcClient::SubscribeRequest sub_req(test_allocator_);
    sub_req.server_id = MQTTString("server1", MQTTStrAllocator(test_allocator_));
    sub_req.client_id = MQTTString("client1", MQTTStrAllocator(test_allocator_));
    sub_req.topic_filter = MQTTString("test/topic", MQTTStrAllocator(test_allocator_));
    sub_req.qos = 1;
    
    ASSERT_EQ(sub_req.server_id, MQTTString("server1", MQTTStrAllocator(test_allocator_)));
    ASSERT_EQ(sub_req.client_id, MQTTString("client1", MQTTStrAllocator(test_allocator_)));
    ASSERT_EQ(sub_req.topic_filter, MQTTString("test/topic", MQTTStrAllocator(test_allocator_)));
    ASSERT_EQ(sub_req.qos, 1);
    
    // 测试取消订阅请求结构
    MQTTRouterRpcClient::UnsubscribeRequest unsub_req(test_allocator_);
    unsub_req.server_id = MQTTString("server1", MQTTStrAllocator(test_allocator_));
    unsub_req.client_id = MQTTString("client1", MQTTStrAllocator(test_allocator_));
    unsub_req.topic_filter = MQTTString("test/topic", MQTTStrAllocator(test_allocator_));
    
    ASSERT_EQ(unsub_req.server_id, MQTTString("server1", MQTTStrAllocator(test_allocator_)));
    ASSERT_EQ(unsub_req.client_id, MQTTString("client1", MQTTStrAllocator(test_allocator_)));
    ASSERT_EQ(unsub_req.topic_filter, MQTTString("test/topic", MQTTStrAllocator(test_allocator_)));
    
    // 测试客户端连接请求结构
    MQTTRouterRpcClient::ClientConnectRequest conn_req(test_allocator_);
    conn_req.server_id = MQTTString("server1", MQTTStrAllocator(test_allocator_));
    conn_req.client_id = MQTTString("client1", MQTTStrAllocator(test_allocator_));
    conn_req.username = MQTTString("user1", MQTTStrAllocator(test_allocator_));
    conn_req.protocol_version = 5;
    conn_req.keep_alive = 60;
    conn_req.clean_session = true;
    
    ASSERT_EQ(conn_req.protocol_version, 5);
    ASSERT_EQ(conn_req.keep_alive, 60);
    ASSERT_TRUE(conn_req.clean_session);
    
    // 测试路由发布请求结构
    MQTTRouterRpcClient::RoutePublishRequest pub_req(test_allocator_);
    pub_req.topic = MQTTString("test/topic", MQTTStrAllocator(test_allocator_));
    pub_req.qos = 2;
    pub_req.retain = true;
    pub_req.publisher_server_id = MQTTString("server1", MQTTStrAllocator(test_allocator_));
    pub_req.publisher_client_id = MQTTString("client1", MQTTStrAllocator(test_allocator_));
    
    // 添加payload
    std::string payload_data = "test message";
    pub_req.payload.assign(payload_data.begin(), payload_data.end());
    
    ASSERT_EQ(pub_req.qos, 2);
    ASSERT_TRUE(pub_req.retain);
    ASSERT_EQ(pub_req.payload.size(), payload_data.size());
}

TEST_F(MQTTRouterRpcClientTest, AsyncOperationsWithoutConnection) {
    // 初始化但不连接
    ASSERT_EQ(rpc_client_->initialize(), 0);
    
    // 测试异步操作在未连接状态下的行为
    MQTTRouterRpcClient::SubscribeRequest sub_req(test_allocator_);
    sub_req.server_id = MQTTString("server1", MQTTStrAllocator(test_allocator_));
    sub_req.client_id = MQTTString("client1", MQTTStrAllocator(test_allocator_));
    sub_req.topic_filter = MQTTString("test/topic", MQTTStrAllocator(test_allocator_));
    sub_req.qos = 1;
    
    // 异步订阅应该失败（因为未连接）
    ASSERT_NE(rpc_client_->subscribe_async(sub_req), 0);
    
    // 验证统计信息
    ASSERT_GT(rpc_client_->get_failed_requests(), 0);
}

TEST_F(MQTTRouterRpcClientTest, RouteTargetOperations) {
    // 测试路由目标结构
    MQTTRouterRpcClient::RouteTarget target(test_allocator_);
    target.server_id = MQTTString("server1", MQTTStrAllocator(test_allocator_));
    target.client_id = MQTTString("client1", MQTTStrAllocator(test_allocator_));
    target.qos = 2;
    
    ASSERT_EQ(target.server_id, MQTTString("server1", MQTTStrAllocator(test_allocator_)));
    ASSERT_EQ(target.client_id, MQTTString("client1", MQTTStrAllocator(test_allocator_)));
    ASSERT_EQ(target.qos, 2);
    
    // 测试路由目标向量
    MQTTRouterRpcClient::RouteTargetVector targets{MQTTSTLAllocator<MQTTRouterRpcClient::RouteTarget>(test_allocator_)};
    targets.push_back(target);
    
    ASSERT_EQ(targets.size(), 1);
    ASSERT_EQ(targets[0].server_id, target.server_id);
    ASSERT_EQ(targets[0].client_id, target.client_id);
    ASSERT_EQ(targets[0].qos, target.qos);
}

TEST_F(MQTTRouterRpcClientTest, RouterStatistics) {
    // 测试路由器统计信息结构
    MQTTRouterRpcClient::RouterStatistics stats;
    
    // 验证默认值
    ASSERT_EQ(stats.total_servers, 0);
    ASSERT_EQ(stats.total_clients, 0);
    ASSERT_EQ(stats.total_subscriptions, 0);
    ASSERT_EQ(stats.total_routes, 0);
    ASSERT_EQ(stats.memory_usage_bytes, 0);
    ASSERT_EQ(stats.redo_log_entries, 0);
    ASSERT_EQ(stats.last_snapshot_time, 0);
    
    // 设置值
    stats.total_servers = 5;
    stats.total_clients = 100;
    stats.total_subscriptions = 500;
    stats.memory_usage_bytes = 1024 * 1024;
    
    ASSERT_EQ(stats.total_servers, 5);
    ASSERT_EQ(stats.total_clients, 100);
    ASSERT_EQ(stats.total_subscriptions, 500);
    ASSERT_EQ(stats.memory_usage_bytes, 1024 * 1024);
}

TEST_F(MQTTRouterRpcClientTest, ErrorHandling) {
    // 测试错误码和错误消息
    ASSERT_EQ(rpc_client_->get_last_error_code(), 0);
    ASSERT_EQ(rpc_client_->get_last_error_time(), 0);
    
    // 测试在未连接状态下的同步操作
    MQTTRouterRpcClient::SubscribeRequest sub_req(test_allocator_);
    sub_req.server_id = MQTTString("server1", MQTTStrAllocator(test_allocator_));
    sub_req.client_id = MQTTString("client1", MQTTStrAllocator(test_allocator_));
    sub_req.topic_filter = MQTTString("test/topic", MQTTStrAllocator(test_allocator_));
    sub_req.qos = 1;
    
    // 同步操作应该失败
    ASSERT_NE(rpc_client_->subscribe(sub_req), 0);
    
    // 验证错误统计
    ASSERT_GT(rpc_client_->get_failed_requests(), 0);
}

// Mock路由服务器用于测试
class MockRouterServer {
public:
    MockRouterServer(int port) : port_(port), should_stop_(false) {}
    
    void start() {
        server_thread_ = std::thread(&MockRouterServer::run, this);
    }
    
    void stop() {
        should_stop_ = true;
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
    
private:
    void run() {
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) return;
        
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);
        
        if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(listen_fd);
            return;
        }
        
        if (listen(listen_fd, 10) < 0) {
            close(listen_fd);
            return;
        }
        
        while (!should_stop_) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listen_fd, &read_fds);
            
            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;  // 100ms
            
            int activity = select(listen_fd + 1, &read_fds, nullptr, nullptr, &timeout);
            if (activity > 0 && FD_ISSET(listen_fd, &read_fds)) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
                if (client_fd >= 0) {
                    // 简单的模拟：立即关闭连接
                    close(client_fd);
                }
            }
        }
        
        close(listen_fd);
    }
    
    int port_;
    std::atomic<bool> should_stop_;
    std::thread server_thread_;
};

class MQTTRouterRpcClientWithMockServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 启动模拟服务器
        mock_server_.reset(new MockRouterServer(19093));
        mock_server_->start();
        
        // 等待服务器启动
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // 获取根分配器
        MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        test_allocator_ = root_allocator->create_child("test_rpc_with_mock", MQTTMemoryTag::MEM_TAG_ROOT);
        
        // 配置RPC客户端连接到模拟服务器
        config_.router_host = "127.0.0.1";
        config_.router_port = 19093;
        config_.connect_timeout_ms = 1000;
        config_.request_timeout_ms = 2000;
        config_.max_retry_count = 1;
        config_.retry_delay_ms = 100;
        config_.enable_heartbeat = false;
        config_.max_pending_requests = 10;
        
        rpc_client_.reset(new MQTTRouterRpcClient(test_allocator_, config_));
    }

    void TearDown() override {
        if (rpc_client_) {
            rpc_client_->disconnect();
            rpc_client_.reset();
        }
        
        if (mock_server_) {
            mock_server_->stop();
            mock_server_.reset();
        }
        
        if (test_allocator_ && test_allocator_->get_parent()) {
            test_allocator_->get_parent()->remove_child("test_rpc_with_mock");
        }
    }

    MQTTAllocator* test_allocator_;
    MQTTRouterRpcClient::RpcClientConfig config_;
    std::unique_ptr<MQTTRouterRpcClient> rpc_client_;
    std::unique_ptr<MockRouterServer> mock_server_;
};

TEST_F(MQTTRouterRpcClientWithMockServerTest, ConnectionToMockServer) {
    // 测试初始化
    ASSERT_EQ(rpc_client_->initialize(), 0);
    
    // 测试连接到模拟服务器
    // 注意：模拟服务器会立即关闭连接，所以这个测试验证连接尝试
    int result = rpc_client_->connect();
    // 连接可能会失败，因为模拟服务器立即关闭连接
    // 这里主要测试连接流程不会崩溃
    
    // 验证连接状态
    // 由于模拟服务器的行为，连接状态可能是false
    
    // 测试断开连接
    ASSERT_EQ(rpc_client_->disconnect(), 0);
}

// 压力测试
class MQTTRouterRpcClientStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        test_allocator_ = root_allocator->create_child("test_stress", MQTTMemoryTag::MEM_TAG_ROOT);
        
        config_.router_host = "127.0.0.1";
        config_.router_port = 19094;
        config_.connect_timeout_ms = 500;
        config_.request_timeout_ms = 1000;
        config_.max_retry_count = 1;
        config_.retry_delay_ms = 50;
        config_.enable_heartbeat = false;
        config_.max_pending_requests = 1000;
        
        rpc_client_.reset(new MQTTRouterRpcClient(test_allocator_, config_));
    }

    void TearDown() override {
        if (rpc_client_) {
            rpc_client_->disconnect();
            rpc_client_.reset();
        }
        
        if (test_allocator_ && test_allocator_->get_parent()) {
            test_allocator_->get_parent()->remove_child("test_stress");
        }
    }

    MQTTAllocator* test_allocator_;
    MQTTRouterRpcClient::RpcClientConfig config_;
    std::unique_ptr<MQTTRouterRpcClient> rpc_client_;
};

TEST_F(MQTTRouterRpcClientStressTest, ManyRequestStructures) {
    const int num_requests = 1000;
    
    // 测试创建大量请求结构不会导致内存问题
    std::vector<std::unique_ptr<MQTTRouterRpcClient::SubscribeRequest>> requests;
    
    for (int i = 0; i < num_requests; ++i) {
        auto req = std::make_unique<MQTTRouterRpcClient::SubscribeRequest>(test_allocator_);
        req->server_id = to_mqtt_string("server" + std::to_string(i % 10), test_allocator_);
        req->client_id = to_mqtt_string("client" + std::to_string(i), test_allocator_);
        req->topic_filter = to_mqtt_string("test/topic/" + std::to_string(i), test_allocator_);
        req->qos = static_cast<uint8_t>(i % 3);
        
        requests.push_back(std::move(req));
    }
    
    // 验证所有请求都正确创建
    ASSERT_EQ(requests.size(), num_requests);
    
    // 验证一些请求的内容
    ASSERT_EQ(requests[0]->server_id, MQTTString("server0", MQTTStrAllocator(test_allocator_)));
    ASSERT_EQ(requests[0]->client_id, MQTTString("client0", MQTTStrAllocator(test_allocator_)));
    ASSERT_EQ(requests[999]->server_id, MQTTString("server9", MQTTStrAllocator(test_allocator_)));
    ASSERT_EQ(requests[999]->client_id, MQTTString("client999", MQTTStrAllocator(test_allocator_)));
    
    // 清理
    requests.clear();
}

// 主函数
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}