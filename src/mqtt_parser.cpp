#include "mqtt_parser.h"
#include <cstring>
#include "logger.h"

namespace mqtt {

MQTTParser::MQTTParser(MQTTAllocator* allocator) : allocator_(allocator) {}

MQTTParser::~MQTTParser() {}

int MQTTParser::parse_packet(const uint8_t* buffer, size_t length, Packet** packet)
{
  if (length < 2) {
    LOG_ERROR("Packet too short");
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  PacketType type = static_cast<PacketType>(buffer[0] & 0xF0);
  uint32_t remaining_length;
  size_t bytes_read;

  int ret = parse_remaining_length(buffer + 1, length - 1, remaining_length, bytes_read);
  if (ret != 0) {
    LOG_ERROR("Failed to parse remaining length");
    return MQ_ERR_PACKET_INVALID;
  }

  if (length < 1 + bytes_read + remaining_length) {
    LOG_ERROR("Packet incomplete");
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  switch (type) {
    case PacketType::CONNECT:
      return parse_connect(buffer, length, reinterpret_cast<ConnectPacket**>(packet));
    case PacketType::PUBLISH:
      return parse_publish(buffer, length, reinterpret_cast<PublishPacket**>(packet));
    case PacketType::SUBSCRIBE:
      return parse_subscribe(buffer, length, reinterpret_cast<SubscribePacket**>(packet));
    case PacketType::CONNACK:
      return parse_connack(buffer, length, reinterpret_cast<ConnAckPacket**>(packet));
    case PacketType::PUBACK:
      return parse_puback(buffer, length, reinterpret_cast<PubAckPacket**>(packet));
    case PacketType::PUBREC:
      return parse_pubrec(buffer, length, reinterpret_cast<PubRecPacket**>(packet));
    case PacketType::PUBREL:
      return parse_pubrel(buffer, length, reinterpret_cast<PubRelPacket**>(packet));
    case PacketType::PUBCOMP:
      return parse_pubcomp(buffer, length, reinterpret_cast<PubCompPacket**>(packet));
    case PacketType::SUBACK:
      return parse_suback(buffer, length, reinterpret_cast<SubAckPacket**>(packet));
    case PacketType::UNSUBACK:
      return parse_unsuback(buffer, length, reinterpret_cast<UnsubAckPacket**>(packet));
    case PacketType::PINGREQ:
      return parse_pingreq(buffer, length, reinterpret_cast<PingReqPacket**>(packet));
    case PacketType::PINGRESP:
      return parse_pingresp(buffer, length, reinterpret_cast<PingRespPacket**>(packet));
    case PacketType::DISCONNECT:
      return parse_disconnect(buffer, length, reinterpret_cast<DisconnectPacket**>(packet));
    case PacketType::AUTH:
      return parse_auth(buffer, length, reinterpret_cast<AuthPacket**>(packet));
    default:
      LOG_ERROR("Unsupported packet type: 0x{:02x}", static_cast<uint8_t>(type));
      return MQ_ERR_PACKET_TYPE;
  }
}

int MQTTParser::parse_connect(const uint8_t* buffer, size_t length, ConnectPacket** packet)
{
  if (length < 10) {  // Minimum CONNECT packet size
    LOG_ERROR("CONNECT packet too short");
    return MQ_ERR_PACKET_INCOMPLETE;
  }
  LOG_HEXDUMP(buffer, length);

  ConnectPacket* connect =
      new (allocator_->allocate(sizeof(ConnectPacket))) ConnectPacket(allocator_);
  if (!connect) {
    LOG_ERROR("Failed to allocate CONNECT packet");
    return MQ_ERR_MEMORY_ALLOC;
  }

  size_t pos = 0;
  connect->type = PacketType::CONNECT;

  // 跳过header (1字节)
  pos++;

  // 跳过remaining length字段
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret =
      parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    LOG_ERROR("Failed to parse remaining length");
    connect->~ConnectPacket();
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // Parse protocol name
  ret = parse_mqtt_string(buffer + pos, length - pos, connect->protocol_name, pos);
  if (ret != 0) {
    LOG_ERROR("Failed to parse protocol name");
    connect->~ConnectPacket();
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return MQ_ERR_PACKET_INVALID;
  }

  // Parse protocol version
  if (pos + 1 > length) {
    LOG_ERROR("Packet too short for protocol version");
    connect->~ConnectPacket();
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }
  connect->protocol_version = buffer[pos++];

  // Parse connect flags
  if (pos + 1 > length) {
    LOG_ERROR("Packet too short for connect flags");
    connect->~ConnectPacket();
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }
  uint8_t flags = buffer[pos++];
  connect->flags.clean_start = (flags >> 1) & 0x01;
  connect->flags.will_flag = (flags >> 2) & 0x01;
  connect->flags.will_qos = (flags >> 3) & 0x03;
  connect->flags.will_retain = (flags >> 5) & 0x01;
  connect->flags.password_flag = (flags >> 6) & 0x01;
  connect->flags.username_flag = (flags >> 7) & 0x01;

  // Parse keep alive
  if (pos + 2 > length) {
    LOG_ERROR("Packet too short for keep alive");
    connect->~ConnectPacket();
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }
  connect->keep_alive = (buffer[pos] << 8) | buffer[pos + 1];
  pos += 2;

  // Parse properties
  size_t properties_bytes_read = 0;
  ret = parse_properties(buffer + pos, length - pos, connect->properties, properties_bytes_read);
  if (ret != 0) {
    LOG_ERROR("Failed to parse properties");
    connect->~ConnectPacket();
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += properties_bytes_read;

  // Parse client ID
  ret = parse_mqtt_string(buffer + pos, length - pos, connect->client_id, pos);
  if (ret != 0) {
    LOG_ERROR("Failed to parse client ID");
    connect->~ConnectPacket();
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return MQ_ERR_CONNECT_CLIENT_ID;
  }

  // Parse will topic and payload if will flag is set
  if (connect->flags.will_flag) {
    ret = parse_mqtt_string(buffer + pos, length - pos, connect->will_topic, pos);
    if (ret != 0) {
      LOG_ERROR("Failed to parse will topic");
      connect->~ConnectPacket();
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return MQ_ERR_PACKET_INVALID;
    }

    ret = parse_mqtt_string(buffer + pos, length - pos, connect->will_payload, pos);
    if (ret != 0) {
      LOG_ERROR("Failed to parse will payload");
      connect->~ConnectPacket();
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return MQ_ERR_PACKET_INVALID;
    }
  }

  // Parse username if username flag is set
  if (connect->flags.username_flag) {
    ret = parse_mqtt_string(buffer + pos, length - pos, connect->username, pos);
    if (ret != 0) {
      LOG_ERROR("Failed to parse username");
      connect->~ConnectPacket();
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return MQ_ERR_PACKET_INVALID;
    }
  }

  // Parse password if password flag is set
  if (connect->flags.password_flag) {
    ret = parse_mqtt_string(buffer + pos, length - pos, connect->password, pos);
    if (ret != 0) {
      LOG_ERROR("Failed to parse password");
      connect->~ConnectPacket();
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return MQ_ERR_PACKET_INVALID;
    }
  }

  *packet = connect;
  return MQ_SUCCESS;
}

int MQTTParser::parse_publish(const uint8_t* buffer, size_t length, PublishPacket** packet)
{
  if (length < 2) {
    LOG_ERROR("PUBLISH packet too short");
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  PublishPacket* publish =
      new (allocator_->allocate(sizeof(PublishPacket))) PublishPacket(allocator_);
  if (!publish) {
    LOG_ERROR("Failed to allocate PUBLISH packet");
    return MQ_ERR_MEMORY_ALLOC;
  }

  size_t pos = 0;
  publish->type = PacketType::PUBLISH;

  // Parse flags
  publish->dup = (buffer[0] >> 3) & 0x01;
  publish->qos = (buffer[0] >> 1) & 0x03;
  publish->retain = buffer[0] & 0x01;
  pos++;

  // Parse remaining length
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret =
      parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    LOG_ERROR("Failed to parse remaining length");
    publish->~PublishPacket();
    allocator_->deallocate(publish, sizeof(PublishPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // 记录header + remaining length的总大小
  size_t header_size = pos;
  size_t payload_end = header_size + remaining_length;

  // Parse topic name
  ret = parse_mqtt_string(buffer + pos, length - pos, publish->topic_name, pos);
  if (ret != 0) {
    LOG_ERROR("Failed to parse topic name");
    publish->~PublishPacket();
    allocator_->deallocate(publish, sizeof(PublishPacket));
    return MQ_ERR_PUBLISH_TOPIC;
  }

  // Parse packet ID if QoS > 0
  if (publish->qos > 0) {
    if (pos + 2 > length) {
      LOG_ERROR("Packet too short for packet ID");
      publish->~PublishPacket();
      allocator_->deallocate(publish, sizeof(PublishPacket));
      return MQ_ERR_PACKET_INCOMPLETE;
    }
    publish->packet_id = (buffer[pos] << 8) | buffer[pos + 1];
    pos += 2;
  }

  // Parse properties
  size_t properties_bytes_read = 0;
  ret = parse_properties(buffer + pos, length - pos, publish->properties, properties_bytes_read);
  if (ret != 0) {
    LOG_ERROR("Failed to parse properties");
    publish->~PublishPacket();
    allocator_->deallocate(publish, sizeof(PublishPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += properties_bytes_read;

  // Parse payload
  size_t payload_length = remaining_length - (pos - header_size);  // Subtract header length
  parse_mqtt_binary_data(buffer + pos, payload_length, publish->payload, pos);
  pos = header_size + remaining_length;  // 直接设置到包的末尾

  *packet = publish;
  return MQ_SUCCESS;
}

int MQTTParser::parse_subscribe(const uint8_t* buffer, size_t length, SubscribePacket** packet)
{
  if (length < 5) {  // Minimum SUBSCRIBE packet size
    LOG_ERROR("SUBSCRIBE packet too short");
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  SubscribePacket* subscribe =
      new (allocator_->allocate(sizeof(SubscribePacket))) SubscribePacket(allocator_);
  if (!subscribe) {
    LOG_ERROR("Failed to allocate SUBSCRIBE packet");
    return MQ_ERR_MEMORY_ALLOC;
  }

  size_t pos = 0;
  subscribe->type = PacketType::SUBSCRIBE;

  // Skip packet type
  pos++;

  // Parse remaining length
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret =
      parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    LOG_ERROR("Failed to parse remaining length");
    subscribe->~SubscribePacket();
    allocator_->deallocate(subscribe, sizeof(SubscribePacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // 记录header + remaining length的总大小
  size_t header_size = pos;
  size_t payload_end = header_size + remaining_length;

  // Parse packet ID
  if (pos + 2 > length) {
    LOG_ERROR("Packet too short for packet ID");
    subscribe->~SubscribePacket();
    allocator_->deallocate(subscribe, sizeof(SubscribePacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }
  subscribe->packet_id = (buffer[pos] << 8) | buffer[pos + 1];
  pos += 2;

  // Parse properties
  size_t properties_bytes_read = 0;
  ret = parse_properties(buffer + pos, length - pos, subscribe->properties, properties_bytes_read);
  if (ret != 0) {
    LOG_ERROR("Failed to parse properties");
    subscribe->~SubscribePacket();
    allocator_->deallocate(subscribe, sizeof(SubscribePacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += properties_bytes_read;

  // Parse subscriptions
  while (pos < payload_end) {
    std::string topic_filter;
    ret = parse_string(buffer + pos, payload_end - pos, topic_filter, pos);
    if (ret != 0) {
      LOG_ERROR("Failed to parse topic filter");
      subscribe->~SubscribePacket();
      allocator_->deallocate(subscribe, sizeof(SubscribePacket));
      return MQ_ERR_SUBSCRIBE_TOPIC;
    }

    if (pos + 1 > payload_end) {
      LOG_ERROR("Packet too short for QoS");
      subscribe->~SubscribePacket();
      allocator_->deallocate(subscribe, sizeof(SubscribePacket));
      return MQ_ERR_PACKET_INCOMPLETE;
    }
    uint8_t qos = buffer[pos++] & 0x03;
    
    // 转换为MQTTString并添加到subscriptions
    MQTTString mqtt_topic = to_mqtt_string(topic_filter, allocator_);
    subscribe->subscriptions.push_back(std::make_pair(mqtt_topic, qos));
  }

  *packet = subscribe;
  return MQ_SUCCESS;
}

int MQTTParser::parse_connack(const uint8_t* buffer, size_t length, ConnAckPacket** packet)
{
  if (length < 2) {
    return MQ_ERR_PACKET_INVALID;
  }

  ConnAckPacket* connack = new (allocator_->allocate(sizeof(ConnAckPacket))) ConnAckPacket();
  connack->type = PacketType::CONNACK;

  size_t bytes_read = 0;

  // 解析会话存在标志和原因码
  connack->session_present = (buffer[bytes_read] & 0x01) != 0;
  bytes_read++;

  connack->reason_code = static_cast<ReasonCode>(buffer[bytes_read]);
  bytes_read++;

  // 解析属性
  if (bytes_read < length) {
    int ret =
        parse_properties(buffer + bytes_read, length - bytes_read, connack->properties, bytes_read);
    if (ret != 0) {
      allocator_->deallocate(connack, sizeof(ConnAckPacket));
      return ret;
    }
  }

  *packet = connack;
  return MQ_SUCCESS;
}

int MQTTParser::parse_puback(const uint8_t* buffer, size_t length, PubAckPacket** packet)
{
  if (length < 2) {
    return MQ_ERR_PACKET_INVALID;
  }

  PubAckPacket* puback = new (allocator_->allocate(sizeof(PubAckPacket))) PubAckPacket();
  puback->type = PacketType::PUBACK;

  size_t bytes_read = 0;

  // 解析包ID
  puback->packet_id = (buffer[bytes_read] << 8) | buffer[bytes_read + 1];
  bytes_read += 2;

  // 解析原因码
  if (bytes_read < length) {
    puback->reason_code = static_cast<ReasonCode>(buffer[bytes_read]);
    bytes_read++;

    // 解析属性
    if (bytes_read < length) {
      int ret = parse_properties(buffer + bytes_read, length - bytes_read, puback->properties,
                                 bytes_read);
      if (ret != 0) {
        allocator_->deallocate(puback, sizeof(PubAckPacket));
        return ret;
      }
    }
  }

  *packet = puback;
  return MQ_SUCCESS;
}

int MQTTParser::parse_pubrec(const uint8_t* buffer, size_t length, PubRecPacket** packet)
{
  if (length < 2) {
    return MQ_ERR_PACKET_INVALID;
  }

  PubRecPacket* pubrec = new (allocator_->allocate(sizeof(PubRecPacket))) PubRecPacket();
  pubrec->type = PacketType::PUBREC;

  size_t bytes_read = 0;

  // 解析包ID
  pubrec->packet_id = (buffer[bytes_read] << 8) | buffer[bytes_read + 1];
  bytes_read += 2;

  // 解析原因码
  if (bytes_read < length) {
    pubrec->reason_code = static_cast<ReasonCode>(buffer[bytes_read]);
    bytes_read++;

    // 解析属性
    if (bytes_read < length) {
      int ret = parse_properties(buffer + bytes_read, length - bytes_read, pubrec->properties,
                                 bytes_read);
      if (ret != 0) {
        allocator_->deallocate(pubrec, sizeof(PubRecPacket));
        return ret;
      }
    }
  }

  *packet = pubrec;
  return MQ_SUCCESS;
}

int MQTTParser::parse_pubrel(const uint8_t* buffer, size_t length, PubRelPacket** packet)
{
  if (length < 2) {
    return MQ_ERR_PACKET_INVALID;
  }

  PubRelPacket* pubrel = new (allocator_->allocate(sizeof(PubRelPacket))) PubRelPacket();
  pubrel->type = PacketType::PUBREL;

  size_t bytes_read = 0;

  // 解析包ID
  pubrel->packet_id = (buffer[bytes_read] << 8) | buffer[bytes_read + 1];
  bytes_read += 2;

  // 解析原因码
  if (bytes_read < length) {
    pubrel->reason_code = static_cast<ReasonCode>(buffer[bytes_read]);
    bytes_read++;

    // 解析属性
    if (bytes_read < length) {
      int ret = parse_properties(buffer + bytes_read, length - bytes_read, pubrel->properties,
                                 bytes_read);
      if (ret != 0) {
        allocator_->deallocate(pubrel, sizeof(PubRelPacket));
        return ret;
      }
    }
  }

  *packet = pubrel;
  return MQ_SUCCESS;
}

int MQTTParser::parse_pubcomp(const uint8_t* buffer, size_t length, PubCompPacket** packet)
{
  if (length < 2) {
    return MQ_ERR_PACKET_INVALID;
  }

  PubCompPacket* pubcomp = new (allocator_->allocate(sizeof(PubCompPacket))) PubCompPacket();
  pubcomp->type = PacketType::PUBCOMP;

  size_t bytes_read = 0;

  // 解析包ID
  pubcomp->packet_id = (buffer[bytes_read] << 8) | buffer[bytes_read + 1];
  bytes_read += 2;

  // 解析原因码
  if (bytes_read < length) {
    pubcomp->reason_code = static_cast<ReasonCode>(buffer[bytes_read]);
    bytes_read++;

    // 解析属性
    if (bytes_read < length) {
      int ret = parse_properties(buffer + bytes_read, length - bytes_read, pubcomp->properties,
                                 bytes_read);
      if (ret != 0) {
        allocator_->deallocate(pubcomp, sizeof(PubCompPacket));
        return ret;
      }
    }
  }

  *packet = pubcomp;
  return MQ_SUCCESS;
}

int MQTTParser::parse_suback(const uint8_t* buffer, size_t length, SubAckPacket** packet)
{
  if (length < 2) {
    return MQ_ERR_PACKET_INVALID;
  }

  SubAckPacket* suback = new (allocator_->allocate(sizeof(SubAckPacket))) SubAckPacket();
  suback->type = PacketType::SUBACK;

  size_t bytes_read = 0;

  // 解析包ID
  suback->packet_id = (buffer[bytes_read] << 8) | buffer[bytes_read + 1];
  bytes_read += 2;

  // 解析原因码列表
  while (bytes_read < length) {
    suback->reason_codes.push_back(static_cast<ReasonCode>(buffer[bytes_read]));
    bytes_read++;
  }

  *packet = suback;
  return MQ_SUCCESS;
}

int MQTTParser::parse_unsuback(const uint8_t* buffer, size_t length, UnsubAckPacket** packet)
{
  if (length < 2) {
    return MQ_ERR_PACKET_INVALID;
  }

  UnsubAckPacket* unsuback = new (allocator_->allocate(sizeof(UnsubAckPacket))) UnsubAckPacket();
  unsuback->type = PacketType::UNSUBACK;

  size_t bytes_read = 0;

  // 解析包ID
  unsuback->packet_id = (buffer[bytes_read] << 8) | buffer[bytes_read + 1];
  bytes_read += 2;

  // 解析原因码列表
  while (bytes_read < length) {
    unsuback->reason_codes.push_back(static_cast<ReasonCode>(buffer[bytes_read]));
    bytes_read++;
  }

  *packet = unsuback;
  return MQ_SUCCESS;
}

int MQTTParser::parse_pingreq(const uint8_t* buffer, size_t length, PingReqPacket** packet)
{
  PingReqPacket* pingreq = new (allocator_->allocate(sizeof(PingReqPacket))) PingReqPacket();
  pingreq->type = PacketType::PINGREQ;
  *packet = pingreq;
  return MQ_SUCCESS;
}

int MQTTParser::parse_pingresp(const uint8_t* buffer, size_t length, PingRespPacket** packet)
{
  PingRespPacket* pingresp = new (allocator_->allocate(sizeof(PingRespPacket))) PingRespPacket();
  pingresp->type = PacketType::PINGRESP;
  *packet = pingresp;
  return MQ_SUCCESS;
}

int MQTTParser::parse_disconnect(const uint8_t* buffer, size_t length, DisconnectPacket** packet)
{
  DisconnectPacket* disconnect =
      new (allocator_->allocate(sizeof(DisconnectPacket))) DisconnectPacket();
  disconnect->type = PacketType::DISCONNECT;

  size_t bytes_read = 0;

  // 解析原因码
  if (length > 0) {
    disconnect->reason_code = static_cast<ReasonCode>(buffer[bytes_read]);
    bytes_read++;

    // 解析属性
    if (bytes_read < length) {
      int ret = parse_properties(buffer + bytes_read, length - bytes_read, disconnect->properties,
                                 bytes_read);
      if (ret != 0) {
        allocator_->deallocate(disconnect, sizeof(DisconnectPacket));
        return ret;
      }
    }
  }

  *packet = disconnect;
  return MQ_SUCCESS;
}

int MQTTParser::parse_auth(const uint8_t* buffer, size_t length, AuthPacket** packet)
{
  AuthPacket* auth = new (allocator_->allocate(sizeof(AuthPacket))) AuthPacket();
  auth->type = PacketType::AUTH;

  size_t bytes_read = 0;

  // 解析原因码
  if (length > 0) {
    auth->reason_code = static_cast<ReasonCode>(buffer[bytes_read]);
    bytes_read++;

    // 解析属性
    if (bytes_read < length) {
      int ret =
          parse_properties(buffer + bytes_read, length - bytes_read, auth->properties, bytes_read);
      if (ret != 0) {
        allocator_->deallocate(auth, sizeof(AuthPacket));
        return ret;
      }
    }
  }

  *packet = auth;
  return MQ_SUCCESS;
}

int MQTTParser::parse_remaining_length(const uint8_t* buffer, size_t length,
                                       uint32_t& remaining_length, size_t& bytes_read)
{
  remaining_length = 0;
  bytes_read = 0;
  uint8_t multiplier = 1;

  do {
    if (bytes_read >= length) {
      LOG_ERROR("Packet too short for remaining length");
      return MQ_ERR_PACKET_INCOMPLETE;
    }
    remaining_length += (buffer[bytes_read] & 0x7F) * multiplier;
    multiplier *= 128;
  } while ((buffer[bytes_read++] & 0x80) != 0);

  return MQ_SUCCESS;
}

int MQTTParser::parse_string(const uint8_t* buffer, size_t length, std::string& str,
                             size_t& bytes_read)
{
  if (length < 2) {
    LOG_ERROR("Packet too short for string length");
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  uint16_t str_length = (buffer[0] << 8) | buffer[1];
  if (length < 2 + str_length) {
    LOG_ERROR("Packet too short for string content");
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  str.assign(reinterpret_cast<const char*>(buffer + 2), str_length);
  bytes_read += 2 + str_length;
  return MQ_SUCCESS;
}

int MQTTParser::parse_binary_data(const uint8_t* buffer, size_t length, std::vector<uint8_t>& data,
                                  size_t& bytes_read)
{
  if (length < 2) {
    LOG_ERROR("Packet too short for binary data length");
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  uint16_t data_length = (buffer[0] << 8) | buffer[1];
  if (length < 2 + data_length) {
    LOG_ERROR("Packet too short for binary data content");
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  data.assign(buffer + 2, buffer + 2 + data_length);
  bytes_read += 2 + data_length;
  return MQ_SUCCESS;
}

int MQTTParser::parse_properties(const uint8_t* buffer, size_t length, Properties& properties,
                                 size_t& bytes_read)
{
  bytes_read = 0;

  // 解析属性长度
  uint32_t properties_length = 0;
  int ret = parse_remaining_length(buffer, length, properties_length, bytes_read);
  if (ret != 0) {
    return ret;
  }

  size_t properties_end = bytes_read + properties_length;
  while (bytes_read < properties_end) {
    PropertyType type = static_cast<PropertyType>(buffer[bytes_read++]);
    switch (type) {
      case PropertyType::PayloadFormatIndicator:
        properties.payload_format_indicator = buffer[bytes_read++];
        break;
      case PropertyType::MessageExpiryInterval:
        properties.message_expiry_interval = (buffer[bytes_read] << 24) |
                                             (buffer[bytes_read + 1] << 16) |
                                             (buffer[bytes_read + 2] << 8) | buffer[bytes_read + 3];
        bytes_read += 4;
        break;
      case PropertyType::ContentType: {
        size_t local_read = 0;
        std::string temp_str;
        ret = parse_string(buffer + bytes_read, length - bytes_read, temp_str, local_read);
        if (ret != 0)
          return ret;
        properties.content_type = to_mqtt_string(temp_str, allocator_);
        bytes_read += local_read;
        break;
      }
      case PropertyType::ResponseTopic: {
        size_t local_read = 0;
        std::string temp_str;
        ret = parse_string(buffer + bytes_read, length - bytes_read, temp_str, local_read);
        if (ret != 0)
          return ret;
        properties.response_topic = to_mqtt_string(temp_str, allocator_);
        bytes_read += local_read;
        break;
      }
      case PropertyType::CorrelationData: {
        size_t local_read = 0;
        std::vector<uint8_t> temp_data;
        ret = parse_binary_data(buffer + bytes_read, length - bytes_read, temp_data, local_read);
        if (ret != 0)
          return ret;
        properties.correlation_data = to_mqtt_bytes(temp_data, allocator_);
        bytes_read += local_read;
        break;
      }
      case PropertyType::SubscriptionIdentifier: {
        uint32_t subid_value = 0;
        uint8_t byte;
        do {
          byte = buffer[bytes_read++];
          subid_value = (subid_value << 7) | (byte & 0x7F);
        } while ((byte & 0x80) != 0);
        properties.subscription_identifier = subid_value;
        break;
      }
      case PropertyType::SessionExpiryInterval:
        properties.session_expiry_interval = (buffer[bytes_read] << 24) |
                                             (buffer[bytes_read + 1] << 16) |
                                             (buffer[bytes_read + 2] << 8) | buffer[bytes_read + 3];
        bytes_read += 4;
        break;
      case PropertyType::AssignedClientIdentifier: {
        size_t local_read = 0;
        std::string temp_str;
        ret = parse_string(buffer + bytes_read, length - bytes_read, temp_str, local_read);
        if (ret != 0)
          return ret;
        properties.assigned_client_identifier = to_mqtt_string(temp_str, allocator_);
        bytes_read += local_read;
        break;
      }
      case PropertyType::ServerKeepAlive:
        properties.server_keep_alive = (buffer[bytes_read] << 8) | buffer[bytes_read + 1];
        bytes_read += 2;
        break;
      case PropertyType::AuthenticationMethod: {
        size_t local_read = 0;
        std::string temp_str;
        ret = parse_string(buffer + bytes_read, length - bytes_read, temp_str, local_read);
        if (ret != 0)
          return ret;
        properties.authentication_method = to_mqtt_string(temp_str, allocator_);
        bytes_read += local_read;
        break;
      }
      case PropertyType::AuthenticationData: {
        size_t local_read = 0;
        std::vector<uint8_t> temp_data;
        ret = parse_binary_data(buffer + bytes_read, length - bytes_read, temp_data, local_read);
        if (ret != 0)
          return ret;
        properties.authentication_data = to_mqtt_bytes(temp_data, allocator_);
        bytes_read += local_read;
        break;
      }
      case PropertyType::RequestProblemInformation:
        properties.request_problem_information = buffer[bytes_read++] != 0;
        break;
      case PropertyType::WillDelayInterval:
        properties.will_delay_interval = (buffer[bytes_read] << 24) |
                                         (buffer[bytes_read + 1] << 16) |
                                         (buffer[bytes_read + 2] << 8) | buffer[bytes_read + 3];
        bytes_read += 4;
        break;
      case PropertyType::RequestResponseInformation:
        properties.request_response_information = buffer[bytes_read++] != 0;
        break;
      case PropertyType::ResponseInformation: {
        size_t local_read = 0;
        std::string temp_str;
        ret = parse_string(buffer + bytes_read, length - bytes_read, temp_str, local_read);
        if (ret != 0)
          return ret;
        properties.response_information = to_mqtt_string(temp_str, allocator_);
        bytes_read += local_read;
        break;
      }
      case PropertyType::ServerReference: {
        size_t local_read = 0;
        std::string temp_str;
        ret = parse_string(buffer + bytes_read, length - bytes_read, temp_str, local_read);
        if (ret != 0)
          return ret;
        properties.server_reference = to_mqtt_string(temp_str, allocator_);
        bytes_read += local_read;
        break;
      }
      case PropertyType::ReasonString: {
        size_t local_read = 0;
        std::string temp_str;
        ret = parse_string(buffer + bytes_read, length - bytes_read, temp_str, local_read);
        if (ret != 0)
          return ret;
        properties.reason_string = to_mqtt_string(temp_str, allocator_);
        bytes_read += local_read;
        break;
      }
      case PropertyType::ReceiveMaximum:
        properties.receive_maximum = (buffer[bytes_read] << 8) | buffer[bytes_read + 1];
        bytes_read += 2;
        break;
      case PropertyType::TopicAliasMaximum:
        properties.topic_alias_maximum = (buffer[bytes_read] << 8) | buffer[bytes_read + 1];
        bytes_read += 2;
        break;
      case PropertyType::TopicAlias:
        properties.topic_alias = (buffer[bytes_read] << 8) | buffer[bytes_read + 1];
        bytes_read += 2;
        break;
      case PropertyType::MaximumQoS:
        properties.maximum_qos = buffer[bytes_read++];
        break;
      case PropertyType::RetainAvailable:
        properties.retain_available = buffer[bytes_read++] != 0;
        break;
      case PropertyType::UserProperty: {
        size_t local_read = 0;
        std::string key, value;
        ret = parse_string(buffer + bytes_read, length - bytes_read, key, local_read);
        if (ret != 0)
          return ret;
        bytes_read += local_read;
        local_read = 0;
        ret = parse_string(buffer + bytes_read, length - bytes_read, value, local_read);
        if (ret != 0)
          return ret;
        bytes_read += local_read;
        
        // 转换为MQTTString pair并添加
        MQTTString mqtt_key = to_mqtt_string(key, allocator_);
        MQTTString mqtt_value = to_mqtt_string(value, allocator_);
        properties.user_properties.push_back(std::make_pair(mqtt_key, mqtt_value));
        break;
      }
      case PropertyType::MaximumPacketSize:
        properties.maximum_packet_size = (buffer[bytes_read] << 24) |
                                         (buffer[bytes_read + 1] << 16) |
                                         (buffer[bytes_read + 2] << 8) | buffer[bytes_read + 3];
        bytes_read += 4;
        break;
      case PropertyType::WildcardSubscriptionAvailable:
        properties.wildcard_subscription_available = buffer[bytes_read++] != 0;
        break;
      case PropertyType::SubscriptionIdentifierAvailable:
        properties.subscription_identifier_available = buffer[bytes_read++] != 0;
        break;
      case PropertyType::SharedSubscriptionAvailable:
        properties.shared_subscription_available = buffer[bytes_read++] != 0;
        break;
      default:
        return MQ_ERR_PACKET_INVALID;
    }
  }

  return MQ_SUCCESS;
}

int MQTTParser::serialize_connect(const ConnectPacket* packet, std::vector<uint8_t>& buffer)
{
  // Calculate total length
  size_t total_length = 0;
  total_length += 2 + packet->protocol_name.length();  // Protocol name
  total_length += 1;                                   // Protocol version
  total_length += 1;                                   // Connect flags
  total_length += 2;                                   // Keep alive
  total_length += 2 + packet->client_id.length();      // Client ID

  if (packet->flags.will_flag) {
    total_length += 2 + packet->will_topic.length();    // Will topic
    total_length += 2 + packet->will_payload.length();  // Will payload
  }

  if (packet->flags.username_flag) {
    total_length += 2 + packet->username.length();  // Username
  }

  if (packet->flags.password_flag) {
    total_length += 2 + packet->password.length();  // Password
  }

  // Properties length
  total_length += 1;  // Properties length byte
  for (size_t i = 0; i < packet->properties.user_properties.size(); ++i) {
    const MQTTStringPair& prop = packet->properties.user_properties[i];
    total_length += 1;                         // Property type
    total_length += 2 + prop.first.length();   // Property key
    total_length += 2 + prop.second.length();  // Property value
  }

  // Reserve buffer space
  buffer.reserve(1 + 4 + total_length);  // 1 for packet type, 4 for remaining length

  // Packet type
  buffer.push_back(static_cast<uint8_t>(PacketType::CONNECT));

  // Remaining length
  serialize_remaining_length(total_length, buffer);

  // Protocol name
  serialize_mqtt_string(packet->protocol_name, buffer);

  // Protocol version
  buffer.push_back(packet->protocol_version);

  // Connect flags
  uint8_t flags = 0;
  flags |= packet->flags.clean_start << 1;
  flags |= packet->flags.will_flag << 2;
  flags |= packet->flags.will_qos << 3;
  flags |= packet->flags.will_retain << 5;
  flags |= packet->flags.password_flag << 6;
  flags |= packet->flags.username_flag << 7;
  buffer.push_back(flags);

  // Keep alive
  buffer.push_back((packet->keep_alive >> 8) & 0xFF);
  buffer.push_back(packet->keep_alive & 0xFF);

  // Properties
  buffer.push_back(0);  // Properties length placeholder
  size_t properties_start = buffer.size();
  serialize_properties(packet->properties, buffer);
  buffer[properties_start - 1] = buffer.size() - properties_start;  // Update properties length

  // Client ID
  serialize_mqtt_string(packet->client_id, buffer);

  // Will topic and payload
  if (packet->flags.will_flag) {
    serialize_mqtt_string(packet->will_topic, buffer);
    serialize_mqtt_string(packet->will_payload, buffer);
  }

  // Username
  if (packet->flags.username_flag) {
    serialize_mqtt_string(packet->username, buffer);
  }

  // Password
  if (packet->flags.password_flag) {
    serialize_mqtt_string(packet->password, buffer);
  }

  return 0;
}

int MQTTParser::serialize_publish(const PublishPacket* packet, std::vector<uint8_t>& buffer)
{
  // Calculate total length
  size_t total_length = 0;
  total_length += 2 + packet->topic_name.length();  // Topic name
  if (packet->qos > 0) {
    total_length += 2;  // Packet ID
  }

  // Properties length
  total_length += 1;  // Properties length byte
  for (size_t i = 0; i < packet->properties.user_properties.size(); ++i) {
    const MQTTStringPair& prop = packet->properties.user_properties[i];
    total_length += 1;                         // Property type
    total_length += 2 + prop.first.length();   // Property key
    total_length += 2 + prop.second.length();  // Property value
  }

  total_length += packet->payload.size();  // Payload

  // Reserve buffer space
  buffer.reserve(1 + 4 + total_length);  // 1 for packet type, 4 for remaining length

  // Packet type and flags
  uint8_t type_flags = static_cast<uint8_t>(PacketType::PUBLISH);
  type_flags |= (packet->dup << 3);
  type_flags |= (packet->qos << 1);
  type_flags |= packet->retain;
  buffer.push_back(type_flags);

  // Remaining length
  serialize_remaining_length(total_length, buffer);

  // Topic name
  serialize_mqtt_string(packet->topic_name, buffer);

  // Packet ID
  if (packet->qos > 0) {
    buffer.push_back((packet->packet_id >> 8) & 0xFF);
    buffer.push_back(packet->packet_id & 0xFF);
  }

  // Properties
  buffer.push_back(0);  // Properties length placeholder
  size_t properties_start = buffer.size();
  serialize_properties(packet->properties, buffer);
  buffer[properties_start - 1] = buffer.size() - properties_start;  // Update properties length

  // Payload
  buffer.insert(buffer.end(), packet->payload.begin(), packet->payload.end());

  return 0;
}

int MQTTParser::serialize_connack(const ConnAckPacket* packet, std::vector<uint8_t>& buffer)
{
  buffer.clear();
  buffer.push_back(static_cast<uint8_t>(PacketType::CONNACK));

  // 计算剩余长度
  uint32_t remaining_length = 2;  // 会话存在标志 + 原因码

  // 序列化属性
  std::vector<uint8_t> properties_buffer;
  int ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }
  remaining_length += properties_buffer.size();

  // 序列化剩余长度
  ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化会话存在标志和原因码
  buffer.push_back(packet->session_present ? 0x01 : 0x00);
  buffer.push_back(static_cast<uint8_t>(packet->reason_code));

  // 添加属性
  buffer.insert(buffer.end(), properties_buffer.begin(), properties_buffer.end());

  return MQ_SUCCESS;
}

int MQTTParser::serialize_puback(const PubAckPacket* packet, std::vector<uint8_t>& buffer)
{
  buffer.clear();
  buffer.push_back(static_cast<uint8_t>(PacketType::PUBACK));

  // 计算剩余长度
  uint32_t remaining_length = 2;  // 包ID

  // 序列化属性
  std::vector<uint8_t> properties_buffer;
  int ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }
  remaining_length += properties_buffer.size() + 1;  // +1 for reason code

  // 序列化剩余长度
  ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化包ID
  buffer.push_back((packet->packet_id >> 8) & 0xFF);
  buffer.push_back(packet->packet_id & 0xFF);

  // 序列化原因码
  buffer.push_back(static_cast<uint8_t>(packet->reason_code));

  // 添加属性
  buffer.insert(buffer.end(), properties_buffer.begin(), properties_buffer.end());

  return MQ_SUCCESS;
}

int MQTTParser::serialize_pubrec(const PubRecPacket* packet, std::vector<uint8_t>& buffer)
{
  buffer.clear();
  buffer.push_back(static_cast<uint8_t>(PacketType::PUBREC));

  // 计算剩余长度
  uint32_t remaining_length = 2;  // 包ID

  // 序列化属性
  std::vector<uint8_t> properties_buffer;
  int ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }
  remaining_length += properties_buffer.size() + 1;  // +1 for reason code

  // 序列化剩余长度
  ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化包ID
  buffer.push_back((packet->packet_id >> 8) & 0xFF);
  buffer.push_back(packet->packet_id & 0xFF);

  // 序列化原因码
  buffer.push_back(static_cast<uint8_t>(packet->reason_code));

  // 添加属性
  buffer.insert(buffer.end(), properties_buffer.begin(), properties_buffer.end());

  return MQ_SUCCESS;
}

int MQTTParser::serialize_pubrel(const PubRelPacket* packet, std::vector<uint8_t>& buffer)
{
  buffer.clear();
  buffer.push_back(static_cast<uint8_t>(PacketType::PUBREL) | 0x02);  // 设置QoS=1

  // 计算剩余长度
  uint32_t remaining_length = 2;  // 包ID

  // 序列化属性
  std::vector<uint8_t> properties_buffer;
  int ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }
  remaining_length += properties_buffer.size() + 1;  // +1 for reason code

  // 序列化剩余长度
  ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化包ID
  buffer.push_back((packet->packet_id >> 8) & 0xFF);
  buffer.push_back(packet->packet_id & 0xFF);

  // 序列化原因码
  buffer.push_back(static_cast<uint8_t>(packet->reason_code));

  // 添加属性
  buffer.insert(buffer.end(), properties_buffer.begin(), properties_buffer.end());

  return MQ_SUCCESS;
}

int MQTTParser::serialize_pubcomp(const PubCompPacket* packet, std::vector<uint8_t>& buffer)
{
  buffer.clear();
  buffer.push_back(static_cast<uint8_t>(PacketType::PUBCOMP));

  // 计算剩余长度
  uint32_t remaining_length = 2;  // 包ID

  // 序列化属性
  std::vector<uint8_t> properties_buffer;
  int ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }
  remaining_length += properties_buffer.size() + 1;  // +1 for reason code

  // 序列化剩余长度
  ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化包ID
  buffer.push_back((packet->packet_id >> 8) & 0xFF);
  buffer.push_back(packet->packet_id & 0xFF);

  // 序列化原因码
  buffer.push_back(static_cast<uint8_t>(packet->reason_code));

  // 添加属性
  buffer.insert(buffer.end(), properties_buffer.begin(), properties_buffer.end());

  return MQ_SUCCESS;
}

int MQTTParser::serialize_suback(const SubAckPacket* packet, std::vector<uint8_t>& buffer)
{
  buffer.clear();
  buffer.push_back(static_cast<uint8_t>(PacketType::SUBACK));

  // 计算剩余长度
  uint32_t remaining_length = 2 + packet->reason_codes.size();  // 包ID + 原因码列表

  // 序列化剩余长度
  int ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化包ID
  buffer.push_back((packet->packet_id >> 8) & 0xFF);
  buffer.push_back(packet->packet_id & 0xFF);

  // 序列化原因码列表
  for (const auto& reason_code : packet->reason_codes) {
    buffer.push_back(static_cast<uint8_t>(reason_code));
  }

  return MQ_SUCCESS;
}

int MQTTParser::serialize_unsuback(const UnsubAckPacket* packet, std::vector<uint8_t>& buffer)
{
  buffer.clear();
  buffer.push_back(static_cast<uint8_t>(PacketType::UNSUBACK));

  // 计算剩余长度
  uint32_t remaining_length = 2 + packet->reason_codes.size();  // 包ID + 原因码列表

  // 序列化剩余长度
  int ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化包ID
  buffer.push_back((packet->packet_id >> 8) & 0xFF);
  buffer.push_back(packet->packet_id & 0xFF);

  // 序列化原因码列表
  for (const auto& reason_code : packet->reason_codes) {
    buffer.push_back(static_cast<uint8_t>(reason_code));
  }

  return MQ_SUCCESS;
}

int MQTTParser::serialize_pingreq(const PingReqPacket* packet, std::vector<uint8_t>& buffer)
{
  buffer.clear();
  buffer.push_back(static_cast<uint8_t>(PacketType::PINGREQ));
  buffer.push_back(0x00);  // 剩余长度为0
  return MQ_SUCCESS;
}

int MQTTParser::serialize_pingresp(const PingRespPacket* packet, std::vector<uint8_t>& buffer)
{
  buffer.clear();
  buffer.push_back(static_cast<uint8_t>(PacketType::PINGRESP));
  buffer.push_back(0x00);  // 剩余长度为0
  return MQ_SUCCESS;
}

int MQTTParser::serialize_disconnect(const DisconnectPacket* packet, std::vector<uint8_t>& buffer)
{
  buffer.clear();
  buffer.push_back(static_cast<uint8_t>(PacketType::DISCONNECT));

  // 计算剩余长度
  uint32_t remaining_length = 1;  // 原因码

  // 序列化属性
  std::vector<uint8_t> properties_buffer;
  int ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }
  remaining_length += properties_buffer.size();

  // 序列化剩余长度
  ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化原因码
  buffer.push_back(static_cast<uint8_t>(packet->reason_code));

  // 添加属性
  buffer.insert(buffer.end(), properties_buffer.begin(), properties_buffer.end());

  return MQ_SUCCESS;
}

int MQTTParser::serialize_auth(const AuthPacket* packet, std::vector<uint8_t>& buffer)
{
  buffer.clear();
  buffer.push_back(static_cast<uint8_t>(PacketType::AUTH));

  // 计算剩余长度
  uint32_t remaining_length = 1;  // 原因码

  // 序列化属性
  std::vector<uint8_t> properties_buffer;
  int ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }
  remaining_length += properties_buffer.size();

  // 序列化剩余长度
  ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化原因码
  buffer.push_back(static_cast<uint8_t>(packet->reason_code));

  // 添加属性
  buffer.insert(buffer.end(), properties_buffer.begin(), properties_buffer.end());

  return MQ_SUCCESS;
}

int MQTTParser::parse_mqtt_string(const uint8_t* buffer, size_t length, MQTTString& str, size_t& bytes_read)
{
  std::string temp_str;
  int ret = parse_string(buffer, length, temp_str, bytes_read);
  if (ret == 0) {
    str = to_mqtt_string(temp_str, allocator_);
  }
  return ret;
}

int MQTTParser::parse_mqtt_binary_data(const uint8_t* buffer, size_t length, MQTTByteVector& data, size_t& bytes_read)
{
  data.assign(buffer, buffer + length);
  bytes_read = length;
  return 0;
}

int MQTTParser::serialize_mqtt_string(const MQTTString& str, std::vector<uint8_t>& buffer)
{
  buffer.push_back((str.length() >> 8) & 0xFF);
  buffer.push_back(str.length() & 0xFF);
  buffer.insert(buffer.end(), str.begin(), str.end());
  return 0;
}

int MQTTParser::serialize_mqtt_binary_data(const MQTTByteVector& data, std::vector<uint8_t>& buffer)
{
  buffer.push_back((data.size() >> 8) & 0xFF);
  buffer.push_back(data.size() & 0xFF);
  buffer.insert(buffer.end(), data.begin(), data.end());
  return 0;
}

int MQTTParser::serialize_subscribe(const SubscribePacket* packet, std::vector<uint8_t>& buffer)
{
  // Calculate total length
  size_t total_length = 0;
  total_length += 2;  // Packet ID

  // Properties length
  total_length += 1;  // Properties length byte
  for (size_t i = 0; i < packet->properties.user_properties.size(); ++i) {
    const MQTTStringPair& prop = packet->properties.user_properties[i];
    total_length += 1;                         // Property type
    total_length += 2 + prop.first.length();   // Property key
    total_length += 2 + prop.second.length();  // Property value
  }

  // Subscriptions
  for (size_t i = 0; i < packet->subscriptions.size(); ++i) {
    total_length += 2 + packet->subscriptions[i].first.length();  // Topic filter
    total_length += 1;                                            // QoS
  }

  // Reserve buffer space
  buffer.reserve(1 + 4 + total_length);  // 1 for packet type, 4 for remaining length

  // Packet type
  buffer.push_back(static_cast<uint8_t>(PacketType::SUBSCRIBE) | 0x02);  // Set reserved bits

  // Remaining length
  serialize_remaining_length(total_length, buffer);

  // Packet ID
  buffer.push_back((packet->packet_id >> 8) & 0xFF);
  buffer.push_back(packet->packet_id & 0xFF);

  // Properties
  buffer.push_back(0);  // Properties length placeholder
  size_t properties_start = buffer.size();
  serialize_properties(packet->properties, buffer);
  buffer[properties_start - 1] = buffer.size() - properties_start;  // Update properties length

  // Subscriptions
  for (size_t i = 0; i < packet->subscriptions.size(); ++i) {
    serialize_mqtt_string(packet->subscriptions[i].first, buffer);
    buffer.push_back(packet->subscriptions[i].second);
  }

  return 0;
}

int MQTTParser::serialize_remaining_length(uint32_t remaining_length, std::vector<uint8_t>& buffer)
{
  do {
    uint8_t byte = remaining_length % 128;
    remaining_length /= 128;
    if (remaining_length > 0) {
      byte |= 0x80;
    }
    buffer.push_back(byte);
  } while (remaining_length > 0);
  return 0;
}

int MQTTParser::serialize_string(const std::string& str, std::vector<uint8_t>& buffer)
{
  buffer.push_back((str.length() >> 8) & 0xFF);
  buffer.push_back(str.length() & 0xFF);
  buffer.insert(buffer.end(), str.begin(), str.end());
  return 0;
}

int MQTTParser::serialize_binary_data(const std::vector<uint8_t>& data,
                                      std::vector<uint8_t>& buffer)
{
  buffer.push_back((data.size() >> 8) & 0xFF);
  buffer.push_back(data.size() & 0xFF);
  buffer.insert(buffer.end(), data.begin(), data.end());
  return 0;
}

int MQTTParser::serialize_properties(const Properties& properties, std::vector<uint8_t>& buffer)
{
  std::vector<uint8_t> properties_buffer;

  // 序列化各个属性
  if (properties.payload_format_indicator != 0) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::PayloadFormatIndicator));
    properties_buffer.push_back(properties.payload_format_indicator);
  }

  if (properties.message_expiry_interval != 0) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::MessageExpiryInterval));
    properties_buffer.push_back((properties.message_expiry_interval >> 24) & 0xFF);
    properties_buffer.push_back((properties.message_expiry_interval >> 16) & 0xFF);
    properties_buffer.push_back((properties.message_expiry_interval >> 8) & 0xFF);
    properties_buffer.push_back(properties.message_expiry_interval & 0xFF);
  }

  if (!properties.content_type.empty()) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::ContentType));
    serialize_mqtt_string(properties.content_type, properties_buffer);
  }

  if (!properties.response_topic.empty()) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::ResponseTopic));
    serialize_mqtt_string(properties.response_topic, properties_buffer);
  }

  if (!properties.correlation_data.empty()) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::CorrelationData));
    serialize_mqtt_binary_data(properties.correlation_data, properties_buffer);
  }

