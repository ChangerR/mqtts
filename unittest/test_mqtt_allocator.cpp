#include <gtest/gtest.h>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_packet.h"
#include "mqtt_stl_allocator.h"
#include "mqtt_server.h"

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

TEST_F(MQTTAllocatorTest, AllocatedPacketNestedPublishReturnsToBaseline)
{
  const size_t baseline = allocator->get_memory_usage();

  {
    mqtt::AllocatedPacket<mqtt::PublishPacket> packet(allocator.get());
    ASSERT_TRUE(static_cast<bool>(packet));
    EXPECT_GT(allocator->get_memory_usage(), baseline);

    const std::string topic = "sensors/temperature/living-room/zone-alpha";
    const std::string payload(256, 'P');
    packet->topic_name =
        mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(allocator.get()));
    packet->payload = mqtt::MQTTByteVector(payload.begin(), payload.end(),
                                           mqtt::MQTTSTLAllocator<uint8_t>(allocator.get()));
    packet->qos = 1;
    packet->packet_id = 42;

    EXPECT_GT(allocator->get_memory_usage(), baseline + sizeof(mqtt::PublishPacket));
  }

  EXPECT_EQ(allocator->get_memory_usage(), baseline);
}

TEST(ClientContextTest, DestroyRemovesRegistrationFromOwningAllocator)
{
  MQTTAllocator parent("context_owner", MQTTMemoryTag::MEM_TAG_ROOT);
  const std::vector<std::string> ids = {"short", "client_127.0.0.1_42001"};
  for (const auto& id : ids) {
    MQTTAllocator* child = parent.create_child(id, MQTTMemoryTag::MEM_TAG_CLIENT);
    ASSERT_NE(child, nullptr);
    void* storage = child->allocate(sizeof(ClientContext));
    ASSERT_NE(storage, nullptr);
    ClientContext* ctx = new (storage) ClientContext();
    ctx->allocator = child;
    ctx->client_id = id;
    ctx->trace_id = "context-lifecycle-unit-test";

    ClientContext::destroy(ctx);

    EXPECT_EQ(parent.get_child(id), nullptr);
    EXPECT_EQ(parent.get_total_memory_usage(), 0u);
    ASSERT_NE(parent.create_child(id, MQTTMemoryTag::MEM_TAG_CLIENT), nullptr);
    parent.remove_child(id);
  }
}

// Google Test main function
int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
