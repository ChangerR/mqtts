#include "mqtt_protocol_handler.h"
#include <cstring>
#include "co_routine.h"
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_parser.h"
#include "mqtt_socket.h"
namespace mqtt {

static const size_t INITIAL_BUFFER_SIZE = 1024;
static const size_t MAX_BUFFER_SIZE = 1024 * 1024;  // 1MB max size

MQTTProtocolHandler::MQTTProtocolHandler(MQTTAllocator* allocator)
    : allocator_(allocator),
      socket_(nullptr),
      client_port_(0),
      connected_(false),
      next_packet_id_(1),
      buffer_(nullptr),
      current_buffer_size_(0),
      bytes_read_(0),
      bytes_needed_(1),
      state_(ReadState::READ_HEADER),
      packet_type_(0),
      remaining_length_(0),
      header_size_(0),
      client_id_(MQTTSTLAllocator<char>(allocator)),
      client_ip_(MQTTSTLAllocator<char>(allocator)),
      subscriptions_(MQTTSTLAllocator<MQTTString>(allocator))
{
  // Allocate initial buffer
  buffer_ = (char*)allocator_->allocate(INITIAL_BUFFER_SIZE);
  if (!buffer_) {
    LOG_ERROR("Failed to allocate initial buffer");
    return;
  }
  current_buffer_size_ = INITIAL_BUFFER_SIZE;

  // Create parser
  parser_ = new (allocator_->allocate(sizeof(MQTTParser))) MQTTParser(allocator_);
}

MQTTProtocolHandler::~MQTTProtocolHandler()
{
  if (buffer_) {
    allocator_->deallocate(buffer_, current_buffer_size_);
  }
  if (parser_) {
    parser_->~MQTTParser();
    allocator_->deallocate(parser_, sizeof(MQTTParser));
  }
}

void MQTTProtocolHandler::init(MQTTSocket* socket, const std::string& client_ip, int client_port)
{
  socket_ = socket;
  client_ip_ = to_mqtt_string(client_ip, allocator_);
  client_port_ = client_port;
}

int MQTTProtocolHandler::process()
{
  int ret = MQ_SUCCESS;

  while (MQ_SUCC(ret)) {
    // Ensure buffer has enough space
    ret = ensure_buffer_size(bytes_needed_);
    if (ret != 0) {
      break;
    }

    // Read packet data
    ret = read_packet();
    if (ret != 0) {
      break;
    }

    // Parse packet if complete
    if (state_ == ReadState::READ_PAYLOAD && bytes_read_ >= header_size_ + remaining_length_) {
      ret = parse_packet();
      if (ret != 0) {
        // Reset state machine on parse error
        state_ = ReadState::READ_HEADER;
        bytes_needed_ = 1;
        bytes_read_ = 0;
        remaining_length_ = 0;
        header_size_ = 0;  // 重置header_size_
        continue;
      }

      // 处理完一个完整的包后,将剩余的字节移动到缓冲区开头
      size_t packet_size = bytes_read_;  // 当前包的总大小
      // 注意：在READ_PAYLOAD状态完成时，bytes_read_包含了整个包
      // 因此packet_size就是bytes_read_本身

      // 由于我们是逐字节读取的，通常不会有剩余数据
      // 但为了健壮性，还是保留这个逻辑
      bytes_read_ = 0;

      // 重置状态为读取下一个包头
      state_ = ReadState::READ_HEADER;
      bytes_needed_ = 1;
      remaining_length_ = 0;  // 重置remaining length
      header_size_ = 0;       // 重置header_size_
    }
  }

  return ret;
}

int MQTTProtocolHandler::ensure_buffer_size(size_t needed_size)
{
  if (bytes_read_ + needed_size > current_buffer_size_) {
    size_t new_size = std::min(current_buffer_size_ * 2, MAX_BUFFER_SIZE);
    if (bytes_read_ + needed_size > new_size) {
      LOG_ERROR("Packet too large from client {}:{} (needed: {}, max: {})", from_mqtt_string(client_ip_),
                client_port_, bytes_read_ + needed_size, MAX_BUFFER_SIZE);
      return MQ_ERR_PACKET_TOO_LARGE;
    }

    // Allocate new buffer
    char* new_buffer = (char*)allocator_->allocate(new_size);
    if (!new_buffer) {
      LOG_ERROR("Failed to resize buffer for client {}:{} (new size: {})", from_mqtt_string(client_ip_), client_port_,
                new_size);
      return MQ_ERR_MEMORY_ALLOC;
    }

    // Copy existing data
    memcpy(new_buffer, buffer_, bytes_read_);

    // Free old buffer and update pointer
    allocator_->deallocate(buffer_, current_buffer_size_);
    buffer_ = new_buffer;
    current_buffer_size_ = new_size;

    LOG_DEBUG("Buffer resized to {} bytes for client {}:{}", new_size, from_mqtt_string(client_ip_), client_port_);
  }
  return MQ_SUCCESS;
}

int MQTTProtocolHandler::read_packet()
{
  // Use co_poll to wait for read event
  struct pollfd pf = {0};
  pf.fd = socket_->get_fd();
  pf.events = (POLLIN | POLLERR | POLLHUP);
  co_poll(co_get_epoll_ct(), &pf, 1, 100);  // 100ms timeout

  // Check if socket is still valid
  if (!socket_->is_connected()) {
    LOG_WARN("Client {}:{} disconnected", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_SOCKET;
  }

  // Try to receive data
  int len = bytes_needed_;
  int ret = socket_->recv(buffer_ + bytes_read_, len);
  if (MQ_FAIL(ret)) {
    LOG_WARN("Client {}:{} disconnected unexpectedly", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_SOCKET_RECV;
  }

  // Check if connection closed
  if (len == 0) {
    LOG_WARN("Client {}:{} closed connection", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_SOCKET;
  }

  // Update bytes read
  bytes_read_ += len;

  // Process received data based on current state
  switch (state_) {
    case ReadState::READ_HEADER: {
      if (bytes_read_ >= 1) {
        packet_type_ = buffer_[0] & 0xF0;  // Get packet type
        state_ = ReadState::READ_REMAINING;
        bytes_needed_ = 1;  // Start reading remaining length
        // 保留header，不重置bytes_read_
      } else {
        // Still need more data for header
        bytes_needed_ = 1 - bytes_read_;
      }
      break;
    }

    case ReadState::READ_REMAINING: {
      if (bytes_read_ > 1) {  // 至少有header + 1字节的remaining length
        // 计算当前正在处理的remaining length字节的索引
        int remaining_length_index = bytes_read_ - 1 - 1;  // -1 for header, -1 for 0-based index
        uint8_t byte = buffer_[bytes_read_ - 1];

        // 重置remaining_length_在读取第一个remaining length字节时
        if (remaining_length_index == 0) {
          remaining_length_ = 0;
        }

        remaining_length_ += (byte & 0x7F) * (1 << (7 * remaining_length_index));

        if ((byte & 0x80) == 0) {
          // Check if remaining length is too large
          if (remaining_length_ > MAX_BUFFER_SIZE - bytes_read_) {
            LOG_ERROR("Packet too large from client {}:{} (size: {}, max: {})", from_mqtt_string(client_ip_),
                      client_port_, remaining_length_ + bytes_read_, MAX_BUFFER_SIZE);
            return MQ_ERR_PACKET_TOO_LARGE;
          }
          // Remaining length complete
          header_size_ = bytes_read_;  // 记录header + remaining length的总大小
          state_ = ReadState::READ_PAYLOAD;
          bytes_needed_ = remaining_length_;
          // 保留header和remaining length，不重置bytes_read_
        } else if (remaining_length_index >= 3) {
          // Invalid remaining length (more than 4 bytes)
          LOG_ERROR("Invalid remaining length from client {}:{}", from_mqtt_string(client_ip_), client_port_);
          return MQ_ERR_PACKET_INVALID;
        } else {
          // Need more bytes for remaining length
          bytes_needed_ = 1;
        }
      } else {
        // Still need more data for remaining length
        bytes_needed_ = 1;
      }
      break;
    }

    case ReadState::READ_PAYLOAD: {
      // 计算已读取的payload字节数
      size_t payload_read = bytes_read_ - header_size_;
      if (payload_read < remaining_length_) {
        // Still need more data for payload
        bytes_needed_ = remaining_length_ - payload_read;
      }
      // Packet will be parsed in the main loop when complete
      break;
    }
  }

  return MQ_SUCCESS;
}

int MQTTProtocolHandler::parse_packet()
{
  // Complete packet received, parse it
  Packet* packet = nullptr;
  LOG_DEBUG("Parsing packet from client {}:{}, type: 0x{:02x}, size: {}", 
            from_mqtt_string(client_ip_), client_port_, packet_type_, bytes_read_);

  int ret = parser_->parse_packet(reinterpret_cast<const uint8_t*>(buffer_), bytes_read_, &packet);
  if (ret != 0) {
    LOG_ERROR("Failed to parse MQTT packet from client {}:{}, error: {}", 
              from_mqtt_string(client_ip_), client_port_, ret);
    return MQ_ERR_PACKET_INVALID;
  }

  LOG_DEBUG("Successfully parsed packet type: 0x{:02x} from client {}:{}", 
            static_cast<uint8_t>(packet->type), from_mqtt_string(client_ip_), client_port_);

  // Handle packet based on type
  switch (packet->type) {
    case PacketType::CONNECT: {
      LOG_DEBUG("Processing CONNECT packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      ConnectPacket* connect = reinterpret_cast<ConnectPacket*>(packet);
      ret = handle_connect(connect);
      connect->~ConnectPacket();
      allocator_->deallocate(packet, sizeof(ConnectPacket));
      break;
    }
    case PacketType::CONNACK: {
      LOG_DEBUG("Processing CONNACK packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      ConnAckPacket* connack = reinterpret_cast<ConnAckPacket*>(packet);
      ret = handle_connack(connack);
      connack->~ConnAckPacket();
      allocator_->deallocate(packet, sizeof(ConnAckPacket));
      break;
    }
    case PacketType::PUBLISH: {
      LOG_DEBUG("Processing PUBLISH packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      PublishPacket* publish = reinterpret_cast<PublishPacket*>(packet);
      ret = handle_publish(publish);
      publish->~PublishPacket();
      allocator_->deallocate(packet, sizeof(PublishPacket));
      break;
    }
    case PacketType::PUBACK: {
      LOG_DEBUG("Processing PUBACK packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      PubAckPacket* puback = reinterpret_cast<PubAckPacket*>(packet);
      ret = handle_puback(puback);
      puback->~PubAckPacket();
      allocator_->deallocate(packet, sizeof(PubAckPacket));
      break;
    }
    case PacketType::PUBREC: {
      LOG_DEBUG("Processing PUBREC packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      PubRecPacket* pubrec = reinterpret_cast<PubRecPacket*>(packet);
      ret = handle_pubrec(pubrec);
      pubrec->~PubRecPacket();
      allocator_->deallocate(packet, sizeof(PubRecPacket));
      break;
    }
    case PacketType::PUBREL: {
      LOG_DEBUG("Processing PUBREL packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      PubRelPacket* pubrel = reinterpret_cast<PubRelPacket*>(packet);
      ret = handle_pubrel(pubrel);
      pubrel->~PubRelPacket();
      allocator_->deallocate(packet, sizeof(PubRelPacket));
      break;
    }
    case PacketType::PUBCOMP: {
      LOG_DEBUG("Processing PUBCOMP packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      PubCompPacket* pubcomp = reinterpret_cast<PubCompPacket*>(packet);
      ret = handle_pubcomp(pubcomp);
      pubcomp->~PubCompPacket();
      allocator_->deallocate(packet, sizeof(PubCompPacket));
      break;
    }
    case PacketType::SUBSCRIBE: {
      LOG_DEBUG("Processing SUBSCRIBE packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      SubscribePacket* subscribe = reinterpret_cast<SubscribePacket*>(packet);
      ret = handle_subscribe(subscribe);
      subscribe->~SubscribePacket();
      allocator_->deallocate(packet, sizeof(SubscribePacket));
      break;
    }
    case PacketType::SUBACK: {
      LOG_DEBUG("Processing SUBACK packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      SubAckPacket* suback = reinterpret_cast<SubAckPacket*>(packet);
      ret = handle_suback(suback);
      suback->~SubAckPacket();
      allocator_->deallocate(packet, sizeof(SubAckPacket));
      break;
    }
    case PacketType::UNSUBSCRIBE: {
      LOG_DEBUG("Processing UNSUBSCRIBE packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      UnsubscribePacket* unsubscribe = reinterpret_cast<UnsubscribePacket*>(packet);
      ret = handle_unsubscribe(unsubscribe);
      unsubscribe->~UnsubscribePacket();
      allocator_->deallocate(packet, sizeof(UnsubscribePacket));
      break;
    }
    case PacketType::UNSUBACK: {
      LOG_DEBUG("Processing UNSUBACK packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      UnsubAckPacket* unsuback = reinterpret_cast<UnsubAckPacket*>(packet);
      ret = handle_unsuback(unsuback);
      unsuback->~UnsubAckPacket();
      allocator_->deallocate(packet, sizeof(UnsubAckPacket));
      break;
    }
    case PacketType::PINGREQ: {
      LOG_DEBUG("Processing PINGREQ packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      PingReqPacket* pingreq = reinterpret_cast<PingReqPacket*>(packet);
      ret = handle_pingreq();
      pingreq->~PingReqPacket();
      allocator_->deallocate(packet, sizeof(PingReqPacket));
      break;
    }
    case PacketType::PINGRESP: {
      LOG_DEBUG("Processing PINGRESP packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      PingRespPacket* pingresp = reinterpret_cast<PingRespPacket*>(packet);
      ret = handle_pingresp();
      pingresp->~PingRespPacket();
      allocator_->deallocate(packet, sizeof(PingRespPacket));
      break;
    }
    case PacketType::DISCONNECT: {
      LOG_DEBUG("Processing DISCONNECT packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      DisconnectPacket* disconnect = reinterpret_cast<DisconnectPacket*>(packet);
      ret = handle_disconnect(disconnect);
      disconnect->~DisconnectPacket();
      allocator_->deallocate(packet, sizeof(DisconnectPacket));
      break;
    }
    case PacketType::AUTH: {
      LOG_DEBUG("Processing AUTH packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
      AuthPacket* auth = reinterpret_cast<AuthPacket*>(packet);
      ret = handle_auth(auth);
      auth->~AuthPacket();
      allocator_->deallocate(packet, sizeof(AuthPacket));
      break;
    }
    default:
      LOG_WARN("Unsupported packet type: 0x{:02x} from client {}:{}",
               static_cast<uint8_t>(packet->type), from_mqtt_string(client_ip_), client_port_);
      packet->~Packet();
      allocator_->deallocate(packet, sizeof(Packet));
      ret = MQ_ERR_PACKET_TYPE;
      break;
  }

  if (ret != 0) {
    LOG_ERROR("Failed to handle packet type: 0x{:02x} from client {}:{}, error: {}", 
              static_cast<uint8_t>(packet->type), from_mqtt_string(client_ip_), client_port_, ret);
  } else {
    LOG_DEBUG("Successfully handled packet type: 0x{:02x} from client {}:{}", 
              static_cast<uint8_t>(packet->type), from_mqtt_string(client_ip_), client_port_);
  }

  // Reset state machine for next packet
  state_ = ReadState::READ_HEADER;
  bytes_needed_ = 1;
  bytes_read_ = 0;
  remaining_length_ = 0;
  header_size_ = 0;

  return ret;
}

int MQTTProtocolHandler::handle_connect(const ConnectPacket* packet)
{
  LOG_DEBUG("Processing CONNECT from client {}:{} (protocol: v{}, client_id: {})", 
            from_mqtt_string(client_ip_), client_port_, packet->protocol_version, from_mqtt_string(packet->client_id));

  // Check protocol version
  if (packet->protocol_version != 5) {
    LOG_ERROR("Unsupported protocol version: {} from client {}:{}", 
              packet->protocol_version, from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_CONNECT_PROTOCOL;
  }

  // Check if client ID is valid
  if (packet->client_id.empty()) {
    LOG_ERROR("Empty client ID from client {}:{}", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_CONNECT_CLIENT_ID;
  }

  // Update session state - 使用拷贝构造
  client_id_ = MQTTString(packet->client_id.begin(), packet->client_id.end(), 
                          MQTTSTLAllocator<char>(allocator_));
  connected_ = true;

  LOG_DEBUG("Client {}:{} connected successfully with client_id: {}", 
            from_mqtt_string(client_ip_), client_port_, from_mqtt_string(client_id_));

  // Send CONNACK
  return send_connack(ReasonCode::Success, true);
}

int MQTTProtocolHandler::handle_connack(const ConnAckPacket* packet)
{
  if (!connected_) {
    LOG_ERROR("Received CONNACK but not connected");
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  if (packet->reason_code != ReasonCode::Success) {
    LOG_ERROR("Connection refused with reason code: 0x{:02x}",
              static_cast<uint8_t>(packet->reason_code));
    return MQ_ERR_CONNECT;
  }

  // 处理会话属性
  session_properties_ = packet->properties;
  session_expiry_interval_ = packet->properties.session_expiry_interval;
  receive_maximum_ = packet->properties.receive_maximum;
  maximum_packet_size_ = packet->properties.maximum_packet_size;
  topic_alias_maximum_ = packet->properties.topic_alias_maximum;
  request_response_information_ = packet->properties.request_response_information;
  request_problem_information_ = packet->properties.request_problem_information;

  return MQ_SUCCESS;
}

int MQTTProtocolHandler::handle_publish(const PublishPacket* packet)
{
  LOG_DEBUG("Processing PUBLISH from client {}:{} (topic: {}, qos: {}, retain: {})", 
            from_mqtt_string(client_ip_), client_port_, from_mqtt_string(packet->topic_name), packet->qos, packet->retain);

  if (!connected_) {
    LOG_ERROR("Client {}:{} not connected", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // Check if topic is valid
  if (packet->topic_name.empty()) {
    LOG_ERROR("Empty topic name from client {}:{}", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_PUBLISH_TOPIC;
  }

  // Check QoS level
  if (packet->qos > 2) {
    LOG_ERROR("Invalid QoS level: {} from client {}:{}", packet->qos, from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_PACKET_QOS;
  }

  LOG_DEBUG("Successfully processed PUBLISH from client {}:{} (payload size: {})", 
            from_mqtt_string(client_ip_), client_port_, packet->payload.size());

  // Send PUBACK for QoS 1
  if (packet->qos == 1) {
    return send_puback(packet->packet_id, ReasonCode::Success);
  }

  return MQ_SUCCESS;
}

int MQTTProtocolHandler::handle_puback(const PubAckPacket* packet)
{
  if (!connected_) {
    LOG_ERROR("Received PUBACK but not connected");
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // 处理PUBACK响应
  if (packet->reason_code != ReasonCode::Success) {
    LOG_WARN("PUBACK with reason code: 0x{:02x}", static_cast<uint8_t>(packet->reason_code));
  }

  return MQ_SUCCESS;
}

int MQTTProtocolHandler::handle_pubrec(const PubRecPacket* packet)
{
  if (!connected_) {
    LOG_ERROR("Received PUBREC but not connected");
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // 处理PUBREC响应
  if (packet->reason_code != ReasonCode::Success) {
    LOG_WARN("PUBREC with reason code: 0x{:02x}", static_cast<uint8_t>(packet->reason_code));
  }

  // 发送PUBREL
  return send_pubrel(packet->packet_id, ReasonCode::Success);
}

int MQTTProtocolHandler::handle_pubrel(const PubRelPacket* packet)
{
  if (!connected_) {
    LOG_ERROR("Received PUBREL but not connected");
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // 处理PUBREL响应
  if (packet->reason_code != ReasonCode::Success) {
    LOG_WARN("PUBREL with reason code: 0x{:02x}", static_cast<uint8_t>(packet->reason_code));
  }

  // 发送PUBCOMP
  return send_pubcomp(packet->packet_id, ReasonCode::Success);
}

int MQTTProtocolHandler::handle_pubcomp(const PubCompPacket* packet)
{
  if (!connected_) {
    LOG_ERROR("Received PUBCOMP but not connected");
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // 处理PUBCOMP响应
  if (packet->reason_code != ReasonCode::Success) {
    LOG_WARN("PUBCOMP with reason code: 0x{:02x}", static_cast<uint8_t>(packet->reason_code));
  }

  return MQ_SUCCESS;
}

int MQTTProtocolHandler::handle_suback(const SubAckPacket* packet)
{
  if (!connected_) {
    LOG_ERROR("Received SUBACK but not connected");
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // 处理SUBACK响应
  for (size_t i = 0; i < packet->reason_codes.size(); ++i) {
    if (packet->reason_codes[i] != ReasonCode::Success) {
      LOG_WARN("Subscription {} failed with reason code: 0x{:02x}", i,
               static_cast<uint8_t>(packet->reason_codes[i]));
    }
  }

  return MQ_SUCCESS;
}

int MQTTProtocolHandler::handle_unsubscribe(const UnsubscribePacket* packet)
{
  if (!connected_) {
    LOG_ERROR("Received UNSUBSCRIBE but not connected");
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // 处理取消订阅
  std::vector<ReasonCode> reason_codes;
  for (const MQTTString& topic : packet->topic_filters) {
    remove_subscription(topic);
    reason_codes.push_back(ReasonCode::Success);
  }

  // 发送UNSUBACK
  return send_unsuback(packet->packet_id, reason_codes);
}

int MQTTProtocolHandler::handle_unsuback(const UnsubAckPacket* packet)
{
  if (!connected_) {
    LOG_ERROR("Received UNSUBACK but not connected");
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // 处理UNSUBACK响应
  for (size_t i = 0; i < packet->reason_codes.size(); ++i) {
    if (packet->reason_codes[i] != ReasonCode::Success) {
      LOG_WARN("Unsubscribe {} failed with reason code: 0x{:02x}", i,
               static_cast<uint8_t>(packet->reason_codes[i]));
    }
  }

  return MQ_SUCCESS;
}

int MQTTProtocolHandler::handle_pingreq()
{
  LOG_DEBUG("Processing PINGREQ from client {}:{}", from_mqtt_string(client_ip_), client_port_);

  if (!connected_) {
    LOG_ERROR("Received PINGREQ but client {}:{} not connected", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  LOG_DEBUG("Sending PINGRESP to client {}:{}", from_mqtt_string(client_ip_), client_port_);
  return send_pingresp();
}

int MQTTProtocolHandler::handle_pingresp()
{
  if (!connected_) {
    LOG_ERROR("Received PINGRESP but not connected");
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  return MQ_SUCCESS;
}

int MQTTProtocolHandler::handle_disconnect(const DisconnectPacket* packet)
{
  LOG_DEBUG("Processing DISCONNECT from client {}:{} (reason: 0x{:02x})", 
            from_mqtt_string(client_ip_), client_port_, static_cast<uint8_t>(packet->reason_code));

  if (!connected_) {
    LOG_ERROR("Received DISCONNECT but client {}:{} not connected", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // 处理断开连接
  connected_ = false;
  LOG_INFO("Client {}:{} disconnected with reason code: 0x{:02x}", 
           from_mqtt_string(client_ip_), client_port_, static_cast<uint8_t>(packet->reason_code));

  // 关闭socket连接
  if (socket_) {
    LOG_DEBUG("Closing socket for client {}:{}", from_mqtt_string(client_ip_), client_port_);
    socket_->close();
    socket_ = nullptr;
  }

  return MQ_SUCCESS;
}

int MQTTProtocolHandler::handle_auth(const AuthPacket* packet)
{
  if (!connected_) {
    LOG_ERROR("Received AUTH but not connected");
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // 处理认证
  if (packet->reason_code != ReasonCode::Success) {
    LOG_WARN("Authentication failed with reason code: 0x{:02x}",
             static_cast<uint8_t>(packet->reason_code));
    return MQ_ERR_CONNECT_CREDENTIALS;
  }

  return MQ_SUCCESS;
}

int MQTTProtocolHandler::handle_subscribe(const SubscribePacket* packet)
{
  LOG_DEBUG("Processing SUBSCRIBE from client {}:{} (packet_id: {})", 
            from_mqtt_string(client_ip_), client_port_, packet->packet_id);

  if (!packet) {
    LOG_ERROR("Invalid SUBSCRIBE packet from client {}:{}", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_PACKET_INVALID;
  }

  // 处理订阅请求
  std::vector<ReasonCode> reason_codes;
  for (const std::pair<MQTTString, uint8_t>& subscription : packet->subscriptions) {
    const MQTTString& topic = subscription.first;
    uint8_t qos = subscription.second;

    LOG_DEBUG("Client {}:{} subscribing to topic: {} with QoS: {}", 
              from_mqtt_string(client_ip_), client_port_, from_mqtt_string(topic), qos);

    // 验证QoS级别
    if (qos > 2) {
      LOG_WARN("Unsupported QoS level {} for topic {} from client {}:{}", 
               qos, from_mqtt_string(topic), from_mqtt_string(client_ip_), client_port_);
      reason_codes.push_back(ReasonCode::QoSNotSupported);
      continue;
    }

    // 添加订阅
    add_subscription(topic);
    reason_codes.push_back(static_cast<ReasonCode>(qos));
    LOG_DEBUG("Successfully subscribed client {}:{} to topic: {} with QoS: {}", 
              from_mqtt_string(client_ip_), client_port_, from_mqtt_string(topic), qos);
  }

  // 发送SUBACK响应
  return send_suback(packet->packet_id, reason_codes);
}

int MQTTProtocolHandler::send_connack(ReasonCode reason_code, bool session_present)
{
  LOG_DEBUG("Sending CONNACK to client {}:{} (reason: 0x{:02x}, session_present: {})", 
            from_mqtt_string(client_ip_), client_port_, static_cast<uint8_t>(reason_code), session_present);

  if (!socket_) {
    LOG_ERROR("Cannot send CONNACK: socket is null for client {}:{}", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_SOCKET;
  }

  // 创建CONNACK包
  ConnAckPacket* packet = new (allocator_->allocate(sizeof(ConnAckPacket))) ConnAckPacket();
  if (!packet) {
    LOG_ERROR("Failed to allocate CONNACK packet for client {}:{}", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::CONNACK;
  packet->reason_code = reason_code;
  packet->session_present = session_present;

  // 序列化包
  std::vector<uint8_t> buffer;
  int ret = parser_->serialize_connack(packet, buffer);
  if (ret != 0) {
    LOG_ERROR("Failed to serialize CONNACK packet for client {}:{}, error: {}", 
              from_mqtt_string(client_ip_), client_port_, ret);
    allocator_->deallocate(packet, sizeof(ConnAckPacket));
    return ret;
  }

  // 发送数据
  ret = socket_->send(buffer.data(), static_cast<int>(buffer.size()));
  if (ret != 0) {
    LOG_ERROR("Failed to send CONNACK packet to client {}:{}, error: {}", 
              from_mqtt_string(client_ip_), client_port_, ret);
  } else {
    LOG_DEBUG("Successfully sent CONNACK packet to client {}:{} (size: {})", 
              from_mqtt_string(client_ip_), client_port_, buffer.size());
  }

  allocator_->deallocate(packet, sizeof(ConnAckPacket));
  return ret;
}

int MQTTProtocolHandler::send_pubrec(uint16_t packet_id, ReasonCode reason_code)
{
  if (!socket_) {
    return MQ_ERR_SOCKET;
  }

  // 创建PUBREC包
  PubRecPacket* packet = new (allocator_->allocate(sizeof(PubRecPacket))) PubRecPacket();
  if (!packet) {
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::PUBREC;
  packet->packet_id = packet_id;
  packet->reason_code = reason_code;

  // 序列化包
  std::vector<uint8_t> buffer;
  int ret = parser_->serialize_pubrec(packet, buffer);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(PubRecPacket));
    return ret;
  }

  // 发送数据
  ret = socket_->send(buffer.data(), static_cast<int>(buffer.size()));
  allocator_->deallocate(packet, sizeof(PubRecPacket));
  return ret;
}

int MQTTProtocolHandler::send_pubrel(uint16_t packet_id, ReasonCode reason_code)
{
  if (!socket_) {
    return MQ_ERR_SOCKET;
  }

  // 创建PUBREL包
  PubRelPacket* packet = new (allocator_->allocate(sizeof(PubRelPacket))) PubRelPacket();
  if (!packet) {
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::PUBREL;
  packet->packet_id = packet_id;
  packet->reason_code = reason_code;

  // 序列化包
  std::vector<uint8_t> buffer;
  int ret = parser_->serialize_pubrel(packet, buffer);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(PubRelPacket));
    return ret;
  }

  // 发送数据
  ret = socket_->send(buffer.data(), static_cast<int>(buffer.size()));
  allocator_->deallocate(packet, sizeof(PubRelPacket));
  return ret;
}

int MQTTProtocolHandler::send_pubcomp(uint16_t packet_id, ReasonCode reason_code)
{
  if (!socket_) {
    return MQ_ERR_SOCKET;
  }

  // 创建PUBCOMP包
  PubCompPacket* packet = new (allocator_->allocate(sizeof(PubCompPacket))) PubCompPacket();
  if (!packet) {
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::PUBCOMP;
  packet->packet_id = packet_id;
  packet->reason_code = reason_code;

  // 序列化包
  std::vector<uint8_t> buffer;
  int ret = parser_->serialize_pubcomp(packet, buffer);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(PubCompPacket));
    return ret;
  }

  // 发送数据
  ret = socket_->send(buffer.data(), static_cast<int>(buffer.size()));
  allocator_->deallocate(packet, sizeof(PubCompPacket));
  return ret;
}

int MQTTProtocolHandler::send_unsuback(uint16_t packet_id,
                                       const std::vector<ReasonCode>& reason_codes)
{
  if (!socket_) {
    return MQ_ERR_SOCKET;
  }

  // 创建UNSUBACK包
  UnsubAckPacket* packet = new (allocator_->allocate(sizeof(UnsubAckPacket))) UnsubAckPacket(allocator_);
  if (!packet) {
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::UNSUBACK;
  packet->packet_id = packet_id;
  // 转换std::vector到MQTTVector
  for (const ReasonCode& code : reason_codes) {
    packet->reason_codes.push_back(code);
  }

  // 序列化包
  std::vector<uint8_t> buffer;
  int ret = parser_->serialize_unsuback(packet, buffer);
  if (ret != 0) {
    packet->~UnsubAckPacket();
    allocator_->deallocate(packet, sizeof(UnsubAckPacket));
    return ret;
  }

  // 发送数据
  ret = socket_->send(buffer.data(), static_cast<int>(buffer.size()));
  packet->~UnsubAckPacket();
  allocator_->deallocate(packet, sizeof(UnsubAckPacket));
  return ret;
}

int MQTTProtocolHandler::send_disconnect(ReasonCode reason_code)
{
  if (!socket_) {
    return MQ_ERR_SOCKET;
  }

  // 创建DISCONNECT包
  DisconnectPacket* packet =
      new (allocator_->allocate(sizeof(DisconnectPacket))) DisconnectPacket();
  if (!packet) {
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::DISCONNECT;
  packet->reason_code = reason_code;

  // 序列化包
  std::vector<uint8_t> buffer;
  int ret = parser_->serialize_disconnect(packet, buffer);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(DisconnectPacket));
    return ret;
  }

  // 发送数据
  ret = socket_->send(buffer.data(), static_cast<int>(buffer.size()));
  allocator_->deallocate(packet, sizeof(DisconnectPacket));
  return ret;
}

int MQTTProtocolHandler::send_auth(ReasonCode reason_code)
{
  if (!socket_) {
    return MQ_ERR_SOCKET;
  }

  // 创建AUTH包
  AuthPacket* packet = new (allocator_->allocate(sizeof(AuthPacket))) AuthPacket();
  if (!packet) {
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::AUTH;
  packet->reason_code = reason_code;

  // 序列化包
  std::vector<uint8_t> buffer;
  int ret = parser_->serialize_auth(packet, buffer);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(AuthPacket));
    return ret;
  }

  // 发送数据
  ret = socket_->send(buffer.data(), static_cast<int>(buffer.size()));
  allocator_->deallocate(packet, sizeof(AuthPacket));
  return ret;
}

void MQTTProtocolHandler::add_subscription(const MQTTString& topic)
{
  subscriptions_.push_back(topic);
}

void MQTTProtocolHandler::remove_subscription(const MQTTString& topic)
{
  subscriptions_.erase(std::remove(subscriptions_.begin(), subscriptions_.end(), topic),
                       subscriptions_.end());
}

int MQTTProtocolHandler::send_suback(uint16_t packet_id,
                                     const std::vector<ReasonCode>& reason_codes)
{
  if (!socket_) {
    return MQ_ERR_SOCKET;
  }

  // 创建SUBACK包
  SubAckPacket* packet = new (allocator_->allocate(sizeof(SubAckPacket))) SubAckPacket(allocator_);
  if (!packet) {
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::SUBACK;
  packet->packet_id = packet_id;
  // 转换std::vector到MQTTVector
  for (const ReasonCode& code : reason_codes) {
    packet->reason_codes.push_back(code);
  }

  // 序列化包
  std::vector<uint8_t> buffer;
  int ret = parser_->serialize_suback(packet, buffer);
  if (ret != 0) {
    packet->~SubAckPacket();
    allocator_->deallocate(packet, sizeof(SubAckPacket));
    return ret;
  }

  // 发送数据
  ret = socket_->send(buffer.data(), static_cast<int>(buffer.size()));
  packet->~SubAckPacket();
  allocator_->deallocate(packet, sizeof(SubAckPacket));
  return ret;
}

int MQTTProtocolHandler::send_puback(uint16_t packet_id, ReasonCode reason_code)
{
  LOG_DEBUG("Sending PUBACK to client {}:{} (packet_id: {}, reason: 0x{:02x})", 
            from_mqtt_string(client_ip_), client_port_, packet_id, static_cast<uint8_t>(reason_code));

  if (!socket_) {
    LOG_ERROR("Cannot send PUBACK: socket is null for client {}:{}", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_SOCKET;
  }

  // 创建PUBACK包
  PubAckPacket* packet = new (allocator_->allocate(sizeof(PubAckPacket))) PubAckPacket();
  if (!packet) {
    LOG_ERROR("Failed to allocate PUBACK packet for client {}:{}", from_mqtt_string(client_ip_), client_port_);
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::PUBACK;
  packet->packet_id = packet_id;
  packet->reason_code = reason_code;

  // 序列化包
  std::vector<uint8_t> buffer;
  int ret = parser_->serialize_puback(packet, buffer);
  if (ret != 0) {
    LOG_ERROR("Failed to serialize PUBACK packet for client {}:{}, error: {}", 
              from_mqtt_string(client_ip_), client_port_, ret);
    allocator_->deallocate(packet, sizeof(PubAckPacket));
    return ret;
  }

  // 发送数据
  ret = socket_->send(buffer.data(), static_cast<int>(buffer.size()));
  if (ret != 0) {
    LOG_ERROR("Failed to send PUBACK packet to client {}:{}, error: {}", 
              from_mqtt_string(client_ip_), client_port_, ret);
  } else {
    LOG_DEBUG("Successfully sent PUBACK packet to client {}:{} (size: {})", 
              from_mqtt_string(client_ip_), client_port_, buffer.size());
  }

  allocator_->deallocate(packet, sizeof(PubAckPacket));
  return ret;
}

int MQTTProtocolHandler::send_pingresp()
{
  if (!socket_) {
    return MQ_ERR_SOCKET;
  }

  // 创建PINGRESP包
  PingRespPacket* packet = new (allocator_->allocate(sizeof(PingRespPacket))) PingRespPacket();
  if (!packet) {
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::PINGRESP;

  // 序列化包
  std::vector<uint8_t> buffer;
  int ret = parser_->serialize_pingresp(packet, buffer);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(PingRespPacket));
    return ret;
  }

  // 发送数据
  ret = socket_->send(buffer.data(), static_cast<int>(buffer.size()));
  allocator_->deallocate(packet, sizeof(PingRespPacket));
  return ret;
}

}  // namespace mqtt