  if (properties.subscription_identifier != 0) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::SubscriptionIdentifier));
    uint32_t value = properties.subscription_identifier;
    // 变长整数序列化
    do {
      uint8_t byte = value & 0x7F;
      value >>= 7;
      if (value != 0)
        byte |= 0x80;
      properties_buffer.push_back(byte);
    } while (value != 0);
  }

  if (properties.session_expiry_interval != 0) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::SessionExpiryInterval));
    properties_buffer.push_back((properties.session_expiry_interval >> 24) & 0xFF);
    properties_buffer.push_back((properties.session_expiry_interval >> 16) & 0xFF);
    properties_buffer.push_back((properties.session_expiry_interval >> 8) & 0xFF);
    properties_buffer.push_back(properties.session_expiry_interval & 0xFF);
  }

  if (!properties.assigned_client_identifier.empty()) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::AssignedClientIdentifier));
    serialize_mqtt_string(properties.assigned_client_identifier, properties_buffer);
  }

  if (properties.server_keep_alive != 0) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::ServerKeepAlive));
    properties_buffer.push_back((properties.server_keep_alive >> 8) & 0xFF);
    properties_buffer.push_back(properties.server_keep_alive & 0xFF);
  }

  if (!properties.authentication_method.empty()) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::AuthenticationMethod));
    serialize_mqtt_string(properties.authentication_method, properties_buffer);
  }

  if (!properties.authentication_data.empty()) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::AuthenticationData));
    serialize_mqtt_binary_data(properties.authentication_data, properties_buffer);
  }

  if (properties.request_problem_information) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::RequestProblemInformation));
    properties_buffer.push_back(1);
  }

  if (properties.will_delay_interval != 0) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::WillDelayInterval));
    properties_buffer.push_back((properties.will_delay_interval >> 24) & 0xFF);
    properties_buffer.push_back((properties.will_delay_interval >> 16) & 0xFF);
    properties_buffer.push_back((properties.will_delay_interval >> 8) & 0xFF);
    properties_buffer.push_back(properties.will_delay_interval & 0xFF);
  }

  if (properties.request_response_information) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::RequestResponseInformation));
    properties_buffer.push_back(1);
  }

  if (!properties.response_information.empty()) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::ResponseInformation));
    serialize_mqtt_string(properties.response_information, properties_buffer);
  }

  if (!properties.server_reference.empty()) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::ServerReference));
    serialize_mqtt_string(properties.server_reference, properties_buffer);
  }

  if (!properties.reason_string.empty()) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::ReasonString));
    serialize_mqtt_string(properties.reason_string, properties_buffer);
  }

  if (properties.receive_maximum != 0) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::ReceiveMaximum));
    properties_buffer.push_back((properties.receive_maximum >> 8) & 0xFF);
    properties_buffer.push_back(properties.receive_maximum & 0xFF);
  }

  if (properties.topic_alias_maximum != 0) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::TopicAliasMaximum));
    properties_buffer.push_back((properties.topic_alias_maximum >> 8) & 0xFF);
    properties_buffer.push_back(properties.topic_alias_maximum & 0xFF);
  }

  if (properties.topic_alias != 0) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::TopicAlias));
    properties_buffer.push_back((properties.topic_alias >> 8) & 0xFF);
    properties_buffer.push_back(properties.topic_alias & 0xFF);
  }

  if (properties.maximum_qos != 0) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::MaximumQoS));
    properties_buffer.push_back(properties.maximum_qos);
  }

  if (!properties.retain_available) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::RetainAvailable));
    properties_buffer.push_back(0);
  }

  for (const MQTTStringPair& prop : properties.user_properties) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::UserProperty));
    serialize_mqtt_string(prop.first, properties_buffer);
    serialize_mqtt_string(prop.second, properties_buffer);
  }

  if (properties.maximum_packet_size != 0) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::MaximumPacketSize));
    properties_buffer.push_back((properties.maximum_packet_size >> 24) & 0xFF);
    properties_buffer.push_back((properties.maximum_packet_size >> 16) & 0xFF);
    properties_buffer.push_back((properties.maximum_packet_size >> 8) & 0xFF);
    properties_buffer.push_back(properties.maximum_packet_size & 0xFF);
  }

  if (!properties.wildcard_subscription_available) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::WildcardSubscriptionAvailable));
    properties_buffer.push_back(0);
  }

  if (!properties.subscription_identifier_available) {
    properties_buffer.push_back(
        static_cast<uint8_t>(PropertyType::SubscriptionIdentifierAvailable));
    properties_buffer.push_back(0);
  }

  if (!properties.shared_subscription_available) {
    properties_buffer.push_back(static_cast<uint8_t>(PropertyType::SharedSubscriptionAvailable));
    properties_buffer.push_back(0);
  }

  // 序列化属性长度
  int ret = serialize_remaining_length(properties_buffer.size(), buffer);
  if (ret != 0) {
    return ret;
  }

  // 添加属性数据
  buffer.insert(buffer.end(), properties_buffer.begin(), properties_buffer.end());

  return MQ_SUCCESS;
}

}  // namespace mqtt