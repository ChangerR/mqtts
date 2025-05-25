#include "mqtt_parser.h"
#include <cstring>
#include "logger.h"

namespace mqtt {

MQTTParser::MQTTParser(MQTTAllocator* allocator) : allocator_(allocator) {}

MQTTParser::~MQTTParser() {}

int MQTTParser::parse_packet(const uint8_t* buffer, size_t length, Packet** packet)
{
  if (length < 2) {
    LOG->error("Packet too short");
    return -1;
  }

  PacketType type = static_cast<PacketType>(buffer[0] & 0xF0);
  uint32_t remaining_length;
  size_t bytes_read;

  int ret = parse_remaining_length(buffer + 1, length - 1, remaining_length, bytes_read);
  if (ret != 0) {
    LOG->error("Failed to parse remaining length");
    return ret;
  }

  if (length < 1 + bytes_read + remaining_length) {
    LOG->error("Packet incomplete");
    return -1;
  }

  switch (type) {
    case PacketType::CONNECT:
      return parse_connect(buffer, length, reinterpret_cast<ConnectPacket**>(packet));
    case PacketType::PUBLISH:
      return parse_publish(buffer, length, reinterpret_cast<PublishPacket**>(packet));
    case PacketType::SUBSCRIBE:
      return parse_subscribe(buffer, length, reinterpret_cast<SubscribePacket**>(packet));
    default:
      LOG->error("Unsupported packet type: 0x{:02x}", static_cast<uint8_t>(type));
      return -1;
  }
}

int MQTTParser::parse_connect(const uint8_t* buffer, size_t length, ConnectPacket** packet)
{
  if (length < 10) {  // Minimum CONNECT packet size
    LOG->error("CONNECT packet too short");
    return -1;
  }

  ConnectPacket* connect =
      reinterpret_cast<ConnectPacket*>(allocator_->allocate(sizeof(ConnectPacket)));
  if (!connect) {
    LOG->error("Failed to allocate CONNECT packet");
    return -1;
  }
  new (connect) ConnectPacket();

  size_t pos = 0;
  connect->type = PacketType::CONNECT;

  // Parse protocol name
  int ret = parse_string(buffer + pos, length - pos, connect->protocol_name, pos);
  if (ret != 0) {
    LOG->error("Failed to parse protocol name");
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return ret;
  }

  // Parse protocol version
  if (pos + 1 > length) {
    LOG->error("Packet too short for protocol version");
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return -1;
  }
  connect->protocol_version = buffer[pos++];

  // Parse connect flags
  if (pos + 1 > length) {
    LOG->error("Packet too short for connect flags");
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return -1;
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
    LOG->error("Packet too short for keep alive");
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return -1;
  }
  connect->keep_alive = (buffer[pos] << 8) | buffer[pos + 1];
  pos += 2;

  // Parse properties
  ret = parse_properties(buffer + pos, length - pos, connect->properties, pos);
  if (ret != 0) {
    LOG->error("Failed to parse properties");
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return ret;
  }

  // Parse client ID
  ret = parse_string(buffer + pos, length - pos, connect->client_id, pos);
  if (ret != 0) {
    LOG->error("Failed to parse client ID");
    allocator_->deallocate(connect, sizeof(ConnectPacket));
    return ret;
  }

  // Parse will topic and payload if will flag is set
  if (connect->flags.will_flag) {
    ret = parse_string(buffer + pos, length - pos, connect->will_topic, pos);
    if (ret != 0) {
      LOG->error("Failed to parse will topic");
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return ret;
    }

    ret = parse_string(buffer + pos, length - pos, connect->will_payload, pos);
    if (ret != 0) {
      LOG->error("Failed to parse will payload");
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return ret;
    }
  }

  // Parse username if username flag is set
  if (connect->flags.username_flag) {
    ret = parse_string(buffer + pos, length - pos, connect->username, pos);
    if (ret != 0) {
      LOG->error("Failed to parse username");
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return ret;
    }
  }

  // Parse password if password flag is set
  if (connect->flags.password_flag) {
    ret = parse_string(buffer + pos, length - pos, connect->password, pos);
    if (ret != 0) {
      LOG->error("Failed to parse password");
      allocator_->deallocate(connect, sizeof(ConnectPacket));
      return ret;
    }
  }

  *packet = connect;
  return 0;
}

int MQTTParser::parse_publish(const uint8_t* buffer, size_t length, PublishPacket** packet)
{
  if (length < 2) {
    LOG->error("PUBLISH packet too short");
    return -1;
  }

  PublishPacket* publish =
      reinterpret_cast<PublishPacket*>(allocator_->allocate(sizeof(PublishPacket)));
  if (!publish) {
    LOG->error("Failed to allocate PUBLISH packet");
    return -1;
  }
  new (publish) PublishPacket();

  size_t pos = 0;
  publish->type = PacketType::PUBLISH;

  // Parse flags
  uint8_t flags = buffer[0] & 0x0F;
  publish->dup = (flags >> 3) & 0x01;
  publish->qos = (flags >> 1) & 0x03;
  publish->retain = flags & 0x01;
  pos++;

  // Parse remaining length
  uint32_t remaining_length;
  int ret = parse_remaining_length(buffer + pos, length - pos, remaining_length, pos);
  if (ret != 0) {
    LOG->error("Failed to parse remaining length");
    allocator_->deallocate(publish, sizeof(PublishPacket));
    return ret;
  }

  // Parse topic name
  ret = parse_string(buffer + pos, length - pos, publish->topic_name, pos);
  if (ret != 0) {
    LOG->error("Failed to parse topic name");
    allocator_->deallocate(publish, sizeof(PublishPacket));
    return ret;
  }

  // Parse packet ID if QoS > 0
  if (publish->qos > 0) {
    if (pos + 2 > length) {
      LOG->error("Packet too short for packet ID");
      allocator_->deallocate(publish, sizeof(PublishPacket));
      return -1;
    }
    publish->packet_id = (buffer[pos] << 8) | buffer[pos + 1];
    pos += 2;
  }

  // Parse properties
  ret = parse_properties(buffer + pos, length - pos, publish->properties, pos);
  if (ret != 0) {
    LOG->error("Failed to parse properties");
    allocator_->deallocate(publish, sizeof(PublishPacket));
    return ret;
  }

  // Parse payload
  size_t payload_length = remaining_length - (pos - 2);  // Subtract header length
  publish->payload.assign(buffer + pos, buffer + pos + payload_length);
  pos += payload_length;

  *packet = publish;
  return 0;
}

int MQTTParser::parse_subscribe(const uint8_t* buffer, size_t length, SubscribePacket** packet)
{
  if (length < 5) {  // Minimum SUBSCRIBE packet size
    LOG->error("SUBSCRIBE packet too short");
    return -1;
  }

  SubscribePacket* subscribe =
      reinterpret_cast<SubscribePacket*>(allocator_->allocate(sizeof(SubscribePacket)));
  if (!subscribe) {
    LOG->error("Failed to allocate SUBSCRIBE packet");
    return -1;
  }
  new (subscribe) SubscribePacket();

  size_t pos = 0;
  subscribe->type = PacketType::SUBSCRIBE;

  // Skip packet type
  pos++;

  // Parse remaining length
  uint32_t remaining_length;
  int ret = parse_remaining_length(buffer + pos, length - pos, remaining_length, pos);
  if (ret != 0) {
    LOG->error("Failed to parse remaining length");
    allocator_->deallocate(subscribe, sizeof(SubscribePacket));
    return ret;
  }

  // Parse packet ID
  if (pos + 2 > length) {
    LOG->error("Packet too short for packet ID");
    allocator_->deallocate(subscribe, sizeof(SubscribePacket));
    return -1;
  }
  subscribe->packet_id = (buffer[pos] << 8) | buffer[pos + 1];
  pos += 2;

  // Parse properties
  ret = parse_properties(buffer + pos, length - pos, subscribe->properties, pos);
  if (ret != 0) {
    LOG->error("Failed to parse properties");
    allocator_->deallocate(subscribe, sizeof(SubscribePacket));
    return ret;
  }

  // Parse subscriptions
  while (pos < length) {
    std::string topic_filter;
    ret = parse_string(buffer + pos, length - pos, topic_filter, pos);
    if (ret != 0) {
      LOG->error("Failed to parse topic filter");
      allocator_->deallocate(subscribe, sizeof(SubscribePacket));
      return ret;
    }

    if (pos + 1 > length) {
      LOG->error("Packet too short for QoS");
      allocator_->deallocate(subscribe, sizeof(SubscribePacket));
      return -1;
    }
    uint8_t qos = buffer[pos++] & 0x03;
    subscribe->subscriptions.push_back(std::make_pair(topic_filter, qos));
  }

  *packet = subscribe;
  return 0;
}

int MQTTParser::parse_remaining_length(const uint8_t* buffer, size_t length,
                                       uint32_t& remaining_length, size_t& bytes_read)
{
  remaining_length = 0;
  bytes_read = 0;
  uint8_t multiplier = 1;

  do {
    if (bytes_read >= length) {
      LOG->error("Packet too short for remaining length");
      return -1;
    }
    remaining_length += (buffer[bytes_read] & 0x7F) * multiplier;
    multiplier *= 128;
  } while ((buffer[bytes_read++] & 0x80) != 0);

  return 0;
}

int MQTTParser::parse_string(const uint8_t* buffer, size_t length, std::string& str,
                             size_t& bytes_read)
{
  if (length < 2) {
    LOG->error("Packet too short for string length");
    return -1;
  }

  uint16_t str_length = (buffer[0] << 8) | buffer[1];
  if (length < 2 + str_length) {
    LOG->error("Packet too short for string content");
    return -1;
  }

  str.assign(reinterpret_cast<const char*>(buffer + 2), str_length);
  bytes_read += 2 + str_length;
  return 0;
}

int MQTTParser::parse_binary_data(const uint8_t* buffer, size_t length, std::vector<uint8_t>& data,
                                  size_t& bytes_read)
{
  if (length < 2) {
    LOG->error("Packet too short for binary data length");
    return -1;
  }

  uint16_t data_length = (buffer[0] << 8) | buffer[1];
  if (length < 2 + data_length) {
    LOG->error("Packet too short for binary data content");
    return -1;
  }

  data.assign(buffer + 2, buffer + 2 + data_length);
  bytes_read += 2 + data_length;
  return 0;
}

int MQTTParser::parse_properties(const uint8_t* buffer, size_t length,
                                 std::vector<std::pair<std::string, std::string>>& properties,
                                 size_t& bytes_read)
{
  if (length < 1) {
    LOG->error("Packet too short for properties length");
    return -1;
  }

  uint8_t properties_length = buffer[0];
  bytes_read = 1;

  while (bytes_read < properties_length + 1) {
    if (bytes_read >= length) {
      LOG->error("Packet too short for property");
      return -1;
    }

    PropertyType type = static_cast<PropertyType>(buffer[bytes_read++]);
    std::string key = std::to_string(static_cast<uint8_t>(type));
    std::string value;

    switch (type) {
      case PropertyType::PayloadFormatIndicator:
      case PropertyType::RequestProblemInformation:
      case PropertyType::RequestResponseInformation:
      case PropertyType::MaximumQoS:
      case PropertyType::RetainAvailable:
      case PropertyType::WildcardSubscriptionAvailable:
      case PropertyType::SubscriptionIdentifierAvailable:
      case PropertyType::SharedSubscriptionAvailable:
        if (bytes_read >= length) {
          LOG->error("Packet too short for property value");
          return -1;
        }
        value = std::to_string(buffer[bytes_read++]);
        break;

      case PropertyType::MessageExpiryInterval:
      case PropertyType::SessionExpiryInterval:
      case PropertyType::WillDelayInterval:
      case PropertyType::MaximumPacketSize:
        if (bytes_read + 4 > length) {
          LOG->error("Packet too short for property value");
          return -1;
        }
        value = std::to_string((buffer[bytes_read] << 24) | (buffer[bytes_read + 1] << 16) |
                               (buffer[bytes_read + 2] << 8) | buffer[bytes_read + 3]);
        bytes_read += 4;
        break;

      case PropertyType::ContentType:
      case PropertyType::ResponseTopic:
      case PropertyType::AssignedClientIdentifier:
      case PropertyType::AuthenticationMethod:
      case PropertyType::ResponseInformation:
      case PropertyType::ServerReference:
      case PropertyType::ReasonString: {
        std::string str;
        int ret = parse_string(buffer + bytes_read, length - bytes_read, str, bytes_read);
        if (ret != 0) {
          LOG->error("Failed to parse property string value");
          return ret;
        }
        value = str;
      } break;

      case PropertyType::CorrelationData:
      case PropertyType::AuthenticationData: {
        std::vector<uint8_t> data;
        int ret = parse_binary_data(buffer + bytes_read, length - bytes_read, data, bytes_read);
        if (ret != 0) {
          LOG->error("Failed to parse property binary value");
          return ret;
        }
        value = std::string(data.begin(), data.end());
      } break;

      case PropertyType::UserProperty: {
        std::string key_str, value_str;
        int ret = parse_string(buffer + bytes_read, length - bytes_read, key_str, bytes_read);
        if (ret != 0) {
          LOG->error("Failed to parse user property key");
          return ret;
        }
        ret = parse_string(buffer + bytes_read, length - bytes_read, value_str, bytes_read);
        if (ret != 0) {
          LOG->error("Failed to parse user property value");
          return ret;
        }
        key = key_str;
        value = value_str;
      } break;

      default:
        LOG->error("Unknown property type: 0x{:02x}", static_cast<uint8_t>(type));
        return -1;
    }

    properties.push_back(std::make_pair(key, value));
  }

  return 0;
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
  for (const auto& prop : packet->properties) {
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
  serialize_string(packet->protocol_name, buffer);

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
  serialize_string(packet->client_id, buffer);

  // Will topic and payload
  if (packet->flags.will_flag) {
    serialize_string(packet->will_topic, buffer);
    serialize_string(packet->will_payload, buffer);
  }

  // Username
  if (packet->flags.username_flag) {
    serialize_string(packet->username, buffer);
  }

  // Password
  if (packet->flags.password_flag) {
    serialize_string(packet->password, buffer);
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
  for (const auto& prop : packet->properties) {
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
  serialize_string(packet->topic_name, buffer);

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

int MQTTParser::serialize_subscribe(const SubscribePacket* packet, std::vector<uint8_t>& buffer)
{
  // Calculate total length
  size_t total_length = 0;
  total_length += 2;  // Packet ID

  // Properties length
  total_length += 1;  // Properties length byte
  for (const auto& prop : packet->properties) {
    total_length += 1;                         // Property type
    total_length += 2 + prop.first.length();   // Property key
    total_length += 2 + prop.second.length();  // Property value
  }

  // Subscriptions
  for (const auto& sub : packet->subscriptions) {
    total_length += 2 + sub.first.length();  // Topic filter
    total_length += 1;                       // QoS
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
  for (const auto& sub : packet->subscriptions) {
    serialize_string(sub.first, buffer);
    buffer.push_back(sub.second);
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

int MQTTParser::serialize_properties(
    const std::vector<std::pair<std::string, std::string>>& properties,
    std::vector<uint8_t>& buffer)
{
  for (const auto& prop : properties) {
    PropertyType type = static_cast<PropertyType>(std::stoi(prop.first));
    buffer.push_back(static_cast<uint8_t>(type));

    switch (type) {
      case PropertyType::PayloadFormatIndicator:
      case PropertyType::RequestProblemInformation:
      case PropertyType::RequestResponseInformation:
      case PropertyType::MaximumQoS:
      case PropertyType::RetainAvailable:
      case PropertyType::WildcardSubscriptionAvailable:
      case PropertyType::SubscriptionIdentifierAvailable:
      case PropertyType::SharedSubscriptionAvailable:
        buffer.push_back(std::stoi(prop.second));
        break;

      case PropertyType::MessageExpiryInterval:
      case PropertyType::SessionExpiryInterval:
      case PropertyType::WillDelayInterval:
      case PropertyType::MaximumPacketSize: {
        uint32_t value = std::stoul(prop.second);
        buffer.push_back((value >> 24) & 0xFF);
        buffer.push_back((value >> 16) & 0xFF);
        buffer.push_back((value >> 8) & 0xFF);
        buffer.push_back(value & 0xFF);
      } break;

      case PropertyType::ContentType:
      case PropertyType::ResponseTopic:
      case PropertyType::AssignedClientIdentifier:
      case PropertyType::AuthenticationMethod:
      case PropertyType::ResponseInformation:
      case PropertyType::ServerReference:
      case PropertyType::ReasonString:
        serialize_string(prop.second, buffer);
        break;

      case PropertyType::CorrelationData:
      case PropertyType::AuthenticationData: {
        std::vector<uint8_t> data(prop.second.begin(), prop.second.end());
        serialize_binary_data(data, buffer);
      } break;

      case PropertyType::UserProperty:
        serialize_string(prop.first, buffer);
        serialize_string(prop.second, buffer);
        break;

      default:
        LOG->error("Unknown property type: 0x{:02x}", static_cast<uint8_t>(type));
        return -1;
    }
  }
  return 0;
}

}  // namespace mqtt