#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mqtt {

// MQTT v5 Packet Types
enum class PacketType : uint8_t {
  CONNECT = 0x10,
  CONNACK = 0x20,
  PUBLISH = 0x30,
  PUBACK = 0x40,
  PUBREC = 0x50,
  PUBREL = 0x60,
  PUBCOMP = 0x70,
  SUBSCRIBE = 0x80,
  SUBACK = 0x90,
  UNSUBSCRIBE = 0xA0,
  UNSUBACK = 0xB0,
  PINGREQ = 0xC0,
  PINGRESP = 0xD0,
  DISCONNECT = 0xE0,
  AUTH = 0xF0
};

// MQTT v5 Connect Flags
struct ConnectFlags
{
  uint8_t clean_start : 1;
  uint8_t will_flag : 1;
  uint8_t will_qos : 2;
  uint8_t will_retain : 1;
  uint8_t password_flag : 1;
  uint8_t username_flag : 1;
  uint8_t reserved : 1;
};

// MQTT v5 Properties
struct Properties
{
  // 会话相关属性
  uint32_t session_expiry_interval = 0;
  uint16_t receive_maximum = 0;
  uint32_t maximum_packet_size = 0;
  uint16_t topic_alias_maximum = 0;
  uint16_t topic_alias = 0;
  bool request_response_information = false;
  bool request_problem_information = false;

  // 用户属性
  std::vector<std::pair<std::string, std::string>> user_properties;

  // 认证相关属性
  std::string authentication_method;
  std::vector<uint8_t> authentication_data;

  // 连接相关属性
  std::string assigned_client_identifier;
  uint16_t server_keep_alive = 0;
  std::string response_information;
  std::string server_reference;
  std::string reason_string;

  // 发布相关属性
  uint8_t payload_format_indicator = 0;
  uint32_t message_expiry_interval = 0;
  std::string content_type;
  std::string response_topic;
  std::vector<uint8_t> correlation_data;
  uint32_t subscription_identifier = 0;
  uint32_t will_delay_interval = 0;

  // 订阅相关属性
  uint8_t maximum_qos = 0;
  bool retain_available = true;
  bool wildcard_subscription_available = true;
  bool subscription_identifier_available = true;
  bool shared_subscription_available = true;
};

// MQTT v5 Reason Codes
enum class ReasonCode : uint8_t {
  Success = 0x00,
  NormalDisconnection = 0x00,
  GrantedQoS0 = 0x00,
  GrantedQoS1 = 0x01,
  GrantedQoS2 = 0x02,
  DisconnectWithWillMessage = 0x04,
  NoMatchingSubscribers = 0x10,
  NoSubscriptionExisted = 0x11,
  ContinueAuthentication = 0x18,
  ReAuthenticate = 0x19,
  UnspecifiedError = 0x80,
  MalformedPacket = 0x81,
  ProtocolError = 0x82,
  ImplementationSpecificError = 0x83,
  UnsupportedProtocolVersion = 0x84,
  ClientIdentifierNotValid = 0x85,
  BadUserNameOrPassword = 0x86,
  NotAuthorized = 0x87,
  ServerUnavailable = 0x88,
  ServerBusy = 0x89,
  Banned = 0x8A,
  ServerShuttingDown = 0x8B,
  BadAuthenticationMethod = 0x8C,
  KeepAliveTimeout = 0x8D,
  SessionTakenOver = 0x8E,
  TopicFilterInvalid = 0x8F,
  TopicNameInvalid = 0x90,
  PacketIdentifierInUse = 0x91,
  PacketIdentifierNotFound = 0x92,
  ReceiveMaximumExceeded = 0x93,
  TopicAliasInvalid = 0x94,
  PacketTooLarge = 0x95,
  MessageRateTooHigh = 0x96,
  QuotaExceeded = 0x97,
  AdministrativeAction = 0x98,
  PayloadFormatInvalid = 0x99,
  RetainNotSupported = 0x9A,
  QoSNotSupported = 0x9B,
  UseAnotherServer = 0x9C,
  ServerMoved = 0x9D,
  SharedSubscriptionsNotSupported = 0x9E,
  ConnectionRateExceeded = 0x9F,
  MaximumConnectTime = 0xA0,
  SubscriptionIdentifiersNotSupported = 0xA1,
  WildcardSubscriptionsNotSupported = 0xA2
};

