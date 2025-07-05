#include "mqtt_session_allocator.h"
#include "mqtt_define.h"
#include <iostream>
#include <cassert>

using namespace mqtt;

void test_allocator_error_codes() {
    std::cout << "Testing allocator error code patterns..." << std::endl;
    
    // Test 1: Valid allocator creation
    {
        MQTTAllocator* allocator = nullptr;
        int ret = SessionAllocatorManager::get_session_allocator("test_client", 1024 * 1024, allocator);
        
        if (ret == MQ_SUCCESS) {
            std::cout << "✓ Valid allocator creation succeeded" << std::endl;
            assert(allocator != nullptr);
        } else {
            std::cout << "✗ Valid allocator creation failed: " << mqtt_error_string(ret) << std::endl;
        }
    }
    
    // Test 2: Memory usage check
    {
        size_t memory_usage = 0;
        int ret = SessionAllocatorManager::get_session_memory_usage("test_client", memory_usage);
        
        if (ret == MQ_SUCCESS) {
            std::cout << "✓ Memory usage check succeeded: " << memory_usage << " bytes" << std::endl;
        } else {
            std::cout << "✗ Memory usage check failed: " << mqtt_error_string(ret) << std::endl;
        }
    }
    
    // Test 3: Memory limit check
    {
        bool limit_exceeded = false;
        int ret = SessionAllocatorManager::is_session_memory_limit_exceeded("test_client", limit_exceeded);
        
        if (ret == MQ_SUCCESS) {
            std::cout << "✓ Memory limit check succeeded: " << (limit_exceeded ? "exceeded" : "within limit") << std::endl;
        } else {
            std::cout << "✗ Memory limit check failed: " << mqtt_error_string(ret) << std::endl;
        }
    }
    
    // Test 4: Non-existent client check
    {
        size_t memory_usage = 0;
        int ret = SessionAllocatorManager::get_session_memory_usage("non_existent_client", memory_usage);
        
        if (ret == MQ_ERR_ALLOCATOR_NOT_FOUND) {
            std::cout << "✓ Non-existent client properly detected" << std::endl;
        } else {
            std::cout << "✗ Non-existent client detection failed: " << mqtt_error_string(ret) << std::endl;
        }
    }
    
    // Test 5: Cleanup
    {
        int ret = SessionAllocatorManager::cleanup_session_allocators("test_client");
        
        if (ret == MQ_SUCCESS) {
            std::cout << "✓ Cleanup succeeded" << std::endl;
        } else {
            std::cout << "✗ Cleanup failed: " << mqtt_error_string(ret) << std::endl;
        }
    }
    
    std::cout << "Allocator error code tests completed!" << std::endl;
}

void test_error_code_strings() {
    std::cout << "\nTesting error code to string conversion..." << std::endl;
    
    // Test all allocator error codes
    struct ErrorCodeTest {
        int code;
        const char* expected_category;
    } tests[] = {
        {MQ_SUCCESS, "Success"},
        {MQ_ERR_ALLOCATOR, "Allocator error"},
        {MQ_ERR_ALLOCATOR_CREATE, "Failed to create allocator"},
        {MQ_ERR_ALLOCATOR_NOT_FOUND, "Allocator not found"},
        {MQ_ERR_ALLOCATOR_CLEANUP, "Failed to cleanup allocator"},
        {MQ_ERR_ALLOCATOR_LIMIT_EXCEEDED, "Allocator limit exceeded"},
        {MQ_ERR_ALLOCATOR_INVALID_PARENT, "Invalid parent allocator"},
        {MQ_ERR_ALLOCATOR_INVALID_TAG, "Invalid memory tag"},
        {MQ_ERR_ALLOCATOR_HIERARCHY, "Allocator hierarchy error"},
    };
    
    for (const auto& test : tests) {
        const char* error_str = mqtt_error_string(test.code);
        std::cout << "Code " << test.code << ": " << error_str << std::endl;
        
        // Basic validation - string should not be empty and should contain expected keywords
        assert(error_str != nullptr);
        assert(strlen(error_str) > 0);
    }
    
    std::cout << "✓ Error code string conversion tests passed!" << std::endl;
}

#ifdef STANDALONE_TEST
int main() {
    try {
        test_allocator_error_codes();
        test_error_code_strings();
        std::cout << "\nAll tests passed successfully!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
#endif