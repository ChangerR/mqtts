#include <gtest/gtest.h>
#include <iostream>
#include <iomanip>
#include "mqtt_parser.h"
#include "mqtt_allocator.h"
#include "mqtt_serialize_buffer.h"
#include "mqtt_memory_tags.h"

namespace mqtt {

// Simple test to debug serialization
TEST(DebugSerializationTest, SimpleDebug)
{
  MQTTAllocator allocator("test", MQTTMemoryTag::MEM_TAG_CLIENT);
  MQTTParser parser(&allocator);
  MQTTSerializeBuffer buffer(&allocator);
  
  // Create a simple publish packet
  PublishPacket packet(&allocator);
  packet.type = PacketType::PUBLISH;
  packet.topic_name = to_mqtt_string("test", &allocator);
  packet.qos = 0;
  packet.retain = false;
  packet.dup = false;
  packet.packet_id = 0;
  packet.payload.resize(5);
  std::memcpy(packet.payload.data(), "hello", 5);
  
  // Debug properties
  std::cout << "Properties before serialization:" << std::endl;
  std::cout << "  retain_available: " << packet.properties.retain_available << std::endl;
  std::cout << "  wildcard_subscription_available: " << packet.properties.wildcard_subscription_available << std::endl;
  std::cout << "  subscription_identifier_available: " << packet.properties.subscription_identifier_available << std::endl;
  std::cout << "  shared_subscription_available: " << packet.properties.shared_subscription_available << std::endl;
  
  // Serialize
  int ret = parser.serialize_publish(&packet, buffer);
  ASSERT_EQ(ret, MQ_SUCCESS);
  
  // Print hex dump
  std::cout << "Serialized packet (" << buffer.size() << " bytes):" << std::endl;
  for (size_t i = 0; i < buffer.size(); i++) {
    std::cout << std::hex << std::setfill('0') << std::setw(2) 
              << static_cast<int>(buffer.data()[i]) << " ";
    if ((i + 1) % 16 == 0) std::cout << std::endl;
  }
  std::cout << std::dec << std::endl;
  
  // Manually analyze packet structure
  const uint8_t* data = buffer.data();
  size_t size = buffer.size();
  
  std::cout << "Manual analysis:" << std::endl;
  std::cout << "  Byte 0 (packet type/flags): 0x" << std::hex << static_cast<int>(data[0]) << std::dec << std::endl;
  std::cout << "  Byte 1 (remaining length): " << static_cast<int>(data[1]) << std::endl;
  
  if (size >= 4) {
    uint16_t topic_len = (data[2] << 8) | data[3];
    std::cout << "  Topic length: " << topic_len << std::endl;
    
    if (size >= 4 + topic_len) {
      std::string topic(reinterpret_cast<const char*>(&data[4]), topic_len);
      std::cout << "  Topic: " << topic << std::endl;
      
      size_t props_offset = 4 + topic_len;
      if (size > props_offset) {
        std::cout << "  Properties length byte: " << static_cast<int>(data[props_offset]) << std::endl;
        
        size_t payload_offset = props_offset + 1 + data[props_offset];
        if (size >= payload_offset) {
          std::cout << "  Payload offset: " << payload_offset << std::endl;
          std::cout << "  Payload size: " << (size - payload_offset) << std::endl;
          if (size > payload_offset) {
            std::string payload_str(reinterpret_cast<const char*>(&data[payload_offset]), size - payload_offset);
            std::cout << "  Payload: " << payload_str << std::endl;
          }
        }
      }
    }
  }
}

}  // namespace mqtt

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}