// MQTT v5 Property Types
enum class PropertyType : uint8_t {
  PayloadFormatIndicator = 0x01,
  MessageExpiryInterval = 0x02,
  ContentType = 0x03,
  ResponseTopic = 0x08,
  CorrelationData = 0x09,
  SubscriptionIdentifier = 0x0B,
  SessionExpiryInterval = 0x11,
  AssignedClientIdentifier = 0x12,
  ServerKeepAlive = 0x13,
  AuthenticationMethod = 0x15,
  AuthenticationData = 0x16,
  RequestProblemInformation = 0x17,
  WillDelayInterval = 0x18,
  RequestResponseInformation = 0x19,
  ResponseInformation = 0x1A,
  ServerReference = 0x1C,
  ReasonString = 0x1F,
  ReceiveMaximum = 0x21,
  TopicAliasMaximum = 0x22,
  TopicAlias = 0x23,
  MaximumQoS = 0x24,
  RetainAvailable = 0x25,
  UserProperty = 0x26,
  MaximumPacketSize = 0x27,
  WildcardSubscriptionAvailable = 0x28,
  SubscriptionIdentifierAvailable = 0x29,
  SharedSubscriptionAvailable = 0x2A
};

// MQTT v5 Packet Base Structure
struct Packet
{
  PacketType type;
  uint32_t remaining_length;
  Properties properties;
  virtual ~Packet() = default;
};

// MQTT v5 Connect Packet
struct ConnectPacket : public Packet
{
  std::string protocol_name;
  uint8_t protocol_version;
  ConnectFlags flags;
  uint16_t keep_alive;
  std::string client_id;
  std::string will_topic;
  std::string will_payload;
  std::string username;
  std::string password;
  Properties will_properties;  // Will消息的属性
};

// MQTT v5 Publish Packet
struct PublishPacket : public Packet
{
  bool dup;
  uint8_t qos;
  bool retain;
  std::string topic_name;
  uint16_t packet_id;
  std::vector<uint8_t> payload;
};

// MQTT v5 Subscribe Packet
struct SubscribePacket : public Packet
{
  uint16_t packet_id;
  std::vector<std::pair<std::string, uint8_t>> subscriptions;
};

// MQTT v5 SubAck Packet
struct SubAckPacket : public Packet
{
  uint16_t packet_id;
  std::vector<ReasonCode> reason_codes;
};

// MQTT v5 Unsubscribe Packet
struct UnsubscribePacket : public Packet
{
  uint16_t packet_id;
  std::vector<std::string> topic_filters;
};

// MQTT v5 UnsubAck Packet
struct UnsubAckPacket : public Packet
{
  uint16_t packet_id;
  std::vector<ReasonCode> reason_codes;
};

// MQTT v5 Disconnect Packet
struct DisconnectPacket : public Packet
{
  ReasonCode reason_code;
};

// MQTT v5 Auth Packet
struct AuthPacket : public Packet
{
  ReasonCode reason_code;
};

// MQTT v5 ConnAck Packet
struct ConnAckPacket : public Packet
{
  bool session_present;
  ReasonCode reason_code;
};

// MQTT v5 PubAck Packet
struct PubAckPacket : public Packet
{
  uint16_t packet_id;
  ReasonCode reason_code;
};

// MQTT v5 PubRec Packet
struct PubRecPacket : public Packet
{
  uint16_t packet_id;
  ReasonCode reason_code;
};

// MQTT v5 PubRel Packet
struct PubRelPacket : public Packet
{
  uint16_t packet_id;
  ReasonCode reason_code;
};

// MQTT v5 PubComp Packet
struct PubCompPacket : public Packet
{
  uint16_t packet_id;
  ReasonCode reason_code;
};

// MQTT v5 PingReq Packet
struct PingReqPacket : public Packet
{
};

// MQTT v5 PingResp Packet
struct PingRespPacket : public Packet
{
};

}  // namespace mqtt