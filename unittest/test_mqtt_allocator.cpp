#include <iostream>
#include <vector>
#include <cassert>
#include "src/mqtt_allocator.h"

void test_basic_allocation() {
    std::cout << "测试基本内存分配..." << std::endl;
    
    MQTTAllocator allocator("test_client", "test", 0);
    
    // 测试分配内存
    void* ptr = allocator.allocate(1024);
    assert(ptr != nullptr);
    std::cout << "分配1024字节内存成功" << std::endl;
    
    // 测试释放内存
    allocator.deallocate(ptr, 1024);
    std::cout << "释放内存成功" << std::endl;
}

void test_multiple_allocations() {
    std::cout << "\n测试多次内存分配..." << std::endl;
    
    MQTTAllocator allocator("test_client", "test", 0);
    std::vector<void*> ptrs;
    
    // 分配多个不同大小的内存块
    const size_t sizes[] = {64, 128, 256, 512, 1024};
    for (size_t size : sizes) {
        void* ptr = allocator.allocate(size);
        assert(ptr != nullptr);
        ptrs.push_back(ptr);
        std::cout << "分配" << size << "字节内存成功" << std::endl;
    }
    
    // 释放所有内存
    for (size_t i = 0; i < ptrs.size(); i++) {
        allocator.deallocate(ptrs[i], sizes[i]);
        std::cout << "释放" << sizes[i] << "字节内存成功" << std::endl;
    }
}

void test_large_allocation() {
    std::cout << "\n测试大内存分配..." << std::endl;
    
    MQTTAllocator allocator("test_client", "test", 0);
    
    // 测试分配较大内存块
    const size_t large_size = 1024 * 1024;  // 1MB
    void* ptr = allocator.allocate(large_size);
    assert(ptr != nullptr);
    std::cout << "分配" << large_size << "字节内存成功" << std::endl;
    
    allocator.deallocate(ptr, large_size);
    std::cout << "释放大内存成功" << std::endl;
}

int main() {
    std::cout << "开始MQTT分配器测试\n" << std::endl;
    
    try {
        test_basic_allocation();
        test_multiple_allocations();
        test_large_allocation();
        
        std::cout << "\n所有测试通过！" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n测试失败: " << e.what() << std::endl;
        return 1;
    }
} 