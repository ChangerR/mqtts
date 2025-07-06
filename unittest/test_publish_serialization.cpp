#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "mqtt_parser.h"
#include "mqtt_allocator.h"
#include "mqtt_serialize_buffer.h"
#include "mqtt_memory_tags.h"

namespace mqtt {

class PublishSerializationTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    allocator_ = new MQTTAllocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT);
    ASSERT_NE(allocator_, nullptr);
    
    parser_ = new MQTTParser(allocator_);
    ASSERT_NE(parser_, nullptr);
    
    buffer_ = new MQTTSerializeBuffer(allocator_);
    ASSERT_NE(buffer_, nullptr);
  }

  void TearDown() override
  {
    if (buffer_) {
      delete buffer_;
      buffer_ = nullptr;
    }
    if (parser_) {
      delete parser_;
      parser_ = nullptr;
    }
    if (allocator_) {
      delete allocator_;
      allocator_ = nullptr;
    }
  }

  // Helper function to create a PublishPacket
  PublishPacket* create_publish_packet(const std::string& topic, const std::string& payload,
                                       uint8_t qos = 0, bool retain = false, bool dup = false,
                                       uint16_t packet_id = 0)
  {
    PublishPacket* packet = new (allocator_->allocate(sizeof(PublishPacket))) PublishPacket(allocator_);
    EXPECT_NE(packet, nullptr);
    
    packet->type = PacketType::PUBLISH;
    packet->topic_name = to_mqtt_string(topic, allocator_);
    packet->qos = qos;
    packet->retain = retain;
    packet->dup = dup;
    packet->packet_id = packet_id;
    
    // Convert payload string to byte vector
    packet->payload.resize(payload.size());
    std::memcpy(packet->payload.data(), payload.c_str(), payload.size());
    
    return packet;
  }

  // Helper function to compare PublishPackets
  void compare_publish_packets(const PublishPacket* original, const PublishPacket* parsed)
  {
    ASSERT_NE(original, nullptr);
    ASSERT_NE(parsed, nullptr);
    
    EXPECT_EQ(original->type, parsed->type);
    EXPECT_EQ(from_mqtt_string(original->topic_name), from_mqtt_string(parsed->topic_name));
    EXPECT_EQ(original->qos, parsed->qos);
    EXPECT_EQ(original->retain, parsed->retain);
    EXPECT_EQ(original->dup, parsed->dup);
    
    if (original->qos > 0) {
      EXPECT_EQ(original->packet_id, parsed->packet_id);
    }
    
    EXPECT_EQ(original->payload.size(), parsed->payload.size());
    if (original->payload.size() > 0) {
      EXPECT_EQ(0, std::memcmp(original->payload.data(), parsed->payload.data(), original->payload.size()));
    }
  }

  // Helper function to cleanup PublishPacket
  void cleanup_publish_packet(PublishPacket* packet)
  {
    if (packet) {
      allocator_->deallocate(packet, sizeof(PublishPacket));
    }
  }

 protected:
  MQTTAllocator* allocator_;
  MQTTParser* parser_;
  MQTTSerializeBuffer* buffer_;
};

// Test basic PUBLISH packet serialization and parsing (QoS 0)
TEST_F(PublishSerializationTest, BasicPublishQoS0)
{
  // Create original packet
  std::string topic = "sensor/temperature";
  std::string payload = "23.5C";
  PublishPacket* original = create_publish_packet(topic, payload, 0, false, false, 0);

  // Serialize packet
  int ret = parser_->serialize_publish(original, *buffer_);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to serialize PUBLISH packet";
  ASSERT_GT(buffer_->size(), 0) << "Serialized buffer is empty";

  // Parse the serialized data back
  Packet* generic_packet = nullptr;
  ret = parser_->parse_packet(buffer_->data(), buffer_->size(), &generic_packet);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to parse serialized PUBLISH packet";
  ASSERT_NE(generic_packet, nullptr);

  PublishPacket* parsed = static_cast<PublishPacket*>(generic_packet);
  ASSERT_EQ(parsed->type, PacketType::PUBLISH);

  // Compare packets
  compare_publish_packets(original, parsed);

  // Cleanup
  cleanup_publish_packet(original);
  cleanup_publish_packet(parsed);
}

