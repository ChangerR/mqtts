#include <gtest/gtest.h>
#include "../src/mqtt_session_manager_v2.h"
#include "../src/mqtt_allocator.h"
#include "../src/logger.h"

using namespace mqtt;

// Simple test to isolate the crash
TEST(SimpleMemoryTest, BasicAllocatorTest) {
    // Test basic allocator functionality
    MQTTAllocator* root = MQTTMemoryManager::get_instance().get_root_allocator();
    EXPECT_NE(root, nullptr);
    
    // Test basic allocation
    void* ptr = root->allocate(100);
    EXPECT_NE(ptr, nullptr);
    root->deallocate(ptr, 100);
}

TEST(SimpleMemoryTest, GlobalSessionManagerCreation) {
    // Test creating and destroying GlobalSessionManager
    {
        GlobalSessionManager manager;
        MQTTAllocator* allocator = manager.get_allocator();
        EXPECT_NE(allocator, nullptr);
    } // manager should be destroyed here
}

TEST(SimpleMemoryTest, ThreadRegistration) {
    GlobalSessionManager manager;
    
    int result = manager.pre_register_threads(1, 100);
    EXPECT_EQ(result, MQ_SUCCESS);
    
    // Test just the first step to isolate the crash
    std::thread::id thread_id = std::this_thread::get_id();
    
    LOG_INFO("About to register thread manager...");
    ThreadLocalSessionManager* thread_manager = manager.register_thread_manager(thread_id);
    LOG_INFO("Thread manager registration completed");
    
    EXPECT_NE(thread_manager, nullptr);
    
    result = manager.finalize_thread_registration();
    EXPECT_EQ(result, MQ_SUCCESS);
}

// Test just allocator creation to isolate the issue
TEST(SimpleMemoryTest, AllocatorCreation) {
    MQTTAllocator* root = MQTTMemoryManager::get_instance().get_root_allocator();
    MQTTAllocator* child = root->create_child("test_child", MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
    EXPECT_NE(child, nullptr);
    
    // Don't manually delete - it should be cleaned up by parent
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}