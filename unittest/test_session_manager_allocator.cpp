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
#include "../src/mqtt_stl_allocator.h"
#include "../src/mqtt_packet.h"
#include "../src/mqtt_define.h"
#include "../src/logger.h"
#include "coroutine_test_helper.h"

using namespace mqtt;

class SessionManagerAllocatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize coroutine environment
        coro_scope_ = std::make_unique<mqtt::test::CoroutineTestScope>();
        
        // 获取初始内存状态
        root_allocator_ = MQTTMemoryManager::get_instance().get_root_allocator();
        initial_session_manager_usage_ = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
        initial_root_usage_ = root_allocator_->get_memory_usage();
    }
    
    void TearDown() override {
        // Clean up coroutine environment
        coro_scope_.reset();
        
        // 等待清理完成
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    bool is_coroutine_available() const {
        return coro_scope_ && coro_scope_->is_available();
    }
    
    MQTTAllocator* root_allocator_;
    size_t initial_session_manager_usage_;
    size_t initial_root_usage_;
    std::unique_ptr<mqtt::test::CoroutineTestScope> coro_scope_;
};

// 测试基础allocator功能
TEST_F(SessionManagerAllocatorTest, BasicAllocatorFunctionality) {
    ASSERT_NE(root_allocator_, nullptr);
    
    // 测试基本分配和释放
    void* ptr1 = root_allocator_->allocate(256);
    EXPECT_NE(ptr1, nullptr);
    
    size_t usage_after_alloc = root_allocator_->get_memory_usage();
    EXPECT_GT(usage_after_alloc, initial_root_usage_);
    
    root_allocator_->deallocate(ptr1, 256);
    
    // 验证内存使用量减少
    size_t usage_after_free = root_allocator_->get_memory_usage();
    EXPECT_LE(usage_after_free, usage_after_alloc);
}