// Test PUBLISH packet with QoS 1
TEST_F(PublishSerializationTest, PublishQoS1WithPacketId)
{
  // Create original packet with QoS 1
  std::string topic = "device/status";
  std::string payload = "online";
  uint16_t packet_id = 1234;
  PublishPacket* original = create_publish_packet(topic, payload, 1, false, false, packet_id);

  // Serialize packet
  int ret = parser_->serialize_publish(original, *buffer_);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to serialize QoS 1 PUBLISH packet";

  // Parse the serialized data back
  Packet* generic_packet = nullptr;
  ret = parser_->parse_packet(buffer_->data(), buffer_->size(), &generic_packet);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to parse QoS 1 PUBLISH packet";
  ASSERT_NE(generic_packet, nullptr);

  PublishPacket* parsed = static_cast<PublishPacket*>(generic_packet);
  ASSERT_EQ(parsed->type, PacketType::PUBLISH);

  // Compare packets
  compare_publish_packets(original, parsed);

  // Cleanup
  cleanup_publish_packet(original);
  cleanup_publish_packet(parsed);
}

// Test PUBLISH packet with retain flag
TEST_F(PublishSerializationTest, PublishWithRetainFlag)
{
  // Create original packet with retain flag
  std::string topic = "config/setting";
  std::string payload = "enabled";
  PublishPacket* original = create_publish_packet(topic, payload, 0, true, false, 0);

  // Serialize packet
  int ret = parser_->serialize_publish(original, *buffer_);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to serialize PUBLISH packet with retain flag";

  // Parse the serialized data back
  Packet* generic_packet = nullptr;
  ret = parser_->parse_packet(buffer_->data(), buffer_->size(), &generic_packet);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to parse PUBLISH packet with retain flag";
  ASSERT_NE(generic_packet, nullptr);

  PublishPacket* parsed = static_cast<PublishPacket*>(generic_packet);
  ASSERT_EQ(parsed->type, PacketType::PUBLISH);

  // Compare packets
  compare_publish_packets(original, parsed);

  // Cleanup
  cleanup_publish_packet(original);
  cleanup_publish_packet(parsed);
}

// Test PUBLISH packet with dup flag
TEST_F(PublishSerializationTest, PublishWithDupFlag)
{
  // Create original packet with dup flag
  std::string topic = "error/log";
  std::string payload = "Critical error occurred";
  uint16_t packet_id = 5678;
  PublishPacket* original = create_publish_packet(topic, payload, 1, false, true, packet_id);

  // Serialize packet
  int ret = parser_->serialize_publish(original, *buffer_);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to serialize PUBLISH packet with dup flag";

  // Parse the serialized data back
  Packet* generic_packet = nullptr;
  ret = parser_->parse_packet(buffer_->data(), buffer_->size(), &generic_packet);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to parse PUBLISH packet with dup flag";
  ASSERT_NE(generic_packet, nullptr);

  PublishPacket* parsed = static_cast<PublishPacket*>(generic_packet);
  ASSERT_EQ(parsed->type, PacketType::PUBLISH);

  // Compare packets
  compare_publish_packets(original, parsed);

  // Cleanup
  cleanup_publish_packet(original);
  cleanup_publish_packet(parsed);
}

// Test PUBLISH packet with empty payload
TEST_F(PublishSerializationTest, PublishWithEmptyPayload)
{
  // Create original packet with empty payload
  std::string topic = "heartbeat";
  std::string payload = "";  // Empty payload
  PublishPacket* original = create_publish_packet(topic, payload, 0, false, false, 0);

  // Serialize packet
  int ret = parser_->serialize_publish(original, *buffer_);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to serialize PUBLISH packet with empty payload";

  // Parse the serialized data back
  Packet* generic_packet = nullptr;
  ret = parser_->parse_packet(buffer_->data(), buffer_->size(), &generic_packet);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to parse PUBLISH packet with empty payload";
  ASSERT_NE(generic_packet, nullptr);

  PublishPacket* parsed = static_cast<PublishPacket*>(generic_packet);
  ASSERT_EQ(parsed->type, PacketType::PUBLISH);

  // Compare packets
  compare_publish_packets(original, parsed);

  // Cleanup
  cleanup_publish_packet(original);
  cleanup_publish_packet(parsed);
}

// Test PUBLISH packet with QoS 2
TEST_F(PublishSerializationTest, PublishQoS2)
{
  // Create original packet with QoS 2
  std::string topic = "important/data";
  std::string payload = "Critical message that must be delivered exactly once";
  uint16_t packet_id = 9999;
  PublishPacket* original = create_publish_packet(topic, payload, 2, false, false, packet_id);

  // Serialize packet
  int ret = parser_->serialize_publish(original, *buffer_);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to serialize QoS 2 PUBLISH packet";

  // Parse the serialized data back
  Packet* generic_packet = nullptr;
  ret = parser_->parse_packet(buffer_->data(), buffer_->size(), &generic_packet);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to parse QoS 2 PUBLISH packet";
  ASSERT_NE(generic_packet, nullptr);

  PublishPacket* parsed = static_cast<PublishPacket*>(generic_packet);
  ASSERT_EQ(parsed->type, PacketType::PUBLISH);

  // Compare packets
  compare_publish_packets(original, parsed);

  // Cleanup
  cleanup_publish_packet(original);
  cleanup_publish_packet(parsed);
}

