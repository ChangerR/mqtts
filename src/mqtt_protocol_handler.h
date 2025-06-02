#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_packet.h"
#include "mqtt_parser.h"
#include "mqtt_socket.h"
namespace mqtt {

class MQTTProtocolHandler
{
 public:
  MQTTProtocolHandler(MQTTAllocator* allocator);
  ~MQTTProtocolHandler();

  // Initialize handler with socket
  void init(MQTTSocket* socket, const std::string& client_ip, int client_port);

  // Main processing loop
  int process();

  // Packet handlers
  int handle_connect(const ConnectPacket* packet);
  int handle_connack(const ConnAckPacket* packet);
  int handle_publish(const PublishPacket* packet);
  int handle_puback(const PubAckPacket* packet);
  int handle_pubrec(const PubRecPacket* packet);
  int handle_pubrel(const PubRelPacket* packet);
  int handle_pubcomp(const PubCompPacket* packet);
  int handle_subscribe(const SubscribePacket* packet);
  int handle_suback(const SubAckPacket* packet);
  int handle_unsubscribe(const UnsubscribePacket* packet);
  int handle_unsuback(const UnsubAckPacket* packet);
  int handle_pingreq();
  int handle_pingresp();
  int handle_disconnect(const DisconnectPacket* packet);
  int handle_auth(const AuthPacket* packet);

  // Response senders
  int send_connack(ReasonCode reason_code, bool session_present = false);
  int send_puback(uint16_t packet_id, ReasonCode reason_code);
  int send_pubrec(uint16_t packet_id, ReasonCode reason_code);
  int send_pubrel(uint16_t packet_id, ReasonCode reason_code);
  int send_pubcomp(uint16_t packet_id, ReasonCode reason_code);
  int send_suback(uint16_t packet_id, const std::vector<ReasonCode>& reason_codes);
  int send_unsuback(uint16_t packet_id, const std::vector<ReasonCode>& reason_codes);
  int send_pingresp();
  int send_disconnect(ReasonCode reason_code);
  int send_auth(ReasonCode reason_code);

  // Session management
  void set_client_id(const std::string& client_id) { client_id_ = client_id; }
  const std::string& get_client_id() const { return client_id_; }
  bool is_connected() const { return connected_; }
  void set_connected(bool connected) { connected_ = connected; }

  // Topic subscription management
  void add_subscription(const std::string& topic);
  void remove_subscription(const std::string& topic);
  const std::vector<std::string>& get_subscriptions() const { return subscriptions_; }

 private:
  // Buffer management
  int ensure_buffer_size(size_t needed_size);
  int read_packet();
  int parse_packet();

  // Packet reading state
  enum class ReadState {
    READ_HEADER,     // Reading packet type
    READ_REMAINING,  // Reading remaining length
    READ_PAYLOAD     // Reading packet payload
  };

  // Buffer management

  char* buffer_;
  size_t current_buffer_size_;
  size_t bytes_read_;
  size_t bytes_needed_;

  // Packet reading state
  ReadState state_;
  uint8_t packet_type_;
  uint32_t remaining_length_;
  size_t header_size_;  // header + remaining length字段的总大小

  // Session state
  MQTTSocket* socket_;
  std::string client_ip_;
  int client_port_;
  std::string client_id_;
  bool connected_;
  uint16_t next_packet_id_;

  // Topic subscriptions
  std::vector<std::string> subscriptions_;

  // Memory management
  MQTTAllocator* allocator_;
  MQTTParser* parser_;

  // MQTT 5.0 session state
  Properties session_properties_;
  uint32_t session_expiry_interval_;
  uint16_t receive_maximum_;
  uint32_t maximum_packet_size_;
  uint16_t topic_alias_maximum_;
  bool request_response_information_;
  bool request_problem_information_;
};

}  // namespace mqtt