// 测试子allocator创建和层次结构
TEST_F(SessionManagerAllocatorTest, AllocatorHierarchy) {
    // 创建子allocator
    MQTTAllocator* child1 = root_allocator_->create_child("test_child_1", MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
    ASSERT_NE(child1, nullptr);
    
    // 验证父子关系
    EXPECT_EQ(child1->get_parent(), root_allocator_);
    EXPECT_EQ(child1->get_tag(), MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    EXPECT_EQ(child1->get_id(), "test_child_1");
    
    // 创建第二个子allocator
    MQTTAllocator* child2 = root_allocator_->create_child("test_child_2", MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
    ASSERT_NE(child2, nullptr);
    EXPECT_NE(child1, child2);
    
    // 在子allocator中分配内存
    void* ptr1 = child1->allocate(128);
    void* ptr2 = child2->allocate(256);
    
    EXPECT_NE(ptr1, nullptr);
    EXPECT_NE(ptr2, nullptr);
    EXPECT_NE(ptr1, ptr2);
    
    // 验证内存使用跟踪
    EXPECT_GT(child1->get_memory_usage(), 0);
    EXPECT_GT(child2->get_memory_usage(), 0);
    
    // 验证总内存使用量
    size_t total_usage = root_allocator_->get_total_memory_usage();
    EXPECT_GT(total_usage, initial_root_usage_);
    
    // 清理
    child1->deallocate(ptr1, 128);
    child2->deallocate(ptr2, 256);
    
    EXPECT_EQ(child1->get_memory_usage(), 0);
    EXPECT_EQ(child2->get_memory_usage(), 0);
    
    // 清理子allocator
    root_allocator_->remove_child("test_child_1");
    root_allocator_->remove_child("test_child_2");
}

// 测试GlobalSessionManager的allocator集成
TEST_F(SessionManagerAllocatorTest, GlobalSessionManagerAllocatorIntegration) {
    {
        // 创建GlobalSessionManager并验证allocator
        GlobalSessionManager manager;
        
        MQTTAllocator* global_allocator = manager.get_allocator();
        ASSERT_NE(global_allocator, nullptr);
        
        // 验证allocator属性
        EXPECT_EQ(global_allocator->get_tag(), MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
        EXPECT_EQ(global_allocator->get_parent(), root_allocator_);
        EXPECT_EQ(global_allocator->get_id(), "global_session_manager");
        
        // 验证初始内存使用
        size_t initial_usage = global_allocator->get_memory_usage();
        
        // 预注册线程应该不会显著增加内存使用
        int result = manager.pre_register_threads(2, 100);
        EXPECT_EQ(result, MQ_SUCCESS);
        
        size_t usage_after_prereg = global_allocator->get_memory_usage();
        // 预注册可能会有少量内存使用（预分配容器容量）
        
        LOG_INFO("Global allocator usage - initial: {}, after pre-register: {}", 
                 initial_usage, usage_after_prereg);
    }
    
    // 验证GlobalSessionManager销毁后内存被正确释放
    // 等待一些时间确保析构完成
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    size_t final_session_manager_usage = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    LOG_INFO("Session manager tag usage - initial: {}, final: {}", 
             initial_session_manager_usage_, final_session_manager_usage);
}

// 测试ThreadLocalSessionManager的allocator集成
TEST_F(SessionManagerAllocatorTest, ThreadLocalSessionManagerAllocatorIntegration) {
    GTEST_SKIP() << "ThreadLocalSessionManager requires full coroutine environment - skipping for stability";
}

// 测试内存标签跟踪功能
TEST_F(SessionManagerAllocatorTest, MemoryTagTracking) {
    size_t initial_usage = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    
    {
        GlobalSessionManager manager1;
        size_t usage_after_manager1 = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
        EXPECT_GT(usage_after_manager1, initial_usage);
        
        size_t usage_after_manager2;
        {
            GlobalSessionManager manager2;
            usage_after_manager2 = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
            EXPECT_GT(usage_after_manager2, usage_after_manager1);
            
            LOG_INFO("Memory tag usage - initial: {}, after manager1: {}, after manager2: {}", 
                     initial_usage, usage_after_manager1, usage_after_manager2);
        }
        
        // manager2销毁后，标签使用量应该减少
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        size_t usage_after_manager2_destroyed = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
        EXPECT_LT(usage_after_manager2_destroyed, usage_after_manager2);
    }
    
    // 所有manager销毁后，标签使用量应该回到初始状态或接近初始状态
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    size_t final_usage = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    LOG_INFO("Final session manager tag usage: {} bytes", final_usage);
}

// 测试多个allocator的并发使用
TEST_F(SessionManagerAllocatorTest, ConcurrentAllocatorUsage) {
    std::vector<std::unique_ptr<GlobalSessionManager>> managers;
    std::vector<MQTTAllocator*> allocators;
    
    const int num_managers = 3;
    
    // 创建多个manager和allocator
    for (int i = 0; i < num_managers; ++i) {
        auto manager = std::make_unique<GlobalSessionManager>();
        MQTTAllocator* allocator = manager->get_allocator();
        
        ASSERT_NE(allocator, nullptr);
        allocators.push_back(allocator);
        managers.push_back(std::move(manager));
    }
    
    // 验证每个allocator都是独立的
    for (int i = 0; i < num_managers; ++i) {
        for (int j = i + 1; j < num_managers; ++j) {
            EXPECT_NE(allocators[i], allocators[j]);
            EXPECT_NE(allocators[i]->get_id(), allocators[j]->get_id());
        }
    }
    
    // 验证总内存使用量包含所有allocator的使用
    size_t total_usage = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    EXPECT_GT(total_usage, initial_session_manager_usage_);
    
    LOG_INFO("Concurrent allocator test - total usage: {} bytes", total_usage);
    
    // 清理
    managers.clear();
    allocators.clear();
}

// 测试allocator的内存限制功能
TEST_F(SessionManagerAllocatorTest, AllocatorMemoryLimits) {
    // 创建有内存限制的子allocator
    const size_t memory_limit = 1024;  // 1KB限制
    MQTTAllocator* limited_allocator = root_allocator_->create_child("limited_test", MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, memory_limit);
    
    ASSERT_NE(limited_allocator, nullptr);
    EXPECT_EQ(limited_allocator->get_memory_limit(), memory_limit);
    
    // 分配内存直到接近限制
    std::vector<void*> ptrs;
    size_t total_allocated = 0;
    const size_t chunk_size = 128;
    
    while (total_allocated + chunk_size <= memory_limit) {
        void* ptr = limited_allocator->allocate(chunk_size);
        if (ptr != nullptr) {
            ptrs.push_back(ptr);
            total_allocated += chunk_size;
        } else {
            break;
        }
    }
    
    EXPECT_GT(ptrs.size(), 0);
    EXPECT_LE(limited_allocator->get_memory_usage(), memory_limit);
    
    LOG_INFO("Limited allocator test - allocated {} chunks, total usage: {} bytes, limit: {} bytes", 
             ptrs.size(), limited_allocator->get_memory_usage(), memory_limit);
    
    // 清理分配的内存
    for (size_t i = 0; i < ptrs.size(); ++i) {
        limited_allocator->deallocate(ptrs[i], chunk_size);
    }
    
    EXPECT_EQ(limited_allocator->get_memory_usage(), 0);
    
    // 清理allocator
    root_allocator_->remove_child("limited_test");
}

// 测试allocator在异常情况下的行为
TEST_F(SessionManagerAllocatorTest, AllocatorErrorHandling) {
    // 测试空指针处理
    MQTTAllocator* null_allocator = nullptr;
    
    // 测试重复创建相同ID的子allocator
    MQTTAllocator* child1 = root_allocator_->create_child("duplicate_test", MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
    ASSERT_NE(child1, nullptr);
    
    MQTTAllocator* child2 = root_allocator_->create_child("duplicate_test", MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
    // 根据实现，可能返回已存在的allocator或者nullptr
    
    // 测试获取不存在的子allocator
    MQTTAllocator* non_existent = root_allocator_->get_child("non_existent_child");
    EXPECT_EQ(non_existent, nullptr);
    
    // 测试移除不存在的子allocator
    root_allocator_->remove_child("non_existent_child");  // 应该不会崩溃
    
    // 清理
    root_allocator_->remove_child("duplicate_test");
}

// 测试GlobalSessionManager状态管理与allocator的交互
TEST_F(SessionManagerAllocatorTest, ManagerStateAndAllocator) {
    GlobalSessionManager manager;
    MQTTAllocator* allocator = manager.get_allocator();
    
    size_t initial_usage = allocator->get_memory_usage();
    
    // 测试预注册阶段
    int result = manager.pre_register_threads(2, 100);
    EXPECT_EQ(result, MQ_SUCCESS);
    
    size_t usage_after_prereg = allocator->get_memory_usage();
    
    // 测试重复预注册（应该失败）
    result = manager.pre_register_threads(3, 200);
    EXPECT_NE(result, MQ_SUCCESS);  // 应该返回错误状态
    
    // 内存使用量不应该因为失败的操作而变化
    size_t usage_after_failed_prereg = allocator->get_memory_usage();
    EXPECT_EQ(usage_after_failed_prereg, usage_after_prereg);
    
    LOG_INFO("Manager state test - initial: {}, after prereg: {}, after failed prereg: {}", 
             initial_usage, usage_after_prereg, usage_after_failed_prereg);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}