// Test PUBLISH packet with long topic and payload
TEST_F(PublishSerializationTest, PublishWithLongContent)
{
  // Create original packet with long topic and payload
  std::string topic = "very/long/topic/path/with/many/levels/to/test/serialization/limits";
  std::string payload;
  for (int i = 0; i < 1000; i++) {
    payload += "This is a long payload to test serialization of large messages. ";
  }
  
  PublishPacket* original = create_publish_packet(topic, payload, 1, true, false, 12345);

  // Serialize packet
  int ret = parser_->serialize_publish(original, *buffer_);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to serialize PUBLISH packet with long content";

  // Parse the serialized data back
  Packet* generic_packet = nullptr;
  ret = parser_->parse_packet(buffer_->data(), buffer_->size(), &generic_packet);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to parse PUBLISH packet with long content";
  ASSERT_NE(generic_packet, nullptr);

  PublishPacket* parsed = static_cast<PublishPacket*>(generic_packet);
  ASSERT_EQ(parsed->type, PacketType::PUBLISH);

  // Compare packets
  compare_publish_packets(original, parsed);

  // Cleanup
  cleanup_publish_packet(original);
  cleanup_publish_packet(parsed);
}

// Test binary payload
TEST_F(PublishSerializationTest, PublishWithBinaryPayload)
{
  // Create original packet with binary payload
  std::string topic = "binary/data";
  
  // Create binary payload with various byte values
  PublishPacket* original = create_publish_packet(topic, "", 0, false, false, 0);
  original->payload.resize(256);
  for (int i = 0; i < 256; i++) {
    original->payload[i] = static_cast<uint8_t>(i);
  }

  // Serialize packet
  int ret = parser_->serialize_publish(original, *buffer_);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to serialize PUBLISH packet with binary payload";

  // Parse the serialized data back
  Packet* generic_packet = nullptr;
  ret = parser_->parse_packet(buffer_->data(), buffer_->size(), &generic_packet);
  ASSERT_EQ(ret, MQ_SUCCESS) << "Failed to parse PUBLISH packet with binary payload";
  ASSERT_NE(generic_packet, nullptr);

  PublishPacket* parsed = static_cast<PublishPacket*>(generic_packet);
  ASSERT_EQ(parsed->type, PacketType::PUBLISH);

  // Compare packets
  compare_publish_packets(original, parsed);

  // Cleanup
  cleanup_publish_packet(original);
  cleanup_publish_packet(parsed);
}

// Test manual packet format verification 
TEST_F(PublishSerializationTest, ManualPacketFormatVerification)
{
  // Create a simple PUBLISH packet
  std::string topic = "test/topic";
  std::string payload = "hello";
  PublishPacket* original = create_publish_packet(topic, payload, 0, false, false, 0);

  // Serialize packet
  int ret = parser_->serialize_publish(original, *buffer_);
  ASSERT_EQ(ret, MQ_SUCCESS);

  // Manually verify packet format
  const uint8_t* data = buffer_->data();
  size_t size = buffer_->size();
  
  ASSERT_GE(size, 4) << "Packet too small";
  
  // Check packet type (first byte should be 0x30 for PUBLISH QoS 0)
  EXPECT_EQ(data[0], 0x30) << "Incorrect packet type/flags";
  
  // Check remaining length (second byte for small packets)
  size_t expected_remaining_length = 2 + topic.length() + 1 + payload.length(); // topic_len(2) + topic + props_len(1) + payload
  EXPECT_EQ(data[1], expected_remaining_length) << "Incorrect remaining length";
  
  // Check topic length
  uint16_t topic_len = (data[2] << 8) | data[3];
  EXPECT_EQ(topic_len, topic.length()) << "Incorrect topic length";
  
  // Check topic content
  std::string parsed_topic(reinterpret_cast<const char*>(&data[4]), topic_len);
  EXPECT_EQ(parsed_topic, topic) << "Incorrect topic content";
  
  // Check properties length (should be 0 for simple packet)
  size_t props_offset = 4 + topic_len;
  EXPECT_EQ(data[props_offset], 0) << "Properties length should be 0";
  
  // Check payload content
  size_t payload_offset = props_offset + 1;
  std::string parsed_payload(reinterpret_cast<const char*>(&data[payload_offset]), payload.length());
  EXPECT_EQ(parsed_payload, payload) << "Incorrect payload content";

  // Cleanup
  cleanup_publish_packet(original);
}

}  // namespace mqtt

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}