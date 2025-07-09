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
      // CONNECT packets must have exact flags 0x00
      if ((buffer[0] & 0x0F) != 0x00) {
        LOG_ERROR("Invalid CONNECT packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_connect(buffer, length, reinterpret_cast<ConnectPacket**>(packet));
    case PacketType::PUBLISH:
      return parse_publish(buffer, length, reinterpret_cast<PublishPacket**>(packet));
    case PacketType::SUBSCRIBE:
      // SUBSCRIBE packets must have bit 1 set in the fixed header
      if ((buffer[0] & 0x0F) != 0x02) {
        LOG_ERROR("Invalid SUBSCRIBE packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_subscribe(buffer, length, reinterpret_cast<SubscribePacket**>(packet));
    case PacketType::UNSUBSCRIBE:
      // UNSUBSCRIBE packets must have bit 1 set in the fixed header
      if ((buffer[0] & 0x0F) != 0x02) {
        LOG_ERROR("Invalid UNSUBSCRIBE packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_unsubscribe(buffer, length, reinterpret_cast<UnsubscribePacket**>(packet));
    case PacketType::CONNACK:
      // CONNACK packets must have exact flags 0x00
      if ((buffer[0] & 0x0F) != 0x00) {
        LOG_ERROR("Invalid CONNACK packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_connack(buffer, length, reinterpret_cast<ConnAckPacket**>(packet));
    case PacketType::PUBACK:
      // PUBACK packets must have exact flags 0x00
      if ((buffer[0] & 0x0F) != 0x00) {
        LOG_ERROR("Invalid PUBACK packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_puback(buffer, length, reinterpret_cast<PubAckPacket**>(packet));
    case PacketType::PUBREC:
      // PUBREC packets must have exact flags 0x00
      if ((buffer[0] & 0x0F) != 0x00) {
        LOG_ERROR("Invalid PUBREC packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_pubrec(buffer, length, reinterpret_cast<PubRecPacket**>(packet));
    case PacketType::PUBREL:
      // PUBREL packets must have bit 1 set in the fixed header
      if ((buffer[0] & 0x0F) != 0x02) {
        LOG_ERROR("Invalid PUBREL packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_pubrel(buffer, length, reinterpret_cast<PubRelPacket**>(packet));
    case PacketType::PUBCOMP:
      // PUBCOMP packets must have exact flags 0x00
      if ((buffer[0] & 0x0F) != 0x00) {
        LOG_ERROR("Invalid PUBCOMP packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_pubcomp(buffer, length, reinterpret_cast<PubCompPacket**>(packet));
    case PacketType::SUBACK:
      // SUBACK packets must have exact flags 0x00
      if ((buffer[0] & 0x0F) != 0x00) {
        LOG_ERROR("Invalid SUBACK packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_suback(buffer, length, reinterpret_cast<SubAckPacket**>(packet));
    case PacketType::UNSUBACK:
      // UNSUBACK packets must have exact flags 0x00
      if ((buffer[0] & 0x0F) != 0x00) {
        LOG_ERROR("Invalid UNSUBACK packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_unsuback(buffer, length, reinterpret_cast<UnsubAckPacket**>(packet));
    case PacketType::PINGREQ:
      // PINGREQ packets must have exact flags 0x00
      if ((buffer[0] & 0x0F) != 0x00) {
        LOG_ERROR("Invalid PINGREQ packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_pingreq(buffer, length, reinterpret_cast<PingReqPacket**>(packet));
    case PacketType::PINGRESP:
      // PINGRESP packets must have exact flags 0x00
      if ((buffer[0] & 0x0F) != 0x00) {
        LOG_ERROR("Invalid PINGRESP packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_pingresp(buffer, length, reinterpret_cast<PingRespPacket**>(packet));
    case PacketType::DISCONNECT:
      // DISCONNECT packets must have exact flags 0x00
      if ((buffer[0] & 0x0F) != 0x00) {
        LOG_ERROR("Invalid DISCONNECT packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
      return parse_disconnect(buffer, length, reinterpret_cast<DisconnectPacket**>(packet));
    case PacketType::AUTH:
      // AUTH packets must have exact flags 0x00
      if ((buffer[0] & 0x0F) != 0x00) {
        LOG_ERROR("Invalid AUTH packet fixed header flags");
        return MQ_ERR_PACKET_TYPE;
      }
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
  size_t string_bytes_read = 0;
  ret = parse_mqtt_string(buffer + pos, length - pos, connect->protocol_name, string_bytes_read);
  if (ret != 0) {
    LOG_ERROR("Failed to parse protocol name");
    connect->~ConnectPacket();
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += string_bytes_read;

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
  string_bytes_read = 0;
  ret = parse_mqtt_string(buffer + pos, length - pos, connect->client_id, string_bytes_read);
  if (ret != 0) {
    LOG_ERROR("Failed to parse client ID");
    connect->~ConnectPacket();
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return MQ_ERR_CONNECT_CLIENT_ID;
  }
  pos += string_bytes_read;

  // Parse will topic and payload if will flag is set
  if (connect->flags.will_flag) {
    // Parse will properties first (MQTT v5)
    size_t will_properties_bytes_read = 0;
    ret = parse_properties(buffer + pos, length - pos, connect->will_properties, will_properties_bytes_read);
    if (ret != 0) {
      LOG_ERROR("Failed to parse will properties");
      connect->~ConnectPacket();
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return MQ_ERR_PACKET_INVALID;
    }
    pos += will_properties_bytes_read;

    // Parse will topic
    string_bytes_read = 0;
    ret = parse_mqtt_string(buffer + pos, length - pos, connect->will_topic, string_bytes_read);
    if (ret != 0) {
      LOG_ERROR("Failed to parse will topic");
      connect->~ConnectPacket();
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return MQ_ERR_PACKET_INVALID;
    }
    pos += string_bytes_read;

    // Parse will payload
    string_bytes_read = 0;
    ret = parse_mqtt_string(buffer + pos, length - pos, connect->will_payload, string_bytes_read);
    if (ret != 0) {
      LOG_ERROR("Failed to parse will payload");
      connect->~ConnectPacket();
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return MQ_ERR_PACKET_INVALID;
    }
    pos += string_bytes_read;
  }

  // Parse username if username flag is set
  if (connect->flags.username_flag) {
    string_bytes_read = 0;
    ret = parse_mqtt_string(buffer + pos, length - pos, connect->username, string_bytes_read);
    if (ret != 0) {
      LOG_ERROR("Failed to parse username");
      connect->~ConnectPacket();
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return MQ_ERR_PACKET_INVALID;
    }
    pos += string_bytes_read;
  }

  // Parse password if password flag is set
  if (connect->flags.password_flag) {
    string_bytes_read = 0;
    ret = parse_mqtt_string(buffer + pos, length - pos, connect->password, string_bytes_read);
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
  // Normalize invalid QoS levels to valid ones
  if (publish->qos > 2) {
    publish->qos = 2;  // Clamp to maximum valid QoS
  }
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
  size_t topic_bytes_read = 0;
  ret = parse_mqtt_string(buffer + pos, length - pos, publish->topic_name, topic_bytes_read);
  if (ret != 0) {
    LOG_ERROR("Failed to parse topic name");
    publish->~PublishPacket();
    allocator_->deallocate(publish, sizeof(PublishPacket));
    return MQ_ERR_PUBLISH_TOPIC;
  }
  pos += topic_bytes_read;

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
  size_t payload_bytes_read = 0;
  parse_mqtt_binary_data(buffer + pos, payload_length, publish->payload, payload_bytes_read);
  pos += payload_bytes_read;

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
    MQTTString topic_filter{MQTTStrAllocator(allocator_)};
    size_t topic_bytes_read = 0;
    ret = parse_mqtt_string(buffer + pos, payload_end - pos, topic_filter, topic_bytes_read);
    if (ret != 0) {
      LOG_ERROR("Failed to parse topic filter");
      subscribe->~SubscribePacket();
      allocator_->deallocate(subscribe, sizeof(SubscribePacket));
      return MQ_ERR_SUBSCRIBE_TOPIC;
    }
    pos += topic_bytes_read;

    if (pos + 1 > payload_end) {
      LOG_ERROR("Packet too short for QoS");
      subscribe->~SubscribePacket();
      allocator_->deallocate(subscribe, sizeof(SubscribePacket));
      return MQ_ERR_PACKET_INCOMPLETE;
    }
    uint8_t qos = buffer[pos++] & 0x03;

    // 直接添加到subscriptions，不需要转换
    subscribe->subscriptions.push_back(std::make_pair(std::move(topic_filter), qos));
  }

  *packet = subscribe;
  return MQ_SUCCESS;
}

int MQTTParser::parse_connack(const uint8_t* buffer, size_t length, ConnAckPacket** packet)
{
  if (length < 4) {  // Fixed header (1 byte) + remaining length (min 1 byte) + variable header (min 2 bytes)
    return MQ_ERR_PACKET_INVALID;
  }

  ConnAckPacket* connack = new (allocator_->allocate(sizeof(ConnAckPacket))) ConnAckPacket(allocator_);
  connack->type = PacketType::CONNACK;

  size_t pos = 0;

  // 跳过header (1字节)
  pos++;

  // 跳过remaining length字段
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret = parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    connack->~ConnAckPacket();
    allocator_->deallocate(connack, sizeof(ConnAckPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // 验证数据长度
  if (pos + remaining_length > length) {
    connack->~ConnAckPacket();
    allocator_->deallocate(connack, sizeof(ConnAckPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  // 解析会话存在标志和原因码
  if (pos + 2 > length) {
    connack->~ConnAckPacket();
    allocator_->deallocate(connack, sizeof(ConnAckPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  connack->session_present = (buffer[pos] & 0x01) != 0;
  pos++;

  connack->reason_code = static_cast<ReasonCode>(buffer[pos]);
  pos++;

  // 计算剩余的属性数据长度
  size_t properties_length = remaining_length - 2; // 减去会话标志和原因码的2字节

  // 解析属性
  if (properties_length > 0) {
    size_t properties_bytes_read = 0;
    ret = parse_properties(buffer + pos, properties_length, connack->properties, properties_bytes_read);
    if (ret != 0) {
      connack->~ConnAckPacket();
      allocator_->deallocate(connack, sizeof(ConnAckPacket));
      return ret;
    }
    if (properties_bytes_read != properties_length) {
      connack->~ConnAckPacket();
      allocator_->deallocate(connack, sizeof(ConnAckPacket));
      return MQ_ERR_PACKET_INVALID;
    }
  }

  *packet = connack;
  return MQ_SUCCESS;
}

int MQTTParser::parse_puback(const uint8_t* buffer, size_t length, PubAckPacket** packet)
{
  if (length < 5) {  // Minimum: 1 byte header + 1 byte remaining length + 2 bytes packet ID + 1 byte reason code
    return MQ_ERR_PACKET_INVALID;
  }

  PubAckPacket* puback = new (allocator_->allocate(sizeof(PubAckPacket))) PubAckPacket(allocator_);
  puback->type = PacketType::PUBACK;

  size_t pos = 0;

  // 跳过header (1字节)
  pos++;

  // 跳过remaining length字段
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret = parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    puback->~PubAckPacket();
    allocator_->deallocate(puback, sizeof(PubAckPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // 解析包ID
  if (pos + 2 > length) {
    puback->~PubAckPacket();
    allocator_->deallocate(puback, sizeof(PubAckPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  puback->packet_id = (buffer[pos] << 8) | buffer[pos + 1];
  pos += 2;

  // 解析原因码
  if (pos < length) {
    puback->reason_code = static_cast<ReasonCode>(buffer[pos]);
    pos++;

    // 解析属性
    if (pos < length) {
      size_t properties_bytes_read = 0;
      ret = parse_properties(buffer + pos, length - pos, puback->properties, properties_bytes_read);
      if (ret != 0) {
        puback->~PubAckPacket();
        allocator_->deallocate(puback, sizeof(PubAckPacket));
        return ret;
      }
    }
  } else {
    // 默认原因码
    puback->reason_code = ReasonCode::Success;
  }

  *packet = puback;
  return MQ_SUCCESS;
}

int MQTTParser::parse_pubrec(const uint8_t* buffer, size_t length, PubRecPacket** packet)
{
  if (length < 5) {  // Minimum: 1 byte header + 1 byte remaining length + 2 bytes packet ID + 1 byte reason code
    return MQ_ERR_PACKET_INVALID;
  }

  PubRecPacket* pubrec = new (allocator_->allocate(sizeof(PubRecPacket))) PubRecPacket(allocator_);
  pubrec->type = PacketType::PUBREC;

  size_t pos = 0;

  // 跳过header (1字节)
  pos++;

  // 跳过remaining length字段
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret = parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    pubrec->~PubRecPacket();
    allocator_->deallocate(pubrec, sizeof(PubRecPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // 解析包ID
  if (pos + 2 > length) {
    pubrec->~PubRecPacket();
    allocator_->deallocate(pubrec, sizeof(PubRecPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pubrec->packet_id = (buffer[pos] << 8) | buffer[pos + 1];
  pos += 2;

  // 解析原因码
  if (pos < length) {
    pubrec->reason_code = static_cast<ReasonCode>(buffer[pos]);
    pos++;

    // 解析属性
    if (pos < length) {
      size_t properties_bytes_read = 0;
      ret = parse_properties(buffer + pos, length - pos, pubrec->properties, properties_bytes_read);
      if (ret != 0) {
        pubrec->~PubRecPacket();
        allocator_->deallocate(pubrec, sizeof(PubRecPacket));
        return ret;
      }
    }
  } else {
    // 默认原因码
    pubrec->reason_code = ReasonCode::Success;
  }

  *packet = pubrec;
  return MQ_SUCCESS;
}

int MQTTParser::parse_pubrel(const uint8_t* buffer, size_t length, PubRelPacket** packet)
{
  if (length < 4) {  // Fixed header (1 byte) + remaining length (min 1 byte) + variable header (min 2 bytes)
    return MQ_ERR_PACKET_INVALID;
  }

  PubRelPacket* pubrel = new (allocator_->allocate(sizeof(PubRelPacket))) PubRelPacket(allocator_);
  pubrel->type = PacketType::PUBREL;

  size_t pos = 0;

  // 跳过header (1字节)
  pos++;

  // 跳过remaining length字段
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret = parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    pubrel->~PubRelPacket();
    allocator_->deallocate(pubrel, sizeof(PubRelPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // 验证数据长度
  if (pos + remaining_length > length) {
    pubrel->~PubRelPacket();
    allocator_->deallocate(pubrel, sizeof(PubRelPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  // 解析包ID
  if (pos + 2 > length) {
    pubrel->~PubRelPacket();
    allocator_->deallocate(pubrel, sizeof(PubRelPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }
  pubrel->packet_id = (buffer[pos] << 8) | buffer[pos + 1];
  pos += 2;

  // 计算剩余的数据长度
  size_t payload_end = pos + remaining_length - 2; // 减去包ID的2字节

  // 解析原因码（如果存在）
  if (pos < payload_end) {
    pubrel->reason_code = static_cast<ReasonCode>(buffer[pos]);
    pos++;

    // 解析属性（如果存在）
    if (pos < payload_end) {
      size_t properties_bytes_read = 0;
      ret = parse_properties(buffer + pos, payload_end - pos, pubrel->properties, properties_bytes_read);
      if (ret != 0) {
        pubrel->~PubRelPacket();
        allocator_->deallocate(pubrel, sizeof(PubRelPacket));
        return ret;
      }
    }
  } else {
    // 默认原因码
    pubrel->reason_code = ReasonCode::Success;
  }

  *packet = pubrel;
  return MQ_SUCCESS;
}

int MQTTParser::parse_pubcomp(const uint8_t* buffer, size_t length, PubCompPacket** packet)
{
  if (length < 4) {  // Fixed header (1 byte) + remaining length (min 1 byte) + variable header (min 2 bytes)
    return MQ_ERR_PACKET_INVALID;
  }

  PubCompPacket* pubcomp = new (allocator_->allocate(sizeof(PubCompPacket))) PubCompPacket(allocator_);
  pubcomp->type = PacketType::PUBCOMP;

  size_t pos = 0;

  // 跳过header (1字节)
  pos++;

  // 跳过remaining length字段
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret = parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    pubcomp->~PubCompPacket();
    allocator_->deallocate(pubcomp, sizeof(PubCompPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // 验证数据长度
  if (pos + remaining_length > length) {
    pubcomp->~PubCompPacket();
    allocator_->deallocate(pubcomp, sizeof(PubCompPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  // 解析包ID
  if (pos + 2 > length) {
    pubcomp->~PubCompPacket();
    allocator_->deallocate(pubcomp, sizeof(PubCompPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }
  pubcomp->packet_id = (buffer[pos] << 8) | buffer[pos + 1];
  pos += 2;

  // 计算剩余的数据长度
  size_t payload_end = pos + remaining_length - 2; // 减去包ID的2字节

  // 解析原因码（如果存在）
  if (pos < payload_end) {
    pubcomp->reason_code = static_cast<ReasonCode>(buffer[pos]);
    pos++;

    // 解析属性（如果存在）
    if (pos < payload_end) {
      size_t properties_bytes_read = 0;
      ret = parse_properties(buffer + pos, payload_end - pos, pubcomp->properties, properties_bytes_read);
      if (ret != 0) {
        pubcomp->~PubCompPacket();
        allocator_->deallocate(pubcomp, sizeof(PubCompPacket));
        return ret;
      }
    }
  } else {
    // 默认原因码
    pubcomp->reason_code = ReasonCode::Success;
  }

  *packet = pubcomp;
  return MQ_SUCCESS;
}

int MQTTParser::parse_suback(const uint8_t* buffer, size_t length, SubAckPacket** packet)
{
  if (length < 6) {  // Fixed header (1 byte) + remaining length (min 1 byte) + variable header (min 4 bytes)
    return MQ_ERR_PACKET_INVALID;
  }

  SubAckPacket* suback = new (allocator_->allocate(sizeof(SubAckPacket))) SubAckPacket(allocator_);
  suback->type = PacketType::SUBACK;

  size_t pos = 0;

  // 跳过header (1字节)
  pos++;

  // 跳过remaining length字段
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret = parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    suback->~SubAckPacket();
    allocator_->deallocate(suback, sizeof(SubAckPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // 验证数据长度
  if (pos + remaining_length > length) {
    suback->~SubAckPacket();
    allocator_->deallocate(suback, sizeof(SubAckPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  // 解析包ID
  if (pos + 2 > length) {
    suback->~SubAckPacket();
    allocator_->deallocate(suback, sizeof(SubAckPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }
  suback->packet_id = (buffer[pos] << 8) | buffer[pos + 1];
  pos += 2;

  // 计算剩余的数据长度，用于属性和原因码
  size_t payload_end = pos + remaining_length - 2; // 减去包ID的2字节

  // 解析属性
  if (pos < payload_end) {
    size_t properties_bytes_read = 0;
    ret = parse_properties(buffer + pos, payload_end - pos, suback->properties, properties_bytes_read);
    if (ret != 0) {
      suback->~SubAckPacket();
      allocator_->deallocate(suback, sizeof(SubAckPacket));
      return ret;
    }
    pos += properties_bytes_read;
  }

  // 解析原因码列表
  while (pos < length) {
    suback->reason_codes.push_back(static_cast<ReasonCode>(buffer[pos]));
    pos++;
  }

  // 验证至少有一个原因码
  if (suback->reason_codes.empty()) {
    suback->~SubAckPacket();
    allocator_->deallocate(suback, sizeof(SubAckPacket));
    return MQ_ERR_PACKET_INVALID;
  }

  *packet = suback;
  return MQ_SUCCESS;
}

int MQTTParser::parse_unsuback(const uint8_t* buffer, size_t length, UnsubAckPacket** packet)
{
  if (length < 6) {  // Fixed header (1 byte) + remaining length (min 1 byte) + variable header (min 4 bytes)
    return MQ_ERR_PACKET_INVALID;
  }

  UnsubAckPacket* unsuback = new (allocator_->allocate(sizeof(UnsubAckPacket))) UnsubAckPacket(allocator_);
  unsuback->type = PacketType::UNSUBACK;

  size_t pos = 0;

  // 跳过header (1字节)
  pos++;

  // 跳过remaining length字段
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret = parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    unsuback->~UnsubAckPacket();
    allocator_->deallocate(unsuback, sizeof(UnsubAckPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // 验证数据长度
  if (pos + remaining_length > length) {
    unsuback->~UnsubAckPacket();
    allocator_->deallocate(unsuback, sizeof(UnsubAckPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  // 解析包ID
  if (pos + 2 > length) {
    unsuback->~UnsubAckPacket();
    allocator_->deallocate(unsuback, sizeof(UnsubAckPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }
  unsuback->packet_id = (buffer[pos] << 8) | buffer[pos + 1];
  pos += 2;

  // 计算剩余的数据长度，用于属性和原因码
  size_t payload_end = pos + remaining_length - 2; // 减去包ID的2字节

  // 解析属性
  if (pos < payload_end) {
    size_t properties_bytes_read = 0;
    ret = parse_properties(buffer + pos, payload_end - pos, unsuback->properties, properties_bytes_read);
    if (ret != 0) {
      unsuback->~UnsubAckPacket();
      allocator_->deallocate(unsuback, sizeof(UnsubAckPacket));
      return ret;
    }
    pos += properties_bytes_read;
  }

  // 解析原因码列表
  while (pos < length) {
    unsuback->reason_codes.push_back(static_cast<ReasonCode>(buffer[pos]));
    pos++;
  }

  // 验证至少有一个原因码
  if (unsuback->reason_codes.empty()) {
    unsuback->~UnsubAckPacket();
    allocator_->deallocate(unsuback, sizeof(UnsubAckPacket));
    return MQ_ERR_PACKET_INVALID;
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
  if (length < 2) {
    LOG_ERROR("DISCONNECT packet too short");
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  DisconnectPacket* disconnect =
      new (allocator_->allocate(sizeof(DisconnectPacket))) DisconnectPacket();
  disconnect->type = PacketType::DISCONNECT;

  size_t pos = 0;

  // 跳过包类型字节
  pos++;

  // 解析剩余长度
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret =
      parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    LOG_ERROR("Failed to parse remaining length");
    allocator_->deallocate(disconnect, sizeof(DisconnectPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // 记录payload结束位置
  size_t payload_end = pos + remaining_length;
  if (payload_end > length) {
    LOG_ERROR("Packet too short for payload");
    allocator_->deallocate(disconnect, sizeof(DisconnectPacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  // 解析原因码 (如果有payload)
  if (remaining_length > 0) {
    if (pos >= payload_end) {
      LOG_ERROR("Packet too short for reason code");
      allocator_->deallocate(disconnect, sizeof(DisconnectPacket));
      return MQ_ERR_PACKET_INCOMPLETE;
    }
    disconnect->reason_code = static_cast<ReasonCode>(buffer[pos++]);

    // 解析属性 (如果还有数据)
    if (pos < payload_end) {
      size_t properties_bytes_read = 0;
      ret = parse_properties(buffer + pos, payload_end - pos, disconnect->properties,
                             properties_bytes_read);
      if (ret != 0) {
        LOG_ERROR("Failed to parse properties");
        allocator_->deallocate(disconnect, sizeof(DisconnectPacket));
        return ret;
      }
      pos += properties_bytes_read;
    }
  }

  *packet = disconnect;
  return MQ_SUCCESS;
}

int MQTTParser::parse_auth(const uint8_t* buffer, size_t length, AuthPacket** packet)
{
  if (length < 2) {
    LOG_ERROR("AUTH packet too short");
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  AuthPacket* auth = new (allocator_->allocate(sizeof(AuthPacket))) AuthPacket(allocator_);
  if (!auth) {
    LOG_ERROR("Failed to allocate AUTH packet");
    return MQ_ERR_MEMORY_ALLOC;
  }

  auth->type = PacketType::AUTH;

  size_t pos = 0;

  // Skip fixed header (packet type + remaining length)
  pos++; // Skip packet type byte

  // Skip remaining length bytes
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret = parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    LOG_ERROR("Failed to parse remaining length");
    auth->~AuthPacket();
    allocator_->deallocate(auth, sizeof(AuthPacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  size_t payload_end = pos + remaining_length;

  // Parse reason code if present
  if (pos < payload_end) {
    auth->reason_code = static_cast<ReasonCode>(buffer[pos++]);

    // Parse properties if present
    if (pos < payload_end) {
      size_t properties_bytes_read = 0;
      ret = parse_properties(buffer + pos, payload_end - pos, auth->properties, properties_bytes_read);
      if (ret != 0) {
        LOG_ERROR("Failed to parse properties");
        auth->~AuthPacket();
        allocator_->deallocate(auth, sizeof(AuthPacket));
        return ret;
      }
      pos += properties_bytes_read;
    }
  } else {
    // Default reason code for AUTH packet
    auth->reason_code = ReasonCode::Success;
  }

  *packet = auth;
  return MQ_SUCCESS;
}

int MQTTParser::parse_remaining_length(const uint8_t* buffer, size_t length,
                                       uint32_t& remaining_length, size_t& bytes_read)
{
  remaining_length = 0;
  bytes_read = 0;
  uint32_t multiplier = 1;

  do {
    if (bytes_read >= length) {
      LOG_ERROR("Packet too short for remaining length");
      return MQ_ERR_PACKET_INCOMPLETE;
    }

    if (bytes_read >= 4) {
      LOG_ERROR("Remaining length encoding too long (max 4 bytes)");
      return MQ_ERR_PACKET_INVALID;
    }

    uint8_t byte = buffer[bytes_read++];
    remaining_length += (byte & 0x7F) * multiplier;
    
    if (multiplier > 128 * 128 * 128) {
      LOG_ERROR("Remaining length exceeds maximum value");
      return MQ_ERR_PACKET_INVALID;
    }
    
    multiplier *= 128;
    
    if ((byte & 0x80) == 0) {
      break;
    }
  } while (true);

  return MQ_SUCCESS;
}

int MQTTParser::map_string_util_error(int string_util_error) const
{
  switch (string_util_error) {
    case mqtt::MQ_STRING_SUCCESS:
      return MQ_SUCCESS;
    case mqtt::MQ_STRING_ERR_BUFFER_TOO_SHORT:
      return MQ_ERR_PACKET_INCOMPLETE;
    case mqtt::MQ_STRING_ERR_INVALID_UTF8:
    case mqtt::MQ_STRING_ERR_INVALID_CONTROL_CHARS:
    case mqtt::MQ_STRING_ERR_STRING_TOO_LONG:
      return MQ_ERR_PACKET_INVALID;
    default:
      return MQ_ERR_PACKET_INVALID;
  }
}

int MQTTParser::parse_mqtt_string(const uint8_t* buffer, size_t length, MQTTString& str,
                                  size_t& bytes_read)
{
  int ret = mqtt::parse_mqtt_string(buffer, length, str, bytes_read);
  int mapped_error = map_string_util_error(ret);
  
  if (mapped_error == MQ_ERR_PACKET_INCOMPLETE) {
    LOG_ERROR("Packet too short for string");
  } else if (mapped_error == MQ_ERR_PACKET_INVALID) {
    LOG_ERROR("String parsing failed with error: %d", ret);
  }
  
  return mapped_error;
}

int MQTTParser::parse_string(const uint8_t* buffer, size_t length, std::string& str,
                             size_t& bytes_read)
{
  int ret = mqtt::parse_string(buffer, length, str, bytes_read);
  int mapped_error = map_string_util_error(ret);
  
  if (mapped_error == MQ_ERR_PACKET_INCOMPLETE) {
    LOG_ERROR("Packet too short for string");
  } else if (mapped_error == MQ_ERR_PACKET_INVALID) {
    LOG_ERROR("String parsing failed with error: %d", ret);
  }
  
  return mapped_error;
}

int MQTTParser::parse_binary_data(const uint8_t* buffer, size_t length, std::vector<uint8_t>& data,
                                  size_t& bytes_read)
{
  int ret = mqtt::parse_binary_data(buffer, length, data, bytes_read);
  int mapped_error = map_string_util_error(ret);
  
  if (mapped_error == MQ_ERR_PACKET_INCOMPLETE) {
    LOG_ERROR("Packet too short for binary data");
  } else if (mapped_error == MQ_ERR_PACKET_INVALID) {
    LOG_ERROR("Binary data parsing failed with error: %d", ret);
  }
  
  return mapped_error;
}

int MQTTParser::parse_mqtt_binary_data(const uint8_t* buffer, size_t length, MQTTByteVector& data,
                                       size_t& bytes_read)
{
  data.assign(buffer, buffer + length);
  bytes_read = length;
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
    if (bytes_read >= length) {
      LOG_ERROR("Properties parsing: buffer overflow, bytes_read={}, length={}", bytes_read, length);
      return MQ_ERR_PACKET_INVALID;
    }
    
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
        ret = parse_mqtt_string(buffer + bytes_read, length - bytes_read, properties.content_type,
                                local_read);
        if (ret != 0)
          return ret;
        bytes_read += local_read;
        break;
      }
      case PropertyType::ResponseTopic: {
        size_t local_read = 0;
        ret = parse_mqtt_string(buffer + bytes_read, length - bytes_read, properties.response_topic,
                                local_read);
        if (ret != 0)
          return ret;
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
        ret = parse_mqtt_string(buffer + bytes_read, length - bytes_read,
                                properties.assigned_client_identifier, local_read);
        if (ret != 0)
          return ret;
        bytes_read += local_read;
        break;
      }
      case PropertyType::ServerKeepAlive:
        properties.server_keep_alive = (buffer[bytes_read] << 8) | buffer[bytes_read + 1];
        bytes_read += 2;
        break;
      case PropertyType::AuthenticationMethod: {
        size_t local_read = 0;
        ret = parse_mqtt_string(buffer + bytes_read, length - bytes_read,
                                properties.authentication_method, local_read);
        if (ret != 0)
          return ret;
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
        ret = parse_mqtt_string(buffer + bytes_read, length - bytes_read,
                                properties.response_information, local_read);
        if (ret != 0)
          return ret;
        bytes_read += local_read;
        break;
      }
      case PropertyType::ServerReference: {
        size_t local_read = 0;
        ret = parse_mqtt_string(buffer + bytes_read, length - bytes_read,
                                properties.server_reference, local_read);
        if (ret != 0)
          return ret;
        bytes_read += local_read;
        break;
      }
      case PropertyType::ReasonString: {
        size_t local_read = 0;
        ret = parse_mqtt_string(buffer + bytes_read, length - bytes_read, properties.reason_string,
                                local_read);
        if (ret != 0)
          return ret;
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
        MQTTString key{MQTTStrAllocator(allocator_)};
        MQTTString value{MQTTStrAllocator(allocator_)};
        ret = parse_mqtt_string(buffer + bytes_read, length - bytes_read, key, local_read);
        if (ret != 0)
          return ret;
        bytes_read += local_read;
        local_read = 0;
        ret = parse_mqtt_string(buffer + bytes_read, length - bytes_read, value, local_read);
        if (ret != 0)
          return ret;
        bytes_read += local_read;

        // 直接添加MQTTString pair
        properties.user_properties.push_back(std::make_pair(std::move(key), std::move(value)));
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
        // According to MQTT v5.0 specification, unknown properties should be ignored
        // However, we cannot safely skip them without knowing their length
        // For now, we'll log a warning and fail gracefully
        LOG_WARN("Unknown property type: 0x{:02x}, failing packet parsing", static_cast<uint8_t>(type));
        return MQ_ERR_PACKET_INVALID;
    }
  }

  return MQ_SUCCESS;
}

int MQTTParser::serialize_connect(const ConnectPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();  // 重置buffer以便复用

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

  // Calculate properties length (using a temporary buffer)
  MQTTSerializeBuffer temp_props_buffer(allocator_);
  int props_ret = serialize_properties(packet->properties, temp_props_buffer);
  if (props_ret == 0) {
    total_length += temp_props_buffer.size();  // Properties with their variable-length header
  }

  // Reserve buffer space
  int ret = buffer.reserve(1 + 4 + total_length);  // 1 for packet type, 4 for remaining length
  if (ret != 0)
    return ret;

  // Packet type
  ret = buffer.push_back(static_cast<uint8_t>(PacketType::CONNECT));
  if (ret != 0)
    return ret;

  // Remaining length
  ret = serialize_remaining_length(total_length, buffer);
  if (ret != 0)
    return ret;

  // Protocol name
  ret = serialize_mqtt_string(packet->protocol_name, buffer);
  if (ret != 0)
    return ret;

  // Protocol version
  ret = buffer.push_back(packet->protocol_version);
  if (ret != 0)
    return ret;

  // Connect flags
  uint8_t flags = 0;
  flags |= packet->flags.clean_start << 1;
  flags |= packet->flags.will_flag << 2;
  flags |= packet->flags.will_qos << 3;
  flags |= packet->flags.will_retain << 5;
  flags |= packet->flags.password_flag << 6;
  flags |= packet->flags.username_flag << 7;
  ret = buffer.push_back(flags);
  if (ret != 0)
    return ret;

  // Keep alive
  ret = buffer.push_back((packet->keep_alive >> 8) & 0xFF);
  if (ret != 0)
    return ret;
  ret = buffer.push_back(packet->keep_alive & 0xFF);
  if (ret != 0)
    return ret;

  // Properties
  ret = serialize_properties(packet->properties, buffer);
  if (ret != 0)
    return ret;

  // Client ID
  ret = serialize_mqtt_string(packet->client_id, buffer);
  if (ret != 0)
    return ret;

  // Will properties, topic and payload
  if (packet->flags.will_flag) {
    // Will properties (MQTT v5)
    ret = serialize_properties(packet->will_properties, buffer);
    if (ret != 0)
      return ret;
    
    ret = serialize_mqtt_string(packet->will_topic, buffer);
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_string(packet->will_payload, buffer);
    if (ret != 0)
      return ret;
  }

  // Username
  if (packet->flags.username_flag) {
    ret = serialize_mqtt_string(packet->username, buffer);
    if (ret != 0)
      return ret;
  }

  // Password
  if (packet->flags.password_flag) {
    ret = serialize_mqtt_string(packet->password, buffer);
    if (ret != 0)
      return ret;
  }

  return MQ_SUCCESS;
}

int MQTTParser::serialize_publish(const PublishPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();  // 重置buffer以便复用

  // First, serialize properties to calculate their actual size
  MQTTSerializeBuffer temp_properties_buffer(allocator_);
  int ret = serialize_properties(packet->properties, temp_properties_buffer);
  if (ret != 0)
    return ret;

  // Calculate total length (properties buffer already includes length encoding)
  size_t total_length = 0;
  total_length += 2 + packet->topic_name.length();  // Topic name
  if (packet->qos > 0) {
    total_length += 2;  // Packet ID
  }
  
  // Properties: complete properties buffer (already includes length encoding)
  total_length += temp_properties_buffer.size();
  
  total_length += packet->payload.size();  // Payload

  // Reserve buffer space
  ret = buffer.reserve(1 + 4 + total_length);  // 1 for packet type, 4 for remaining length
  if (ret != 0)
    return ret;

  // Packet type and flags
  uint8_t type_flags = static_cast<uint8_t>(PacketType::PUBLISH);
  type_flags |= (packet->dup << 3);
  type_flags |= (packet->qos << 1);
  type_flags |= packet->retain;
  ret = buffer.push_back(type_flags);
  if (ret != 0)
    return ret;

  // Remaining length
  ret = serialize_remaining_length(total_length, buffer);
  if (ret != 0)
    return ret;

  // Topic name
  ret = serialize_mqtt_string(packet->topic_name, buffer);
  if (ret != 0)
    return ret;

  // Packet ID
  if (packet->qos > 0) {
    ret = buffer.push_back((packet->packet_id >> 8) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back(packet->packet_id & 0xFF);
    if (ret != 0)
      return ret;
  }

  // Properties - directly append the complete properties buffer (includes length encoding)
  ret = buffer.append(temp_properties_buffer.data(), temp_properties_buffer.size());
  if (ret != 0)
    return ret;

  // Payload
  ret = buffer.append(packet->payload.data(), packet->payload.size());
  if (ret != 0)
    return ret;

  return MQ_SUCCESS;
}

int MQTTParser::serialize_connack(const ConnAckPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::CONNACK));
  if (ret != 0)
    return ret;

  // 序列化属性
  MQTTSerializeBuffer properties_buffer(allocator_);
  ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }

  // 计算剩余长度
  uint32_t remaining_length = 2;  // 会话存在标志 + 原因码
  remaining_length += properties_buffer.size();

  // 序列化剩余长度
  ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化会话存在标志和原因码
  ret = buffer.push_back(packet->session_present ? 0x01 : 0x00);
  if (ret != 0)
    return ret;
  ret = buffer.push_back(static_cast<uint8_t>(packet->reason_code));
  if (ret != 0)
    return ret;

  // 添加属性
  ret = buffer.append(properties_buffer.data(), properties_buffer.size());
  if (ret != 0)
    return ret;

  return MQ_SUCCESS;
}

int MQTTParser::serialize_puback(const PubAckPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::PUBACK));
  if (ret != 0)
    return ret;

  // 计算剩余长度
  uint32_t remaining_length = 2;  // 包ID

  // 序列化属性
  MQTTSerializeBuffer properties_buffer(allocator_);
  ret = serialize_properties(packet->properties, properties_buffer);
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
  ret = buffer.push_back((packet->packet_id >> 8) & 0xFF);
  if (ret != 0)
    return ret;
  ret = buffer.push_back(packet->packet_id & 0xFF);
  if (ret != 0)
    return ret;

  // 序列化原因码
  ret = buffer.push_back(static_cast<uint8_t>(packet->reason_code));
  if (ret != 0)
    return ret;

  // 添加属性
  ret = buffer.append(properties_buffer.data(), properties_buffer.size());
  if (ret != 0)
    return ret;

  return MQ_SUCCESS;
}

int MQTTParser::serialize_pubrec(const PubRecPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::PUBREC));
  if (ret != 0)
    return ret;

  // 计算剩余长度
  uint32_t remaining_length = 2;  // 包ID

  // 序列化属性
  MQTTSerializeBuffer properties_buffer(allocator_);
  ret = serialize_properties(packet->properties, properties_buffer);
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
  ret = buffer.push_back((packet->packet_id >> 8) & 0xFF);
  if (ret != 0)
    return ret;
  ret = buffer.push_back(packet->packet_id & 0xFF);
  if (ret != 0)
    return ret;

  // 序列化原因码
  ret = buffer.push_back(static_cast<uint8_t>(packet->reason_code));
  if (ret != 0)
    return ret;

  // 添加属性
  ret = buffer.append(properties_buffer.data(), properties_buffer.size());
  if (ret != 0)
    return ret;

  return MQ_SUCCESS;
}

int MQTTParser::serialize_pubrel(const PubRelPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::PUBREL) | 0x02);  // 设置QoS=1
  if (ret != 0)
    return ret;

  // 计算剩余长度
  uint32_t remaining_length = 2;  // 包ID

  // 序列化属性
  MQTTSerializeBuffer properties_buffer(allocator_);
  ret = serialize_properties(packet->properties, properties_buffer);
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
  ret = buffer.push_back((packet->packet_id >> 8) & 0xFF);
  if (ret != 0)
    return ret;
  ret = buffer.push_back(packet->packet_id & 0xFF);
  if (ret != 0)
    return ret;

  // 序列化原因码
  ret = buffer.push_back(static_cast<uint8_t>(packet->reason_code));
  if (ret != 0)
    return ret;

  // 添加属性
  ret = buffer.append(properties_buffer.data(), properties_buffer.size());
  if (ret != 0)
    return ret;

  return MQ_SUCCESS;
}

int MQTTParser::serialize_pubcomp(const PubCompPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::PUBCOMP));
  if (ret != 0)
    return ret;

  // 计算剩余长度
  uint32_t remaining_length = 2;  // 包ID

  // 序列化属性
  MQTTSerializeBuffer properties_buffer(allocator_);
  ret = serialize_properties(packet->properties, properties_buffer);
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
  ret = buffer.push_back((packet->packet_id >> 8) & 0xFF);
  if (ret != 0)
    return ret;
  ret = buffer.push_back(packet->packet_id & 0xFF);
  if (ret != 0)
    return ret;

  // 序列化原因码
  ret = buffer.push_back(static_cast<uint8_t>(packet->reason_code));
  if (ret != 0)
    return ret;

  // 添加属性
  ret = buffer.append(properties_buffer.data(), properties_buffer.size());
  if (ret != 0)
    return ret;

  return MQ_SUCCESS;
}

int MQTTParser::serialize_suback(const SubAckPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::SUBACK));
  if (ret != 0)
    return ret;

  // 序列化属性到临时缓冲区
  MQTTSerializeBuffer properties_buffer(allocator_);
  ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }

  // 计算剩余长度: 包ID(2) + 属性内容（已包含长度编码） + 原因码列表
  uint32_t remaining_length = 2;  // 包ID
  remaining_length +=
      static_cast<uint32_t>(properties_buffer.size());  // 属性内容（已包含长度编码）
  remaining_length += packet->reason_codes.size();      // 原因码列表

  // 序列化剩余长度
  ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化包ID
  ret = buffer.push_back((packet->packet_id >> 8) & 0xFF);
  if (ret != 0)
    return ret;
  ret = buffer.push_back(packet->packet_id & 0xFF);
  if (ret != 0)
    return ret;

  // 添加属性内容（已包含长度编码）
  ret = buffer.append(properties_buffer.data(), properties_buffer.size());
  if (ret != 0)
    return ret;

  // 序列化原因码列表
  for (const ReasonCode& reason_code : packet->reason_codes) {
    ret = buffer.push_back(static_cast<uint8_t>(reason_code));
    if (ret != 0)
      return ret;
  }

  return MQ_SUCCESS;
}

int MQTTParser::serialize_unsuback(const UnsubAckPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::UNSUBACK));
  if (ret != 0)
    return ret;

  // 序列化属性到临时缓冲区
  MQTTSerializeBuffer properties_buffer(allocator_);
  ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }

  // 计算剩余长度: 包ID(2) + 属性内容（已包含长度编码） + 原因码列表
  uint32_t remaining_length = 2;  // 包ID
  remaining_length +=
      static_cast<uint32_t>(properties_buffer.size());  // 属性内容（已包含长度编码）
  remaining_length += packet->reason_codes.size();      // 原因码列表

  // 序列化剩余长度
  ret = serialize_remaining_length(remaining_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 序列化包ID
  ret = buffer.push_back((packet->packet_id >> 8) & 0xFF);
  if (ret != 0)
    return ret;
  ret = buffer.push_back(packet->packet_id & 0xFF);
  if (ret != 0)
    return ret;

  // 添加属性内容（已包含长度编码）
  ret = buffer.append(properties_buffer.data(), properties_buffer.size());
  if (ret != 0)
    return ret;

  // 序列化原因码列表
  for (const ReasonCode& reason_code : packet->reason_codes) {
    ret = buffer.push_back(static_cast<uint8_t>(reason_code));
    if (ret != 0)
      return ret;
  }

  return MQ_SUCCESS;
}

int MQTTParser::serialize_pingreq(const PingReqPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::PINGREQ));
  if (ret != 0)
    return ret;
  ret = buffer.push_back(0x00);  // 剩余长度为0
  if (ret != 0)
    return ret;
  return MQ_SUCCESS;
}

int MQTTParser::serialize_pingresp(const PingRespPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::PINGRESP));
  if (ret != 0)
    return ret;
  ret = buffer.push_back(0x00);  // 剩余长度为0
  if (ret != 0)
    return ret;
  return MQ_SUCCESS;
}

int MQTTParser::serialize_disconnect(const DisconnectPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::DISCONNECT));
  if (ret != 0)
    return ret;

  // 计算剩余长度
  uint32_t remaining_length = 1;  // 原因码

  // 序列化属性
  MQTTSerializeBuffer properties_buffer(allocator_);
  ret = serialize_properties(packet->properties, properties_buffer);
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
  ret = buffer.push_back(static_cast<uint8_t>(packet->reason_code));
  if (ret != 0)
    return ret;

  // 添加属性
  ret = buffer.append(properties_buffer.data(), properties_buffer.size());
  if (ret != 0)
    return ret;

  return MQ_SUCCESS;
}

int MQTTParser::serialize_auth(const AuthPacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::AUTH));
  if (ret != 0)
    return ret;

  // 计算剩余长度
  uint32_t remaining_length = 1;  // 原因码

  // 序列化属性
  MQTTSerializeBuffer properties_buffer(allocator_);
  ret = serialize_properties(packet->properties, properties_buffer);
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
  ret = buffer.push_back(static_cast<uint8_t>(packet->reason_code));
  if (ret != 0)
    return ret;

  // 添加属性
  ret = buffer.append(properties_buffer.data(), properties_buffer.size());
  if (ret != 0)
    return ret;

  return MQ_SUCCESS;
}

int MQTTParser::serialize_subscribe(const SubscribePacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  
  // Packet type
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::SUBSCRIBE) | 0x02);  // Set reserved bits
  if (ret != 0)
    return ret;

  // Serialize properties
  MQTTSerializeBuffer properties_buffer(allocator_);
  ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }

  // Calculate total length
  size_t total_length = 0;
  total_length += 2;  // Packet ID
  total_length += properties_buffer.size();  // Properties

  // Subscriptions
  for (size_t i = 0; i < packet->subscriptions.size(); ++i) {
    total_length += 2 + packet->subscriptions[i].first.length();  // Topic filter
    total_length += 1;                                            // QoS
  }

  // Remaining length
  ret = serialize_remaining_length(total_length, buffer);
  if (ret != 0)
    return ret;

  // Packet ID
  ret = buffer.push_back((packet->packet_id >> 8) & 0xFF);
  if (ret != 0)
    return ret;
  ret = buffer.push_back(packet->packet_id & 0xFF);
  if (ret != 0)
    return ret;

  // Properties
  ret = buffer.append(properties_buffer.data(), properties_buffer.size());
  if (ret != 0)
    return ret;

  // Subscriptions
  for (size_t i = 0; i < packet->subscriptions.size(); ++i) {
    ret = serialize_mqtt_string(packet->subscriptions[i].first, buffer);
    if (ret != 0)
      return ret;
    ret = buffer.push_back(packet->subscriptions[i].second);
    if (ret != 0)
      return ret;
  }

  return MQ_SUCCESS;
}

int MQTTParser::serialize_remaining_length(uint32_t remaining_length, MQTTSerializeBuffer& buffer)
{
  int ret;
  do {
    uint8_t byte = remaining_length % 128;
    remaining_length /= 128;
    if (remaining_length > 0) {
      byte |= 0x80;
    }
    ret = buffer.push_back(byte);
    if (ret != 0)
      return ret;
  } while (remaining_length > 0);
  return MQ_SUCCESS;
}

int MQTTParser::serialize_string(const std::string& str, MQTTSerializeBuffer& buffer)
{
  int ret = mqtt::serialize_string(str, buffer);
  int mapped_error = map_string_util_error(ret);
  
  if (mapped_error == MQ_ERR_PACKET_INVALID) {
    LOG_ERROR("String serialization failed with error: %d", ret);
  }
  
  return mapped_error;
}

int MQTTParser::serialize_mqtt_string(const MQTTString& str, MQTTSerializeBuffer& buffer)
{
  int ret = mqtt::serialize_mqtt_string(str, buffer);
  int mapped_error = map_string_util_error(ret);
  
  if (mapped_error == MQ_ERR_PACKET_INVALID) {
    LOG_ERROR("String serialization failed with error: %d", ret);
  }
  
  return mapped_error;
}

int MQTTParser::serialize_binary_data(const std::vector<uint8_t>& data, MQTTSerializeBuffer& buffer)
{
  int ret = mqtt::serialize_binary_data(data, buffer);
  int mapped_error = map_string_util_error(ret);
  
  if (mapped_error == MQ_ERR_PACKET_INVALID) {
    LOG_ERROR("Binary data serialization failed with error: %d", ret);
  }
  
  return mapped_error;
}

int MQTTParser::serialize_mqtt_binary_data(const MQTTByteVector& data, MQTTSerializeBuffer& buffer)
{
  int ret = mqtt::serialize_mqtt_binary_data(data, buffer);
  int mapped_error = map_string_util_error(ret);
  
  if (mapped_error == MQ_ERR_PACKET_INVALID) {
    LOG_ERROR("Binary data serialization failed with error: %d", ret);
  }
  
  return mapped_error;
}

int MQTTParser::serialize_properties(const Properties& properties, MQTTSerializeBuffer& buffer)
{
  // 第一遍：计算所有属性的总长度
  size_t properties_total_length = 0;

  // 计算各个属性的长度
  if (properties.payload_format_indicator != 0) {
    properties_total_length += 1 + 1;  // PropertyType + value
  }

  if (properties.message_expiry_interval != 0) {
    properties_total_length += 1 + 4;  // PropertyType + 4 bytes
  }

  if (!properties.content_type.empty()) {
    properties_total_length +=
        1 + 2 + properties.content_type.length();  // PropertyType + length + content
  }

  if (!properties.response_topic.empty()) {
    properties_total_length += 1 + 2 + properties.response_topic.length();
  }

  if (!properties.correlation_data.empty()) {
    properties_total_length += 1 + 2 + properties.correlation_data.size();
  }

  if (properties.subscription_identifier != 0) {
    properties_total_length += 1;  // PropertyType
    // 计算变长整数的长度
    uint32_t value = properties.subscription_identifier;
    do {
      properties_total_length++;
      value >>= 7;
    } while (value != 0);
  }

  if (properties.session_expiry_interval != 0) {
    properties_total_length += 1 + 4;  // PropertyType + 4 bytes
  }

  if (!properties.assigned_client_identifier.empty()) {
    properties_total_length += 1 + 2 + properties.assigned_client_identifier.length();
  }

  if (properties.server_keep_alive != 0) {
    properties_total_length += 1 + 2;  // PropertyType + 2 bytes
  }

  if (!properties.authentication_method.empty()) {
    properties_total_length += 1 + 2 + properties.authentication_method.length();
  }

  if (!properties.authentication_data.empty()) {
    properties_total_length += 1 + 2 + properties.authentication_data.size();
  }

  if (properties.request_problem_information) {
    properties_total_length += 1 + 1;  // PropertyType + 1 byte
  }

  if (properties.will_delay_interval != 0) {
    properties_total_length += 1 + 4;  // PropertyType + 4 bytes
  }

  if (properties.request_response_information) {
    properties_total_length += 1 + 1;  // PropertyType + 1 byte
  }

  if (!properties.response_information.empty()) {
    properties_total_length += 1 + 2 + properties.response_information.length();
  }

  if (!properties.server_reference.empty()) {
    properties_total_length += 1 + 2 + properties.server_reference.length();
  }

  if (!properties.reason_string.empty()) {
    properties_total_length += 1 + 2 + properties.reason_string.length();
  }

  if (properties.receive_maximum != 0) {
    properties_total_length += 1 + 2;  // PropertyType + 2 bytes
  }

  if (properties.topic_alias_maximum != 0) {
    properties_total_length += 1 + 2;  // PropertyType + 2 bytes
  }

  if (properties.topic_alias != 0) {
    properties_total_length += 1 + 2;  // PropertyType + 2 bytes
  }

  if (properties.maximum_qos != 0) {
    properties_total_length += 1 + 1;  // PropertyType + 1 byte
  }

  if (!properties.retain_available) {
    properties_total_length += 1 + 1;  // PropertyType + 1 byte
  }

  for (const MQTTStringPair& prop : properties.user_properties) {
    properties_total_length += 1;                         // PropertyType
    properties_total_length += 2 + prop.first.length();   // key length + key
    properties_total_length += 2 + prop.second.length();  // value length + value
  }

  if (properties.maximum_packet_size != 0) {
    properties_total_length += 1 + 4;  // PropertyType + 4 bytes
  }

  if (!properties.wildcard_subscription_available) {
    properties_total_length += 1 + 1;  // PropertyType + 1 byte
  }

  if (!properties.subscription_identifier_available) {
    properties_total_length += 1 + 1;  // PropertyType + 1 byte
  }

  if (!properties.shared_subscription_available) {
    properties_total_length += 1 + 1;  // PropertyType + 1 byte
  }

  // 序列化属性长度
  int ret = serialize_remaining_length(properties_total_length, buffer);
  if (ret != 0) {
    return ret;
  }

  // 第二遍：直接序列化所有属性到目标buffer
  if (properties.payload_format_indicator != 0) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::PayloadFormatIndicator));
    if (ret != 0)
      return ret;
    ret = buffer.push_back(properties.payload_format_indicator);
    if (ret != 0)
      return ret;
  }

  if (properties.message_expiry_interval != 0) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::MessageExpiryInterval));
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.message_expiry_interval >> 24) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.message_expiry_interval >> 16) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.message_expiry_interval >> 8) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back(properties.message_expiry_interval & 0xFF);
    if (ret != 0)
      return ret;
  }

  if (!properties.content_type.empty()) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::ContentType));
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_string(properties.content_type, buffer);
    if (ret != 0)
      return ret;
  }

  if (!properties.response_topic.empty()) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::ResponseTopic));
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_string(properties.response_topic, buffer);
    if (ret != 0)
      return ret;
  }

  if (!properties.correlation_data.empty()) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::CorrelationData));
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_binary_data(properties.correlation_data, buffer);
    if (ret != 0)
      return ret;
  }

  if (properties.subscription_identifier != 0) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::SubscriptionIdentifier));
    if (ret != 0)
      return ret;
    uint32_t value = properties.subscription_identifier;
    // 变长整数序列化
    do {
      uint8_t byte = value & 0x7F;
      value >>= 7;
      if (value != 0)
        byte |= 0x80;
      ret = buffer.push_back(byte);
      if (ret != 0)
        return ret;
    } while (value != 0);
  }

  if (properties.session_expiry_interval != 0) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::SessionExpiryInterval));
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.session_expiry_interval >> 24) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.session_expiry_interval >> 16) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.session_expiry_interval >> 8) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back(properties.session_expiry_interval & 0xFF);
    if (ret != 0)
      return ret;
  }

  if (!properties.assigned_client_identifier.empty()) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::AssignedClientIdentifier));
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_string(properties.assigned_client_identifier, buffer);
    if (ret != 0)
      return ret;
  }

  if (properties.server_keep_alive != 0) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::ServerKeepAlive));
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.server_keep_alive >> 8) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back(properties.server_keep_alive & 0xFF);
    if (ret != 0)
      return ret;
  }

  if (!properties.authentication_method.empty()) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::AuthenticationMethod));
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_string(properties.authentication_method, buffer);
    if (ret != 0)
      return ret;
  }

  if (!properties.authentication_data.empty()) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::AuthenticationData));
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_binary_data(properties.authentication_data, buffer);
    if (ret != 0)
      return ret;
  }

  if (properties.request_problem_information) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::RequestProblemInformation));
    if (ret != 0)
      return ret;
    ret = buffer.push_back(1);
    if (ret != 0)
      return ret;
  }

  if (properties.will_delay_interval != 0) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::WillDelayInterval));
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.will_delay_interval >> 24) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.will_delay_interval >> 16) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.will_delay_interval >> 8) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back(properties.will_delay_interval & 0xFF);
    if (ret != 0)
      return ret;
  }

  if (properties.request_response_information) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::RequestResponseInformation));
    if (ret != 0)
      return ret;
    ret = buffer.push_back(1);
    if (ret != 0)
      return ret;
  }

  if (!properties.response_information.empty()) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::ResponseInformation));
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_string(properties.response_information, buffer);
    if (ret != 0)
      return ret;
  }

  if (!properties.server_reference.empty()) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::ServerReference));
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_string(properties.server_reference, buffer);
    if (ret != 0)
      return ret;
  }

  if (!properties.reason_string.empty()) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::ReasonString));
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_string(properties.reason_string, buffer);
    if (ret != 0)
      return ret;
  }

  if (properties.receive_maximum != 0) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::ReceiveMaximum));
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.receive_maximum >> 8) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back(properties.receive_maximum & 0xFF);
    if (ret != 0)
      return ret;
  }

  if (properties.topic_alias_maximum != 0) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::TopicAliasMaximum));
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.topic_alias_maximum >> 8) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back(properties.topic_alias_maximum & 0xFF);
    if (ret != 0)
      return ret;
  }

  if (properties.topic_alias != 0) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::TopicAlias));
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.topic_alias >> 8) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back(properties.topic_alias & 0xFF);
    if (ret != 0)
      return ret;
  }

  if (properties.maximum_qos != 0) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::MaximumQoS));
    if (ret != 0)
      return ret;
    ret = buffer.push_back(properties.maximum_qos);
    if (ret != 0)
      return ret;
  }

  if (!properties.retain_available) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::RetainAvailable));
    if (ret != 0)
      return ret;
    ret = buffer.push_back(0);
    if (ret != 0)
      return ret;
  }

  for (const MQTTStringPair& prop : properties.user_properties) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::UserProperty));
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_string(prop.first, buffer);
    if (ret != 0)
      return ret;
    ret = serialize_mqtt_string(prop.second, buffer);
    if (ret != 0)
      return ret;
  }

  if (properties.maximum_packet_size != 0) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::MaximumPacketSize));
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.maximum_packet_size >> 24) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.maximum_packet_size >> 16) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back((properties.maximum_packet_size >> 8) & 0xFF);
    if (ret != 0)
      return ret;
    ret = buffer.push_back(properties.maximum_packet_size & 0xFF);
    if (ret != 0)
      return ret;
  }

  if (!properties.wildcard_subscription_available) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::WildcardSubscriptionAvailable));
    if (ret != 0)
      return ret;
    ret = buffer.push_back(0);
    if (ret != 0)
      return ret;
  }

  if (!properties.subscription_identifier_available) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::SubscriptionIdentifierAvailable));
    if (ret != 0)
      return ret;
    ret = buffer.push_back(0);
    if (ret != 0)
      return ret;
  }

  if (!properties.shared_subscription_available) {
    ret = buffer.push_back(static_cast<uint8_t>(PropertyType::SharedSubscriptionAvailable));
    if (ret != 0)
      return ret;
    ret = buffer.push_back(0);
    if (ret != 0)
      return ret;
  }

  return MQ_SUCCESS;
}

