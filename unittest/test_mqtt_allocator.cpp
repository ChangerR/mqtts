#include <gtest/gtest.h>
#include <vector>
#include "src/mqtt_allocator.h"

// Test fixture for allocator tests
class MQTTAllocatorTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    // Create a new allocator for each test
    allocator = std::make_unique<MQTTAllocator>("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  }

  void TearDown() override { allocator.reset(); }

  std::unique_ptr<MQTTAllocator> allocator;
};

TEST_F(MQTTAllocatorTest, BasicAllocation)
{
  // Test basic memory allocation
  void* ptr = allocator->allocate(1024);
  ASSERT_NE(ptr, nullptr);

  // Test memory deallocation
  allocator->deallocate(ptr, 1024);
  SUCCEED();
}

TEST_F(MQTTAllocatorTest, MultipleAllocations)
{
  std::vector<void*> ptrs;

  // Allocate multiple blocks of different sizes
  const size_t sizes[] = {64, 128, 256, 512, 1024};
  for (size_t size : sizes) {
    void* ptr = allocator->allocate(size);
    ASSERT_NE(ptr, nullptr) << "Failed to allocate " << size << " bytes";
    ptrs.push_back(ptr);
  }

  // Deallocate all memory
  for (size_t i = 0; i < ptrs.size(); i++) {
    allocator->deallocate(ptrs[i], sizes[i]);
  }

  SUCCEED();
}

TEST_F(MQTTAllocatorTest, LargeAllocation)
{
  // Test large memory allocation
  const size_t large_size = 1024 * 1024;  // 1MB
  void* ptr = allocator->allocate(large_size);
  ASSERT_NE(ptr, nullptr) << "Failed to allocate " << large_size << " bytes";

  allocator->deallocate(ptr, large_size);
  SUCCEED();
}

TEST_F(MQTTAllocatorTest, ZeroSizeAllocation)
{
  // Test zero-size allocation
  void* ptr = allocator->allocate(0);
  // Zero-size allocation behavior depends on implementation
  // Some allocators return nullptr, others return a valid pointer
  if (ptr) {
    allocator->deallocate(ptr, 0);
  }
  SUCCEED();
}

TEST_F(MQTTAllocatorTest, NullPointerDeallocation)
{
  // Test deallocating null pointer (should not crash)
  allocator->deallocate(nullptr, 1024);
  SUCCEED();
}

TEST_F(MQTTAllocatorTest, AllocatorInfo)
{
  // Test allocator information
  std::string id = allocator->get_id();
  EXPECT_EQ(id, "test_client");

  MQTTMemoryTag tag = allocator->get_tag();
  EXPECT_EQ(tag, MQTTMemoryTag::MEM_TAG_CLIENT);
}

TEST_F(MQTTAllocatorTest, MemoryStatistics)
{
  // Test memory statistics tracking
  size_t initial_usage = allocator->get_memory_usage();

  // Allocate some memory
  void* ptr = allocator->allocate(1024);
  ASSERT_NE(ptr, nullptr);

  // Check statistics after allocation
  size_t after_usage = allocator->get_memory_usage();
  EXPECT_GT(after_usage, initial_usage);

  // Deallocate memory
  allocator->deallocate(ptr, 1024);

  // Check statistics after deallocation
  size_t final_usage = allocator->get_memory_usage();
  EXPECT_LE(final_usage, after_usage);
}

TEST_F(MQTTAllocatorTest, ChildAllocator)
{
  // Test creating child allocator
  MQTTAllocator* child =
      allocator->create_child("child_test", MQTTMemoryTag::MEM_TAG_TOPIC_TREE, 0);
  ASSERT_NE(child, nullptr);

  // Test allocation with child
  void* ptr = child->allocate(512);
  ASSERT_NE(ptr, nullptr);

  child->deallocate(ptr, 512);

  // Check child can be retrieved
  MQTTAllocator* retrieved_child = allocator->get_child("child_test");
  EXPECT_EQ(retrieved_child, child);

  // Clean up child allocator
  allocator->remove_child("child_test");
  SUCCEED();
}

// Google Test main function
int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}