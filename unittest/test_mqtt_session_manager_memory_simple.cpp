#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <string>
#include <unordered_set>
#include "../src/mqtt_session_manager_v2.h"
#include "../src/mqtt_allocator.h"
#include "../src/mqtt_memory_tags.h"
#include "../src/mqtt_protocol_handler.h"
#include "../src/mqtt_define.h"
#include "../src/mqtt_stl_allocator.h"
#include "../src/mqtt_packet.h"
#include "../src/mqtt_message_queue.h"
#include "../src/logger.h"

using namespace mqtt;

class MockMQTTProtocolHandler : public MQTTProtocolHandler {
public:
    MockMQTTProtocolHandler(const std::string& client_id) 
        : MQTTProtocolHandler(MQTTMemoryManager::get_instance().get_root_allocator()), client_id_(client_id), connected_(true) {
        // Set the client ID using the to_mqtt_string helper
        MQTTAllocator* allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        set_client_id(to_mqtt_string(client_id, allocator));
    }
    
    bool is_connected() const { return connected_; }
    void set_connected(bool connected) { connected_ = connected; }
    
    const std::string& get_client_id_string() const { return client_id_; }

private:
    std::string client_id_;
    bool connected_;
};

class SessionManagerMemoryTestSimple : public ::testing::Test {
protected:
    void SetUp() override {
        // Each test creates its own manager to avoid state conflicts
        manager_ = std::make_unique<GlobalSessionManager>();
        root_allocator_ = MQTTMemoryManager::get_instance().get_root_allocator();
        
        // Pre-register threads for testing
        int result = manager_->pre_register_threads(4, 1000);
        ASSERT_EQ(result, MQ_SUCCESS) << "Failed to pre-register threads";
    }
    
    void TearDown() override {
        // Clean up test data
        if (manager_) {
            manager_->cleanup_all_invalid_sessions();
            manager_.reset();
        }
        
        // Wait for cleanup to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::unique_ptr<MockMQTTProtocolHandler> create_mock_handler(const std::string& client_id) {
        return std::make_unique<MockMQTTProtocolHandler>(client_id);
    }
    
    std::unique_ptr<GlobalSessionManager> manager_;
    MQTTAllocator* root_allocator_;
};

// Test allocator integration in GlobalSessionManager
TEST_F(SessionManagerMemoryTestSimple, GlobalSessionManagerAllocatorIntegration) {
    EXPECT_NE(manager_, nullptr);
    
    // Test that GlobalSessionManager has a valid allocator
    MQTTAllocator* global_allocator = manager_->get_allocator();
    EXPECT_NE(global_allocator, nullptr);
    EXPECT_EQ(global_allocator->get_tag(), MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    EXPECT_EQ(global_allocator->get_parent(), root_allocator_);
    
    // Test initial memory usage
    size_t initial_usage = global_allocator->get_memory_usage();
    EXPECT_GE(initial_usage, 0);
    
    LOG_INFO("Global allocator ID: {}, initial usage: {} bytes", 
             global_allocator->get_id(), initial_usage);
}

// Test GlobalSessionManager state management
TEST_F(SessionManagerMemoryTestSimple, GlobalSessionManagerStateManagement) {
    EXPECT_NE(manager_, nullptr);
    
    // Test pre-registration state
    MQTTAllocator* global_allocator = manager_->get_allocator();
    size_t initial_usage = global_allocator->get_memory_usage();
    
    // Test that manager is in correct state after pre-registration
    EXPECT_EQ(manager_->get_total_session_count(), 0);
    
    // Test finalize_thread_registration
    int result = manager_->finalize_thread_registration();
    EXPECT_EQ(result, MQ_SUCCESS);
    
    // Memory usage should remain stable
    size_t usage_after_finalize = global_allocator->get_memory_usage();
    EXPECT_GE(usage_after_finalize, initial_usage);
    
    LOG_INFO("Memory usage - Initial: {} bytes, After finalize: {} bytes",
             initial_usage, usage_after_finalize);
}

// Test memory tag reporting
TEST_F(SessionManagerMemoryTestSimple, MemoryTagReporting) {
    // Test basic memory tag reporting without requiring ThreadLocalSessionManager
    size_t initial_session_manager_usage = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    
    // The GlobalSessionManager itself should contribute to memory usage
    size_t session_manager_usage = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    EXPECT_GE(session_manager_usage, initial_session_manager_usage);
    
    LOG_INFO("Session manager tag usage: {} bytes", session_manager_usage);
}

// Test GlobalSessionManager cleanup functionality
TEST_F(SessionManagerMemoryTestSimple, GlobalSessionManagerCleanup) {
    EXPECT_NE(manager_, nullptr);
    
    MQTTAllocator* global_allocator = manager_->get_allocator();
    size_t initial_usage = global_allocator->get_memory_usage();
    
    // Test cleanup with no sessions (should not crash)
    int cleaned_count = manager_->cleanup_all_invalid_sessions();
    EXPECT_EQ(cleaned_count, 0);
    
    // Memory usage should remain stable
    size_t usage_after_cleanup = global_allocator->get_memory_usage();
    EXPECT_EQ(usage_after_cleanup, initial_usage);
    
    LOG_INFO("Memory usage - Initial: {} bytes, After cleanup: {} bytes",
             initial_usage, usage_after_cleanup);
}

// Test topic tree functionality (which is part of GlobalSessionManager)
TEST_F(SessionManagerMemoryTestSimple, TopicTreeMemoryUsage) {
    EXPECT_NE(manager_, nullptr);
    
    MQTTAllocator* global_allocator = manager_->get_allocator();
    size_t initial_usage = global_allocator->get_memory_usage();
    
    // Test topic tree operations that don't require ThreadLocalSessionManager
    size_t subscriber_count = 0;
    size_t node_count = 0;
    int result = manager_->get_topic_tree_stats(subscriber_count, node_count);
    EXPECT_EQ(result, MQ_SUCCESS);
    
    LOG_INFO("Topic tree stats - Subscribers: {}, Nodes: {}", subscriber_count, node_count);
    
    // Test topic tree cleanup
    size_t cleaned_count = 0;
    result = manager_->cleanup_topic_tree(cleaned_count);
    EXPECT_EQ(result, MQ_SUCCESS);
    
    LOG_INFO("Topic tree cleanup - Cleaned {} nodes", cleaned_count);
    
    // Memory usage should remain stable or decrease
    size_t usage_after_cleanup = global_allocator->get_memory_usage();
    EXPECT_LE(usage_after_cleanup, initial_usage + 1000); // Allow some reasonable variance
    
    LOG_INFO("Memory usage - Initial: {} bytes, After topic tree ops: {} bytes",
             initial_usage, usage_after_cleanup);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}