int MQTTParser::serialize_unsubscribe(const UnsubscribePacket* packet, MQTTSerializeBuffer& buffer)
{
  buffer.clear();
  
  // Packet type
  int ret = buffer.push_back(static_cast<uint8_t>(PacketType::UNSUBSCRIBE) | 0x02);  // Set reserved bits
  if (ret != 0)
    return ret;

  // Serialize properties
  MQTTSerializeBuffer properties_buffer(allocator_);
  ret = serialize_properties(packet->properties, properties_buffer);
  if (ret != 0) {
    return ret;
  }

  // Calculate total length
  size_t total_length = 0;
  total_length += 2;  // Packet ID
  total_length += properties_buffer.size();  // Properties

  // Topic filters
  for (size_t i = 0; i < packet->topic_filters.size(); ++i) {
    total_length += 2 + packet->topic_filters[i].length();  // Topic filter
  }

  // Remaining length
  ret = serialize_remaining_length(total_length, buffer);
  if (ret != 0)
    return ret;

  // Packet ID
  ret = buffer.push_back((packet->packet_id >> 8) & 0xFF);
  if (ret != 0)
    return ret;
  ret = buffer.push_back(packet->packet_id & 0xFF);
  if (ret != 0)
    return ret;

  // Properties
  ret = buffer.append(properties_buffer.data(), properties_buffer.size());
  if (ret != 0)
    return ret;

  // Topic filters
  for (size_t i = 0; i < packet->topic_filters.size(); ++i) {
    ret = serialize_mqtt_string(packet->topic_filters[i], buffer);
    if (ret != 0)
      return ret;
  }

  return MQ_SUCCESS;
}

