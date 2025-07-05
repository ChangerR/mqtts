#pragma once

#include <poll.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_coroutine_utils.h"
#include "mqtt_define.h"
#include "mqtt_packet.h"
#include "mqtt_parser.h"
#include "mqtt_serialize_buffer.h"
#include "mqtt_socket.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {

// 前向声明
class GlobalSessionManager;

class MQTTProtocolHandler
{
 public:
  MQTTProtocolHandler(MQTTAllocator* allocator);
  ~MQTTProtocolHandler();

  // Initialize handler with socket
  void init(MQTTSocket* socket, const std::string& client_ip, int client_port);

  // Main processing loop
  int process();

  // Session manager integration
  void set_session_manager(GlobalSessionManager* session_manager)
  {
    session_manager_ = session_manager;
  }
  GlobalSessionManager* get_session_manager() const { return session_manager_; }

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
  bool is_connected() const { return connected_; }
  void set_client_id(const MQTTString& client_id) { client_id_ = client_id; }
  const MQTTString& get_client_id() const { return client_id_; }
  uint16_t get_next_packet_id() { return next_packet_id_++; }

  // Topic subscription management
  void add_subscription(const MQTTString& topic);
  void remove_subscription(const MQTTString& topic);
  const MQTTVector<MQTTString>& get_subscriptions() const { return subscriptions_; }

 private:
  // Buffer management
  int ensure_buffer_size(size_t needed_size);
  int read_packet();
  int parse_packet();

  // 统一的协程安全写入函数，timeout_ms控制锁等待时间
  int send_data_with_lock(const char* data, size_t size, int timeout_ms = 5000);

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
  MQTTString client_ip_;
  int client_port_;
  MQTTString client_id_;
  bool connected_;
  uint16_t next_packet_id_;

  // Topic subscriptions
  MQTTVector<MQTTString> subscriptions_;

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

  MQTTSerializeBuffer* serialize_buffer_;  // 复用的序列化缓冲区

  // 写入锁状态和条件变量，支持超时等待
  mutable std::atomic<bool> write_lock_acquired_;
  mutable CoroCondition write_lock_condition_;

  // Session manager
  GlobalSessionManager* session_manager_;

  // Helper method to unregister from session manager
  void cleanup_session_registration(const char* context = nullptr);

  // Helper method to register with session manager
  int register_session_with_manager();
};

}  // namespace mqtt