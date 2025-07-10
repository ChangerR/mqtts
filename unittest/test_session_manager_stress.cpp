#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <random>
#include "../src/mqtt_session_manager_v2.h"
#include "../src/mqtt_allocator.h"
#include "../src/mqtt_memory_tags.h"
#include "../src/mqtt_stl_allocator.h"
#include "../src/mqtt_packet.h"
#include "../src/mqtt_define.h"
#include "../src/logger.h"
#include "coroutine_test_helper.h"

using namespace mqtt;

class SessionManagerStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize coroutine environment
        coro_scope_ = std::make_unique<mqtt::test::CoroutineTestScope>();
        
        root_allocator_ = MQTTMemoryManager::get_instance().get_root_allocator();
        initial_usage_ = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    }
    
    void TearDown() override {
        // Clean up coroutine environment
        coro_scope_.reset();
        
        // Wait for cleanup to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    bool is_coroutine_available() const {
        return coro_scope_ && coro_scope_->is_available();
    }
    
    MQTTAllocator* root_allocator_;
    size_t initial_usage_;
    std::unique_ptr<mqtt::test::CoroutineTestScope> coro_scope_;
};

// Test large allocator creation and destruction
TEST_F(SessionManagerStressTest, MassiveAllocatorCreationDestruction) {
    const int num_allocators = 100;
    std::vector<std::string> allocator_ids;
    
    // Create many child allocators
    for (int i = 0; i < num_allocators; ++i) {
        std::string id = "stress_test_" + std::to_string(i);
        MQTTAllocator* child = root_allocator_->create_child(id, MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
        
        if (child != nullptr) {
            allocator_ids.push_back(id);
            
            // Allocate some memory in each allocator
            void* ptr = child->allocate(64 + (i % 128));  // Variable size
            EXPECT_NE(ptr, nullptr);
            
            if (ptr) {
                child->deallocate(ptr, 64 + (i % 128));
            }
        }
    }
    
    EXPECT_GT(allocator_ids.size(), num_allocators / 2);  // At least half created
    
    size_t peak_usage = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    LOG_INFO("Massive allocator test - created {} allocators, peak usage: {} bytes", 
             allocator_ids.size(), peak_usage);
    
    // Clean up all allocators
    for (const std::string& id : allocator_ids) {
        root_allocator_->remove_child(id);
    }
    
    // Verify memory cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    size_t final_usage = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    EXPECT_LE(final_usage, peak_usage);
}

// Test rapid GlobalSessionManager creation and destruction
TEST_F(SessionManagerStressTest, RapidManagerCreationDestruction) {
    const int num_iterations = 50;
    std::vector<size_t> usage_snapshots;
    
    for (int i = 0; i < num_iterations; ++i) {
        {
            GlobalSessionManager manager;
            
            // Quick pre-register and configure
            int result = manager.pre_register_threads(1, 10);
            EXPECT_EQ(result, MQ_SUCCESS);
            
            size_t current_usage = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
            usage_snapshots.push_back(current_usage);
            
            // Simulate some work
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        // Manager destroyed, wait a bit
        if (i % 10 == 9) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    // Analyze memory usage patterns
    size_t max_usage = *std::max_element(usage_snapshots.begin(), usage_snapshots.end());
    size_t min_usage = *std::min_element(usage_snapshots.begin(), usage_snapshots.end());
    
    LOG_INFO("Rapid creation test - {} iterations, usage range: {} - {} bytes", 
             num_iterations, min_usage, max_usage);
    
    // Verify no serious memory leaks (allow some reasonable fluctuation)
    size_t final_usage = MQTTMemoryManager::get_instance().get_tag_usage(MQTTMemoryTag::MEM_TAG_SESSION_MANAGER);
    EXPECT_LT(final_usage, max_usage * 2);  // Should not exceed 2x peak
}

// Test memory fragmentation scenario (basic allocator stress)
TEST_F(SessionManagerStressTest, MemoryFragmentation) {
    const int num_cycles = 20;
    std::vector<MQTTAllocator*> allocators;
    std::vector<std::vector<void*>> allocated_ptrs(num_cycles);
    
    // Create some allocators
    for (int i = 0; i < 5; ++i) {
        std::string id = "frag_test_" + std::to_string(i);
        MQTTAllocator* allocator = root_allocator_->create_child(id, MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
        if (allocator) {
            allocators.push_back(allocator);
        }
    }
    
    ASSERT_GT(allocators.size(), 0);
    
    // Simulate fragmentation: allocate different sized memory blocks
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> size_dist(16, 512);
    std::uniform_int_distribution<> allocator_dist(0, allocators.size() - 1);
    
    for (int cycle = 0; cycle < num_cycles; ++cycle) {
        for (int alloc = 0; alloc < 10; ++alloc) {
            size_t size = size_dist(gen);
            int allocator_idx = allocator_dist(gen);
            
            void* ptr = allocators[allocator_idx]->allocate(size);
            if (ptr) {
                allocated_ptrs[cycle].push_back(ptr);
            }
        }
        
        // Randomly free some memory (simulate fragmentation)
        if (cycle > 0 && cycle % 3 == 0) {
            int prev_cycle = cycle - 1;
            if (!allocated_ptrs[prev_cycle].empty()) {
                for (size_t i = 0; i < allocated_ptrs[prev_cycle].size(); i += 2) {
                    // Only free half, creating fragments
                    int allocator_idx = allocator_dist(gen);
                    allocators[allocator_idx]->deallocate(allocated_ptrs[prev_cycle][i], size_dist(gen));
                }
                allocated_ptrs[prev_cycle].clear();
            }
        }
    }
    
    // Record memory usage after fragmentation
    size_t fragmented_usage = 0;
    for (MQTTAllocator* allocator : allocators) {
        fragmented_usage += allocator->get_memory_usage();
    }
    
    LOG_INFO("Memory fragmentation test - total usage after fragmentation: {} bytes", fragmented_usage);
    
    // Clean up all allocated memory
    for (int cycle = 0; cycle < num_cycles; ++cycle) {
        for (void* ptr : allocated_ptrs[cycle]) {
            if (ptr) {
                // Simplified cleanup, use first allocator
                if (!allocators.empty()) {
                    allocators[0]->deallocate(ptr, 64);  // Use average size
                }
            }
        }
        allocated_ptrs[cycle].clear();
    }
    
    // Clean up allocators
    for (int i = 0; i < 5; ++i) {
        std::string id = "frag_test_" + std::to_string(i);
        root_allocator_->remove_child(id);
    }
}

// Test boundary condition: zero size allocation
TEST_F(SessionManagerStressTest, ZeroSizeAllocation) {
    MQTTAllocator* test_allocator = root_allocator_->create_child("zero_test", MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
    ASSERT_NE(test_allocator, nullptr);
    
    // Test zero size allocation
    void* ptr = test_allocator->allocate(0);
    // Depending on implementation, may return nullptr or valid pointer
    
    if (ptr != nullptr) {
        test_allocator->deallocate(ptr, 0);
    }
    
    // Verify memory usage
    EXPECT_EQ(test_allocator->get_memory_usage(), 0);
    
    root_allocator_->remove_child("zero_test");
}

// Test boundary condition: large memory allocation
TEST_F(SessionManagerStressTest, LargeMemoryAllocation) {
    MQTTAllocator* test_allocator = root_allocator_->create_child("large_test", MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
    ASSERT_NE(test_allocator, nullptr);
    
    size_t initial_usage = test_allocator->get_memory_usage();
    
    // Test large memory allocation (1MB)
    const size_t large_size = 1024 * 1024;
    void* large_ptr = test_allocator->allocate(large_size);
    
    if (large_ptr != nullptr) {
        // Verify memory usage increased
        size_t usage_after_large = test_allocator->get_memory_usage();
        EXPECT_GT(usage_after_large, initial_usage);
        
        LOG_INFO("Large allocation test - allocated {} bytes, usage increased by {} bytes", 
                 large_size, usage_after_large - initial_usage);
        
        // Free large memory block
        test_allocator->deallocate(large_ptr, large_size);
        
        // Verify memory usage decreased
        size_t usage_after_free = test_allocator->get_memory_usage();
        EXPECT_LE(usage_after_free, usage_after_large);
    } else {
        LOG_INFO("Large allocation ({} bytes) failed - this may be expected", large_size);
    }
    
    root_allocator_->remove_child("large_test");
}

// Test concurrent allocator operations (simplified)
TEST_F(SessionManagerStressTest, ConcurrentAllocatorOperations) {
    const int num_threads = 4;
    const int operations_per_thread = 50;
    std::atomic<int> successful_operations{0};
    std::atomic<int> failed_operations{0};
    
    std::vector<std::thread> threads;
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, operations_per_thread, &successful_operations, &failed_operations]() {
            for (int op = 0; op < operations_per_thread; ++op) {
                try {
                    std::string allocator_id = "concurrent_" + std::to_string(t) + "_" + std::to_string(op);
                    
                    // Create allocator
                    MQTTAllocator* allocator = root_allocator_->create_child(allocator_id, MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
                    
                    if (allocator != nullptr) {
                        // Allocate memory
                        void* ptr = allocator->allocate(128 + (op % 256));
                        
                        if (ptr != nullptr) {
                            // Simulate some work
                            std::this_thread::sleep_for(std::chrono::microseconds(10));
                            
                            // Free memory
                            allocator->deallocate(ptr, 128 + (op % 256));
                            successful_operations.fetch_add(1);
                        } else {
                            failed_operations.fetch_add(1);
                        }
                        
                        // Clean up allocator
                        root_allocator_->remove_child(allocator_id);
                    } else {
                        failed_operations.fetch_add(1);
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Concurrent operation failed: {}", e.what());
                    failed_operations.fetch_add(1);
                }
            }
        });
    }
    
    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
    
    int total_operations = num_threads * operations_per_thread;
    int actual_successful = successful_operations.load();
    int actual_failed = failed_operations.load();
    
    LOG_INFO("Concurrent test - {} threads, {} ops/thread, successful: {}, failed: {}", 
             num_threads, operations_per_thread, actual_successful, actual_failed);
    
    // Verify operation results
    EXPECT_EQ(actual_successful + actual_failed, total_operations);
    EXPECT_GT(actual_successful, total_operations / 2);  // At least half successful
}

// Test memory usage statistics accuracy
TEST_F(SessionManagerStressTest, MemoryUsageAccuracy) {
    std::vector<MQTTAllocator*> allocators;
    std::vector<std::pair<void*, size_t>> allocations;
    
    // Create multiple allocators
    for (int i = 0; i < 3; ++i) {
        std::string id = "accuracy_test_" + std::to_string(i);
        MQTTAllocator* allocator = root_allocator_->create_child(id, MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
        if (allocator) {
            allocators.push_back(allocator);
        }
    }
    
    ASSERT_GT(allocators.size(), 0);
    
    size_t expected_total_usage = 0;
    
    // Perform precise tracking allocations
    std::vector<size_t> allocation_sizes = {64, 128, 256, 512, 1024};
    
    for (size_t size : allocation_sizes) {
        for (MQTTAllocator* allocator : allocators) {
            void* ptr = allocator->allocate(size);
            if (ptr) {
                allocations.push_back({ptr, size});
                expected_total_usage += size;
            }
        }
    }
    
    // Verify each allocator's usage
    size_t actual_total_usage = 0;
    for (MQTTAllocator* allocator : allocators) {
        actual_total_usage += allocator->get_memory_usage();
    }
    
    LOG_INFO("Memory accuracy test - expected: {} bytes, actual: {} bytes", 
             expected_total_usage, actual_total_usage);
    
    // Allow some reasonable difference (due to memory alignment etc.)
    EXPECT_LE(actual_total_usage, expected_total_usage * 1.5);  // Not more than 1.5x expected
    EXPECT_GE(actual_total_usage, expected_total_usage * 0.8);  // Not less than 0.8x expected
    
    // Precisely free all memory
    for (auto& alloc : allocations) {
        // Find corresponding allocator (simplified, use first one)
        if (!allocators.empty()) {
            allocators[0]->deallocate(alloc.first, alloc.second);
        }
    }
    
    // Verify usage after freeing
    size_t final_usage = 0;
    for (MQTTAllocator* allocator : allocators) {
        final_usage += allocator->get_memory_usage();
    }
    
    EXPECT_EQ(final_usage, 0);
    
    // Clean up allocators
    for (int i = 0; i < 3; ++i) {
        std::string id = "accuracy_test_" + std::to_string(i);
        root_allocator_->remove_child(id);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}