int MQTTParser::parse_unsubscribe(const uint8_t* buffer, size_t length, UnsubscribePacket** packet)
{
  if (length < 5) {  // Minimum UNSUBSCRIBE packet size
    LOG_ERROR("UNSUBSCRIBE packet too short");
    return MQ_ERR_PACKET_INCOMPLETE;
  }

  UnsubscribePacket* unsubscribe =
      new (allocator_->allocate(sizeof(UnsubscribePacket))) UnsubscribePacket(allocator_);
  if (!unsubscribe) {
    LOG_ERROR("Failed to allocate UNSUBSCRIBE packet");
    return MQ_ERR_MEMORY_ALLOC;
  }

  size_t pos = 0;
  unsubscribe->type = PacketType::UNSUBSCRIBE;

  // Skip packet type
  pos++;

  // Parse remaining length
  uint32_t remaining_length;
  size_t remaining_length_bytes;
  int ret =
      parse_remaining_length(buffer + pos, length - pos, remaining_length, remaining_length_bytes);
  if (ret != 0) {
    LOG_ERROR("Failed to parse remaining length");
    unsubscribe->~UnsubscribePacket();
    allocator_->deallocate(unsubscribe, sizeof(UnsubscribePacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += remaining_length_bytes;

  // 记录header + remaining length的总大小
  size_t header_size = pos;
  size_t payload_end = header_size + remaining_length;

  // Parse packet ID
  if (pos + 2 > length) {
    LOG_ERROR("Packet too short for packet ID");
    unsubscribe->~UnsubscribePacket();
    allocator_->deallocate(unsubscribe, sizeof(UnsubscribePacket));
    return MQ_ERR_PACKET_INCOMPLETE;
  }
  unsubscribe->packet_id = (buffer[pos] << 8) | buffer[pos + 1];
  pos += 2;

  // Parse properties
  size_t properties_bytes_read = 0;
  ret =
      parse_properties(buffer + pos, length - pos, unsubscribe->properties, properties_bytes_read);
  if (ret != 0) {
    LOG_ERROR("Failed to parse properties");
    unsubscribe->~UnsubscribePacket();
    allocator_->deallocate(unsubscribe, sizeof(UnsubscribePacket));
    return MQ_ERR_PACKET_INVALID;
  }
  pos += properties_bytes_read;

  // Parse topic filters
  while (pos < payload_end) {
    MQTTString topic_filter{MQTTStrAllocator(allocator_)};
    size_t topic_bytes_read = 0;
    ret = parse_mqtt_string(buffer + pos, payload_end - pos, topic_filter, topic_bytes_read);
    if (ret != 0) {
      LOG_ERROR("Failed to parse topic filter");
      unsubscribe->~UnsubscribePacket();
      allocator_->deallocate(unsubscribe, sizeof(UnsubscribePacket));
      return MQ_ERR_PACKET_INVALID;
    }
    pos += topic_bytes_read;

    // 直接添加到topic_filters
    unsubscribe->topic_filters.push_back(std::move(topic_filter));
  }

  *packet = unsubscribe;
  return MQ_SUCCESS;
}

}  // namespace mqtt