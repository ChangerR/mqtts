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
  int handle_publish(const PublishPacket* packet);
  int handle_subscribe(const SubscribePacket* packet);
  int handle_pingreq();
  int handle_disconnect();

  // Response senders
  int send_connack(ReasonCode reason_code);
  int send_puback(uint16_t packet_id, ReasonCode reason_code);
  int send_suback(uint16_t packet_id, const std::vector<ReasonCode>& reason_codes);
  int send_pingresp();

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
};

}  // namespace mqtt