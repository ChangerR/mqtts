#ifndef MQTT_PARSER_H
#define MQTT_PARSER_H

#include <cstdint>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_packet.h"
#include "mqtt_serialize_buffer.h"
#include "mqtt_string_utils.h"

namespace mqtt {

class MQTTParser
{
 public:
  explicit MQTTParser(MQTTAllocator* allocator);
  ~MQTTParser();

  // 禁止拷贝构造和赋值
  MQTTParser(const MQTTParser&) = delete;
  MQTTParser& operator=(const MQTTParser&) = delete;

  // Parse a complete MQTT packet from buffer
  int parse_packet(const uint8_t* buffer, size_t length, Packet** packet);

  // Parse specific packet types
  int parse_connect(const uint8_t* buffer, size_t length, ConnectPacket** packet);
  int parse_connack(const uint8_t* buffer, size_t length, ConnAckPacket** packet);
  int parse_publish(const uint8_t* buffer, size_t length, PublishPacket** packet);
  int parse_puback(const uint8_t* buffer, size_t length, PubAckPacket** packet);
  int parse_pubrec(const uint8_t* buffer, size_t length, PubRecPacket** packet);
  int parse_pubrel(const uint8_t* buffer, size_t length, PubRelPacket** packet);
  int parse_pubcomp(const uint8_t* buffer, size_t length, PubCompPacket** packet);
  int parse_subscribe(const uint8_t* buffer, size_t length, SubscribePacket** packet);
  int parse_suback(const uint8_t* buffer, size_t length, SubAckPacket** packet);
  int parse_unsubscribe(const uint8_t* buffer, size_t length, UnsubscribePacket** packet);
  int parse_unsuback(const uint8_t* buffer, size_t length, UnsubAckPacket** packet);
  int parse_pingreq(const uint8_t* buffer, size_t length, PingReqPacket** packet);
  int parse_pingresp(const uint8_t* buffer, size_t length, PingRespPacket** packet);
  int parse_disconnect(const uint8_t* buffer, size_t length, DisconnectPacket** packet);
  int parse_auth(const uint8_t* buffer, size_t length, AuthPacket** packet);

  // Serialize packets to buffer
  int serialize_connect(const ConnectPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_connack(const ConnAckPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_publish(const PublishPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_puback(const PubAckPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_pubrec(const PubRecPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_pubrel(const PubRelPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_pubcomp(const PubCompPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_subscribe(const SubscribePacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_suback(const SubAckPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_unsubscribe(const UnsubscribePacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_unsuback(const UnsubAckPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_pingreq(const PingReqPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_pingresp(const PingRespPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_disconnect(const DisconnectPacket* packet, MQTTSerializeBuffer& buffer);
  int serialize_auth(const AuthPacket* packet, MQTTSerializeBuffer& buffer);

 private:
  // Helper functions for parsing
  int parse_remaining_length(const uint8_t* buffer, size_t length, uint32_t& remaining_length,
                             size_t& bytes_read);
  int parse_string(const uint8_t* buffer, size_t length, std::string& str, size_t& bytes_read);
  int parse_mqtt_string(const uint8_t* buffer, size_t length, MQTTString& str, size_t& bytes_read);
  int parse_binary_data(const uint8_t* buffer, size_t length, std::vector<uint8_t>& data,
                        size_t& bytes_read);
  int parse_mqtt_binary_data(const uint8_t* buffer, size_t length, MQTTByteVector& data,
                             size_t& bytes_read);
  int parse_properties(const uint8_t* buffer, size_t length, Properties& properties,
                       size_t& bytes_read);
  int parse_reason_codes(const uint8_t* buffer, size_t length,
                         std::vector<ReasonCode>& reason_codes, size_t& bytes_read);

  // Helper functions for serialization
  int serialize_remaining_length(uint32_t remaining_length, MQTTSerializeBuffer& buffer);
  int serialize_string(const std::string& str, MQTTSerializeBuffer& buffer);
  int serialize_mqtt_string(const MQTTString& str, MQTTSerializeBuffer& buffer);
  int serialize_binary_data(const std::vector<uint8_t>& data, MQTTSerializeBuffer& buffer);
  int serialize_mqtt_binary_data(const MQTTByteVector& data, MQTTSerializeBuffer& buffer);
  int serialize_properties(const Properties& properties, MQTTSerializeBuffer& buffer);
  int serialize_reason_codes(const std::vector<ReasonCode>& reason_codes,
                             MQTTSerializeBuffer& buffer);

  // Helper function to map string util error codes to parser error codes
  int map_string_util_error(int string_util_error) const;

  MQTTAllocator* allocator_;
};

}  // namespace mqtt

#endif  // MQTT_PARSER_H