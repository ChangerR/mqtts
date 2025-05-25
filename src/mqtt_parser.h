#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_packet.h"

namespace mqtt {

class MQTTParser
{
 public:
  MQTTParser(MQTTAllocator* allocator);
  ~MQTTParser();

  // Parse a complete MQTT packet from buffer
  int parse_packet(const uint8_t* buffer, size_t length, Packet** packet);

  // Parse specific packet types
  int parse_connect(const uint8_t* buffer, size_t length, ConnectPacket** packet);
  int parse_publish(const uint8_t* buffer, size_t length, PublishPacket** packet);
  int parse_subscribe(const uint8_t* buffer, size_t length, SubscribePacket** packet);

  // Serialize packets to buffer
  int serialize_connect(const ConnectPacket* packet, std::vector<uint8_t>& buffer);
  int serialize_publish(const PublishPacket* packet, std::vector<uint8_t>& buffer);
  int serialize_subscribe(const SubscribePacket* packet, std::vector<uint8_t>& buffer);

 private:
  // Helper functions for parsing
  int parse_remaining_length(const uint8_t* buffer, size_t length, uint32_t& remaining_length,
                             size_t& bytes_read);
  int parse_string(const uint8_t* buffer, size_t length, std::string& str, size_t& bytes_read);
  int parse_binary_data(const uint8_t* buffer, size_t length, std::vector<uint8_t>& data,
                        size_t& bytes_read);
  int parse_properties(const uint8_t* buffer, size_t length,
                       std::vector<std::pair<std::string, std::string>>& properties,
                       size_t& bytes_read);

  // Helper functions for serialization
  int serialize_remaining_length(uint32_t remaining_length, std::vector<uint8_t>& buffer);
  int serialize_string(const std::string& str, std::vector<uint8_t>& buffer);
  int serialize_binary_data(const std::vector<uint8_t>& data, std::vector<uint8_t>& buffer);
  int serialize_properties(const std::vector<std::pair<std::string, std::string>>& properties,
                           std::vector<uint8_t>& buffer);

  MQTTAllocator* allocator_;
};

}  // namespace mqtt