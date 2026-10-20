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
#include "mqtt_auth_interface.h"
#include "mqtt_buffer.h"
#include "mqtt_coroutine_utils.h"
#include "mqtt_define.h"
#include "mqtt_durable_store.h"
#include "mqtt_packet.h"
#include "mqtt_parser.h"
#include "mqtt_runtime.h"
#include "mqtt_config.h"
#include "mqtt_connection_watch.h"
#include "mqtt_flow_control.h"
#include "mqtt_socket.h"
#include "mqtt_takeover.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {

// 前向声明
class GlobalSessionManager;

class MQTTProtocolHandler
{
 public:
  MQTTProtocolHandler(MQTTAllocator* allocator);
  virtual ~MQTTProtocolHandler();

  // Initialize handler with socket
  int init(MQTTSocket* socket, const std::string& client_ip, int client_port);

  // Main processing loop
  int process();

  // Session manager integration
  void set_session_manager(GlobalSessionManager* session_manager)
  {
    session_manager_ = session_manager;
  }
  GlobalSessionManager* get_session_manager() const { return session_manager_; }

  // Authentication manager integration
  void set_auth_manager(auth::AuthManager* auth_manager)
  {
    auth_manager_ = auth_manager;
  }
  auth::AuthManager* get_auth_manager() const { return auth_manager_; }

  // 启动时冻结的只读限额。空指针表示使用 ProtocolLimits 默认值。
  void set_protocol_limits(const ProtocolLimits* limits)
  {
    protocol_limits_ = limits;
    if (limits) {
      allow_mqtt3x_ = limits->allow_mqtt3x;
    }
    sync_server_flow_limits();
  }
  const ProtocolLimits& effective_limits() const;
  bool is_mqtt3x_allowed() const { return allow_mqtt3x_; }

  void set_connection_established(std::chrono::steady_clock::time_point when)
  {
    established_ = when;
    established_set_ = true;
  }

  // 由旧连接所在事件线程的事件循环回调调用。只打标记并唤醒读协程，不在回调里发送。
  virtual void request_session_takeover(const std::shared_ptr<SessionTakeoverGate>& gate);

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

  // Publish message sender
  virtual int send_publish(const MQTTString& topic, const MQTTByteVector& payload, uint8_t qos = 0,
                   bool retain = false, bool dup = false,
                   const Properties& properties = Properties(),
                   std::shared_ptr<auth::AuthorizationRequest>* pending = nullptr);
  virtual int send_publish(const PublishPacket& packet);

  // Session management
  virtual bool is_connected() const { return connected_; }
  void set_client_id(const MQTTString& client_id) { client_id_ = client_id; }
  const MQTTString& get_client_id() const { return client_id_; }
  uint16_t get_next_packet_id();

  // 客户端在 CONNECT 里声明的接收窗口和最大报文。所有会话都记录，不只是持久化会话。
  void observe_client_flow(uint8_t protocol_version, uint16_t receive_maximum,
                          uint32_t maximum_packet_size);
  bool outbound_exceeds_client_maximum(size_t bytes) const;
  static void omit_optional_outbound_properties(Properties& properties);
  uint8_t granted_qos_limit() const;
  ConnectionFlow::Qos2Admit admit_inbound_qos2(uint16_t packet_id);
  void release_inbound_qos2(uint16_t packet_id);
  void release_outbound_packet_id(uint16_t packet_id);
  ConnectionFlow::AckDisposition classify_outbound_ack(uint16_t packet_id,
                                                      ConnectionFlow::AckKind kind) const
  {
    return flow_.classify_outbound_ack(packet_id, kind);
  }
  void mark_outbound_awaiting_pubcomp(uint16_t packet_id)
  {
    flow_.mark_awaiting_pubcomp(packet_id);
  }
  bool outbound_packet_id_inflight(uint16_t packet_id) const
  {
    return flow_.outbound_holds(packet_id);
  }

  // Topic subscription management
  int add_subscription(const MQTTString& topic);
  int remove_subscription(const MQTTString& topic);
  int get_subscriptions(MQTTVector<MQTTString>& subscriptions) const;

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
  uint64_t durable_epoch_ = 0;
  int64_t durable_gap_ = 0;
  bool durable_running_ = false;
  runtime::TaskHandle durable_task_;
  std::shared_ptr<DurableStore::Signal> durable_revision_;
  void pump_durable();
  ConnectionFlow flow_;

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
  uint8_t negotiated_protocol_version_;
  bool version_negotiated_;
  bool allow_mqtt3x_;

  MQTTBuffer* serialize_buffer_;  // 复用的序列化缓冲区

  // 写入锁状态和条件变量，支持超时等待
  mutable std::atomic<bool> write_lock_acquired_;
  mutable CoroCondition write_lock_condition_;

  // Session manager
  GlobalSessionManager* session_manager_;

  // Authentication manager
  auth::AuthManager* auth_manager_;

  // Current authenticated user info
  auth::UserInfo* current_user_info_;
  auth::ClientAuthContext* current_auth_context_;

  // Helper method to unregister from session manager
  void cleanup_session_registration(const char* context = nullptr);

  // Helper method to register with session manager
  int register_session_with_manager();

  // Helper methods for CONNECT auth/reject flow
  int ensure_auth_state();
  int reject_connect_request(uint8_t protocol_version, ReasonCode reason_code, int err_code);

  const ProtocolLimits* protocol_limits_;
  uint16_t client_keep_alive_;
  bool client_requested_session_expiry_;

  ConnectionWatch watch_;
  std::chrono::steady_clock::time_point established_{};
  bool established_set_ = false;
  std::shared_ptr<SessionTakeoverGate> takeover_gate_;
  std::atomic<bool> takeover_pending_{false};
  bool cleanup_finished_ = false;
  bool protocol_fault_noted_ = false;

  void arm_connection_watch();
  int fail_protocol(ReasonCode reason, int err);
  int close_for_flow(ReasonCode reason, int err);
  template <typename Serialize>
  int send_within_client_limit(Packet* packet, Serialize serialize);
  void signal_takeover_done();
  void sync_server_flow_limits();
  uint32_t inbound_packet_limit() const;
  static int claim_outbound_packet_id(uint16_t id, void* arg);
  ConnectionFlow::Allocation allocate_outbound_packet_id(uint8_t qos = 0);

 protected:
  virtual int transmit_serialized(const uint8_t* data, size_t size);
};

}  // namespace mqtt
