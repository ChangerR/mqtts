#include "mqtt_protocol_handler.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include "co_routine.h"
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_coroutine_utils.h"
#include "mqtt_memory_tags.h"
#include "mqtt_parser.h"
#include "mqtt_session_manager_v2.h"
#include "mqtt_socket.h"
namespace mqtt {

static const size_t INITIAL_BUFFER_SIZE = 1024;
static const size_t MAX_BUFFER_SIZE = 1024 * 1024;  // 1MB max size

MQTTProtocolHandler::MQTTProtocolHandler(MQTTAllocator* allocator)
    : buffer_(nullptr),
      current_buffer_size_(0),
      bytes_read_(0),
      bytes_needed_(1),
      state_(ReadState::READ_HEADER),
      packet_type_(0),
      remaining_length_(0),
      header_size_(0),
      socket_(nullptr),
      client_ip_(MQTTStrAllocator(allocator)),
      client_port_(0),
      client_id_(MQTTStrAllocator(allocator)),
      connected_(false),
      next_packet_id_(1),
      subscriptions_(MQTTStrAllocator(allocator)),
      allocator_(allocator),
      parser_(nullptr),
      session_expiry_interval_(0),
      receive_maximum_(65535),
      maximum_packet_size_(268435455),
      topic_alias_maximum_(0),
      request_response_information_(false),
      request_problem_information_(true),
      serialize_buffer_(nullptr),
      write_lock_acquired_(false),
      write_lock_condition_(),
      session_manager_(nullptr)
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

  // Create serialize buffer
  serialize_buffer_ =
      new (allocator_->allocate(sizeof(MQTTSerializeBuffer))) MQTTSerializeBuffer(allocator_);
}

MQTTProtocolHandler::~MQTTProtocolHandler()
{
  // Final cleanup - unregister from session manager if still connected (failsafe)
  cleanup_session_registration("in destructor");

  if (buffer_) {
    allocator_->deallocate(buffer_, current_buffer_size_);
  }
  if (parser_) {
    parser_->~MQTTParser();
    allocator_->deallocate(parser_, sizeof(MQTTParser));
  }
  if (serialize_buffer_) {
    serialize_buffer_->~MQTTSerializeBuffer();
    allocator_->deallocate(serialize_buffer_, sizeof(MQTTSerializeBuffer));
  }
}

int MQTTProtocolHandler::init(MQTTSocket* socket, const std::string& client_ip, int client_port)
{
  int ret = MQ_SUCCESS;

  if (MQ_ISNULL(socket)) {
    LOG_ERROR("Socket cannot be null");
    ret = MQ_ERR_PARAM_V2;
  } else if (client_ip.empty()) {
    LOG_ERROR("Client IP cannot be empty");
    ret = MQ_ERR_PARAM_V2;
  } else if (client_port <= 0 || client_port > 65535) {
    LOG_ERROR("Client port {} is invalid", client_port);
    ret = MQ_ERR_PARAM_V2;
  } else {
    try {
      socket_ = socket;
      client_ip_ = to_mqtt_string(client_ip, allocator_);
      client_port_ = client_port;
    } catch (const std::exception& e) {
      LOG_ERROR("Failed to initialize protocol handler: {}", e.what());
      ret = MQ_ERR_INTERNAL;
    }
  }

  return ret;
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
      LOG_ERROR("Failed to read packet from client {}:{}, error: {}", client_ip_.c_str(),
                client_port_, mqtt_error_string(ret));
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

  // Cleanup session registration when process() exits (for any reason: socket disconnect, error,
  // etc.)
  cleanup_session_registration("on process exit");

  return ret;
}

int MQTTProtocolHandler::ensure_buffer_size(size_t needed_size)
{
  if (bytes_read_ + needed_size > current_buffer_size_) {
    size_t new_size = std::min(current_buffer_size_ * 2, MAX_BUFFER_SIZE);
    if (bytes_read_ + needed_size > new_size) {
      LOG_ERROR("Packet too large from client {}:{} (needed: {}, max: {})", client_ip_.c_str(),
                client_port_, bytes_read_ + needed_size, MAX_BUFFER_SIZE);
      return MQ_ERR_PACKET_TOO_LARGE;
    }

    // Allocate new buffer
    char* new_buffer = (char*)allocator_->allocate(new_size);
    if (!new_buffer) {
      LOG_ERROR("Failed to resize buffer for client {}:{} (new size: {})", client_ip_.c_str(),
                client_port_, new_size);
      return MQ_ERR_MEMORY_ALLOC;
    }

    // Copy existing data
    memcpy(new_buffer, buffer_, bytes_read_);

    // Free old buffer and update pointer
    allocator_->deallocate(buffer_, current_buffer_size_);
    buffer_ = new_buffer;
    current_buffer_size_ = new_size;

    LOG_DEBUG("Buffer resized to {} bytes for client {}:{}", new_size, client_ip_.c_str(),
              client_port_);
  }
  return MQ_SUCCESS;
}

int MQTTProtocolHandler::read_packet()
{
  // Use co_poll to wait for read event
  struct pollfd pf = {0};
  // Check if socket is still valid
  if (!socket_->is_connected()) {
    LOG_WARN("Client {}:{} disconnected", client_ip_.c_str(), client_port_);
    return MQ_ERR_SOCKET;
  }

  pf.fd = socket_->get_fd();
  pf.events = (POLLIN | POLLERR | POLLHUP);
  co_poll(co_get_epoll_ct(), &pf, 1, 1000);  // 100ms timeout

  // Try to receive data
  int len = bytes_needed_;
  int ret = socket_->recv(buffer_ + bytes_read_, len);
  if (MQ_FAIL(ret)) {
    LOG_WARN("Client {}:{} disconnected unexpectedly", client_ip_.c_str(), client_port_);
    return MQ_ERR_SOCKET_RECV;
  }

  // Check if connection closed
  if (len == 0) {
    LOG_WARN("Client {}:{} closed connection", client_ip_.c_str(), client_port_);
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
            LOG_ERROR("Packet too large from client {}:{} (size: {}, max: {})", client_ip_.c_str(),
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
          LOG_ERROR("Invalid remaining length from client {}:{}", client_ip_.c_str(), client_port_);
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
  LOG_DEBUG("Parsing packet from client {}:{}, type: 0x{:02x}, size: {}", client_ip_.c_str(),
            client_port_, packet_type_, bytes_read_);

  int ret = parser_->parse_packet(reinterpret_cast<const uint8_t*>(buffer_), bytes_read_, &packet);
  if (ret != 0) {
    LOG_ERROR("Failed to parse MQTT packet from client {}:{}, {}, error: {}", client_ip_.c_str(),
              client_port_, bytes_read_, mqtt_error_string(ret));
    return MQ_ERR_PACKET_INVALID;
  }

  LOG_DEBUG("Successfully parsed packet type: 0x{:02x} from client {}:{}",
            static_cast<uint8_t>(packet->type), client_ip_.c_str(), client_port_);

  // Handle packet based on type
  switch (packet->type) {
    case PacketType::CONNECT: {
      LOG_DEBUG("Processing CONNECT packet from client {}:{}", client_ip_.c_str(), client_port_);
      ConnectPacket* connect = reinterpret_cast<ConnectPacket*>(packet);
      ret = handle_connect(connect);
      connect->~ConnectPacket();
      allocator_->deallocate(packet, sizeof(ConnectPacket));
      break;
    }
    case PacketType::CONNACK: {
      LOG_DEBUG("Processing CONNACK packet from client {}:{}", client_ip_.c_str(), client_port_);
      ConnAckPacket* connack = reinterpret_cast<ConnAckPacket*>(packet);
      ret = handle_connack(connack);
      connack->~ConnAckPacket();
      allocator_->deallocate(packet, sizeof(ConnAckPacket));
      break;
    }
    case PacketType::PUBLISH: {
      LOG_DEBUG("Processing PUBLISH packet from client {}:{}", client_ip_.c_str(), client_port_);
      PublishPacket* publish = reinterpret_cast<PublishPacket*>(packet);
      ret = handle_publish(publish);
      publish->~PublishPacket();
      allocator_->deallocate(packet, sizeof(PublishPacket));
      break;
    }
    case PacketType::PUBACK: {
      LOG_DEBUG("Processing PUBACK packet from client {}:{}", client_ip_.c_str(), client_port_);
      PubAckPacket* puback = reinterpret_cast<PubAckPacket*>(packet);
      ret = handle_puback(puback);
      puback->~PubAckPacket();
      allocator_->deallocate(packet, sizeof(PubAckPacket));
      break;
    }
    case PacketType::PUBREC: {
      LOG_DEBUG("Processing PUBREC packet from client {}:{}", client_ip_.c_str(), client_port_);
      PubRecPacket* pubrec = reinterpret_cast<PubRecPacket*>(packet);
      ret = handle_pubrec(pubrec);
      pubrec->~PubRecPacket();
      allocator_->deallocate(packet, sizeof(PubRecPacket));
      break;
    }
    case PacketType::PUBREL: {
      LOG_DEBUG("Processing PUBREL packet from client {}:{}", client_ip_.c_str(), client_port_);
      PubRelPacket* pubrel = reinterpret_cast<PubRelPacket*>(packet);
      ret = handle_pubrel(pubrel);
      pubrel->~PubRelPacket();
      allocator_->deallocate(packet, sizeof(PubRelPacket));
      break;
    }
    case PacketType::PUBCOMP: {
      LOG_DEBUG("Processing PUBCOMP packet from client {}:{}", client_ip_.c_str(), client_port_);
      PubCompPacket* pubcomp = reinterpret_cast<PubCompPacket*>(packet);
      ret = handle_pubcomp(pubcomp);
      pubcomp->~PubCompPacket();
      allocator_->deallocate(packet, sizeof(PubCompPacket));
      break;
    }
    case PacketType::SUBSCRIBE: {
      LOG_DEBUG("Processing SUBSCRIBE packet from client {}:{}", client_ip_.c_str(), client_port_);
      SubscribePacket* subscribe = reinterpret_cast<SubscribePacket*>(packet);
      ret = handle_subscribe(subscribe);
      subscribe->~SubscribePacket();
      allocator_->deallocate(packet, sizeof(SubscribePacket));
      break;
    }
    case PacketType::SUBACK: {
      LOG_DEBUG("Processing SUBACK packet from client {}:{}", client_ip_.c_str(), client_port_);
      SubAckPacket* suback = reinterpret_cast<SubAckPacket*>(packet);
      ret = handle_suback(suback);
      suback->~SubAckPacket();
      allocator_->deallocate(packet, sizeof(SubAckPacket));
      break;
    }
    case PacketType::UNSUBSCRIBE: {
      LOG_DEBUG("Processing UNSUBSCRIBE packet from client {}:{}", client_ip_.c_str(),
                client_port_);
      UnsubscribePacket* unsubscribe = reinterpret_cast<UnsubscribePacket*>(packet);
      ret = handle_unsubscribe(unsubscribe);
      unsubscribe->~UnsubscribePacket();
      allocator_->deallocate(packet, sizeof(UnsubscribePacket));
      break;
    }
    case PacketType::UNSUBACK: {
      LOG_DEBUG("Processing UNSUBACK packet from client {}:{}", client_ip_.c_str(), client_port_);
      UnsubAckPacket* unsuback = reinterpret_cast<UnsubAckPacket*>(packet);
      ret = handle_unsuback(unsuback);
      unsuback->~UnsubAckPacket();
      allocator_->deallocate(packet, sizeof(UnsubAckPacket));
      break;
    }
    case PacketType::PINGREQ: {
      LOG_DEBUG("Processing PINGREQ packet from client {}:{}", client_ip_.c_str(), client_port_);
      PingReqPacket* pingreq = reinterpret_cast<PingReqPacket*>(packet);
      ret = handle_pingreq();
      pingreq->~PingReqPacket();
      allocator_->deallocate(packet, sizeof(PingReqPacket));
      break;
    }
    case PacketType::PINGRESP: {
      LOG_DEBUG("Processing PINGRESP packet from client {}:{}", client_ip_.c_str(), client_port_);
      PingRespPacket* pingresp = reinterpret_cast<PingRespPacket*>(packet);
      ret = handle_pingresp();
      pingresp->~PingRespPacket();
      allocator_->deallocate(packet, sizeof(PingRespPacket));
      break;
    }
    case PacketType::DISCONNECT: {
      LOG_DEBUG("Processing DISCONNECT packet from client {}:{}", client_ip_.c_str(), client_port_);
      DisconnectPacket* disconnect = reinterpret_cast<DisconnectPacket*>(packet);
      ret = handle_disconnect(disconnect);
      disconnect->~DisconnectPacket();
      allocator_->deallocate(packet, sizeof(DisconnectPacket));
      break;
    }
    case PacketType::AUTH: {
      LOG_DEBUG("Processing AUTH packet from client {}:{}", client_ip_.c_str(), client_port_);
      AuthPacket* auth = reinterpret_cast<AuthPacket*>(packet);
      ret = handle_auth(auth);
      auth->~AuthPacket();
      allocator_->deallocate(packet, sizeof(AuthPacket));
      break;
    }
    default:
      LOG_WARN("Unsupported packet type: 0x{:02x} from client {}:{}",
               static_cast<uint8_t>(packet->type), client_ip_.c_str(), client_port_);
      packet->~Packet();
      allocator_->deallocate(packet, sizeof(Packet));
      ret = MQ_ERR_PACKET_TYPE;
      break;
  }

  if (ret != 0) {
    LOG_ERROR("Failed to handle packet type: 0x{:02x} from client {}:{}, error: {}",
              static_cast<uint8_t>(packet_type_), client_ip_.c_str(), client_port_, ret);
  } else {
    LOG_DEBUG("Successfully handled packet type: 0x{:02x} from client {}:{}",
              static_cast<uint8_t>(packet_type_), client_ip_.c_str(), client_port_);
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
            client_ip_.c_str(), client_port_, packet->protocol_version,
            from_mqtt_string(packet->client_id));

  // Check protocol version
  if (packet->protocol_version != 5) {
    LOG_ERROR("Unsupported protocol version: {} from client {}:{}", packet->protocol_version,
              client_ip_.c_str(), client_port_);
    return MQ_ERR_CONNECT_PROTOCOL;
  }

  // Check if client ID is valid
  if (packet->client_id.empty()) {
    LOG_ERROR("Empty client ID from client {}:{}", client_ip_.c_str(), client_port_);
    return MQ_ERR_CONNECT_CLIENT_ID;
  }

  // Update session state - 使用拷贝构造
  client_id_ =
      MQTTString(packet->client_id.begin(), packet->client_id.end(), MQTTStrAllocator(allocator_));
  connected_ = true;

  LOG_DEBUG("Client {}:{} connected successfully with client_id: {}", client_ip_.c_str(),
            client_port_, from_mqtt_string(client_id_));

  // Register this handler with the session manager
  int ret = register_session_with_manager();
  if (ret != 0) {
    connected_ = false;
    return ret;
  }

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
            client_ip_.c_str(), client_port_, from_mqtt_string(packet->topic_name), packet->qos,
            packet->retain);

  if (!connected_) {
    LOG_ERROR("Client {}:{} not connected", client_ip_.c_str(), client_port_);
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // Check if topic is valid
  if (packet->topic_name.empty()) {
    LOG_ERROR("Empty topic name from client {}:{}", client_ip_.c_str(), client_port_);
    return MQ_ERR_PUBLISH_TOPIC;
  }

  // Check QoS level
  if (packet->qos > 2) {
    LOG_ERROR("Invalid QoS level: {} from client {}:{}", packet->qos, client_ip_.c_str(),
              client_port_);
    return MQ_ERR_PACKET_QOS;
  }

  LOG_DEBUG("Successfully processed PUBLISH from client {}:{} (payload size: {})",
            client_ip_.c_str(), client_port_, packet->payload.size());

  // 转发消息给订阅者
  if (session_manager_) {
    int ret = session_manager_->forward_publish_by_topic(packet->topic_name, *packet, client_id_);
    if (ret != MQ_SUCCESS) {
      LOG_WARN("Failed to forward PUBLISH message to subscribers for topic: {}, error: {}", 
               from_mqtt_string(packet->topic_name), ret);
    } else {
      LOG_DEBUG("Successfully forwarded PUBLISH message to subscribers for topic: {}", 
                from_mqtt_string(packet->topic_name));
    }
  } else {
    LOG_WARN("Session manager not available, cannot forward PUBLISH message for topic: {}", 
             from_mqtt_string(packet->topic_name));
  }

  // Send PUBACK for QoS 1
  if (packet->qos == 1) {
    return send_puback(packet->packet_id, ReasonCode::Success);
  }
  
  // Send PUBREC for QoS 2
  if (packet->qos == 2) {
    return send_pubrec(packet->packet_id, ReasonCode::Success);
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
  LOG_DEBUG("Processing PINGREQ from client {}:{}", client_ip_.c_str(), client_port_);

  if (!connected_) {
    LOG_ERROR("Received PINGREQ but client {}:{} not connected", client_ip_.c_str(), client_port_);
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  LOG_DEBUG("Sending PINGRESP to client {}:{}", client_ip_.c_str(), client_port_);
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
  LOG_DEBUG("Processing DISCONNECT from client {}:{} (reason: 0x{:02x})", client_ip_.c_str(),
            client_port_, static_cast<uint8_t>(packet->reason_code));

  if (!connected_) {
    LOG_ERROR("Received DISCONNECT but client {}:{} not connected", client_ip_.c_str(),
              client_port_);
    return MQ_ERR_SESSION_NOT_CONNECTED;
  }

  // Unregister from session manager before marking as disconnected
  cleanup_session_registration("on DISCONNECT packet");
  LOG_INFO("Client {}:{} disconnected with reason code: 0x{:02x}", client_ip_.c_str(), client_port_,
           static_cast<uint8_t>(packet->reason_code));

  // 关闭socket连接
  if (socket_) {
    LOG_DEBUG("Closing socket for client {}:{}", client_ip_.c_str(), client_port_);
    socket_->close();
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
  LOG_DEBUG("Processing SUBSCRIBE from client {}:{} (packet_id: {})", client_ip_.c_str(),
            client_port_, packet->packet_id);

  if (!packet) {
    LOG_ERROR("Invalid SUBSCRIBE packet from client {}:{}", client_ip_.c_str(), client_port_);
    return MQ_ERR_PACKET_INVALID;
  }

  // 处理订阅请求
  std::vector<ReasonCode> reason_codes;
  for (const std::pair<MQTTString, uint8_t>& subscription : packet->subscriptions) {
    const MQTTString& topic = subscription.first;
    uint8_t qos = subscription.second;

    LOG_DEBUG("Client {}:{} subscribing to topic: {} with QoS: {}", client_ip_.c_str(),
              client_port_, from_mqtt_string(topic), qos);

    // 验证QoS级别
    if (qos > 2) {
      LOG_WARN("Unsupported QoS level {} for topic {} from client {}:{}", qos,
               from_mqtt_string(topic), client_ip_.c_str(), client_port_);
      reason_codes.push_back(ReasonCode::QoSNotSupported);
      continue;
    }

    // 添加订阅到本地存储
    add_subscription(topic);
    
    // 注册到全局session manager的topic tree
    if (session_manager_) {
      int ret = session_manager_->subscribe_topic(topic, client_id_, qos);
      if (ret != MQ_SUCCESS) {
        LOG_WARN("Failed to register subscription with session manager for client {}:{}, topic: {}, error: {}", 
                 client_ip_.c_str(), client_port_, from_mqtt_string(topic), ret);
      } else {
        LOG_DEBUG("Successfully registered subscription with session manager for client {}:{}, topic: {}", 
                  client_ip_.c_str(), client_port_, from_mqtt_string(topic));
      }
    }
    
    reason_codes.push_back(static_cast<ReasonCode>(qos));
    LOG_DEBUG("Successfully subscribed client {}:{} to topic: {} with QoS: {}", client_ip_.c_str(),
              client_port_, from_mqtt_string(topic), qos);
  }

  // 发送SUBACK响应
  return send_suback(packet->packet_id, reason_codes);
}

int MQTTProtocolHandler::send_data_with_lock(const char* data, size_t size, int timeout_ms)
{
  if (!socket_ || !data || size == 0) {
    LOG_ERROR("Invalid parameters for send_data_with_lock: socket={}, data={}, size={}",
              socket_ ? "valid" : "null", data ? "valid" : "null", size);
    return MQ_ERR_INVALID_ARGS;
  }

  // 尝试在超时时间内获取写入锁
  auto start_time = std::chrono::steady_clock::now();

  while (true) {
    // 尝试获取锁
    bool expected = false;
    if (write_lock_acquired_.compare_exchange_weak(expected, true)) {
      // 成功获取锁，执行发送操作
      int result = socket_->send(reinterpret_cast<const uint8_t*>(data), static_cast<int>(size));

      // 释放锁并通知等待的协程
      write_lock_acquired_.store(false);
      write_lock_condition_.broadcast();

      if (result != 0) {
        LOG_ERROR("Failed to send data to client {}:{}, error: {}", client_ip_.c_str(),
                  client_port_, result);
      } else {
        LOG_DEBUG("Successfully sent {} bytes to client {}:{}", size, client_ip_.c_str(),
                  client_port_);
      }

      return result;
    }

    // 锁被占用，检查是否超时
    if (timeout_ms > 0) {
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
      int64_t elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();

      if (elapsed >= timeout_ms) {
        LOG_WARN("Failed to acquire write lock within {}ms for client {}:{}", timeout_ms,
                 client_ip_.c_str(), client_port_);
        return MQ_ERR_TIMEOUT;
      }

      // 等待锁释放信号，剩余时间
      int remaining_time = timeout_ms - static_cast<int>(elapsed);
      if (remaining_time > 0) {
        write_lock_condition_.wait(std::min(remaining_time, 100));  // 最多等待100ms
      }
    } else {
      // 无超时限制，等待锁释放信号
      write_lock_condition_.wait(100);  // 每100ms检查一次
    }
  }
}

int MQTTProtocolHandler::send_connack(ReasonCode reason_code, bool session_present)
{
  LOG_DEBUG("Sending CONNACK to client {}:{} (reason: 0x{:02x}, session_present: {})",
            client_ip_.c_str(), client_port_, static_cast<uint8_t>(reason_code), session_present);

  if (!socket_) {
    LOG_ERROR("Cannot send CONNACK: socket is null for client {}:{}", client_ip_.c_str(),
              client_port_);
    return MQ_ERR_SOCKET;
  }

  // 创建CONNACK包
  ConnAckPacket* packet = new (allocator_->allocate(sizeof(ConnAckPacket))) ConnAckPacket();
  if (!packet) {
    LOG_ERROR("Failed to allocate CONNACK packet for client {}:{}", client_ip_.c_str(),
              client_port_);
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::CONNACK;
  packet->reason_code = reason_code;
  packet->session_present = session_present;

  // 序列化包
  int ret = parser_->serialize_connack(packet, *serialize_buffer_);
  if (ret != 0) {
    LOG_ERROR("Failed to serialize CONNACK packet for client {}:{}, error: {}", client_ip_.c_str(),
              client_port_, ret);
    allocator_->deallocate(packet, sizeof(ConnAckPacket));
    return ret;
  }

  // 使用统一的带锁写入函数
  ret = send_data_with_lock(reinterpret_cast<const char*>(serialize_buffer_->data()),
                            serialize_buffer_->size());
  if (ret != 0) {
    LOG_ERROR("Failed to send CONNACK packet to client {}:{}, error: {}", client_ip_.c_str(),
              client_port_, ret);
  } else {
    LOG_DEBUG("Successfully sent CONNACK packet to client {}:{} (size: {})", client_ip_.c_str(),
              client_port_, serialize_buffer_->size());
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
  int ret = parser_->serialize_pubrec(packet, *serialize_buffer_);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(PubRecPacket));
    return ret;
  }

  // 使用统一的带锁写入函数
  ret = send_data_with_lock(reinterpret_cast<const char*>(serialize_buffer_->data()),
                            serialize_buffer_->size());
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
  int ret = parser_->serialize_pubrel(packet, *serialize_buffer_);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(PubRelPacket));
    return ret;
  }

  // 使用统一的带锁写入函数
  ret = send_data_with_lock(reinterpret_cast<const char*>(serialize_buffer_->data()),
                            serialize_buffer_->size());
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
  int ret = parser_->serialize_pubcomp(packet, *serialize_buffer_);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(PubCompPacket));
    return ret;
  }

  // 使用统一的带锁写入函数
  ret = send_data_with_lock(reinterpret_cast<const char*>(serialize_buffer_->data()),
                            serialize_buffer_->size());
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
  UnsubAckPacket* packet =
      new (allocator_->allocate(sizeof(UnsubAckPacket))) UnsubAckPacket(allocator_);
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
  int ret = parser_->serialize_unsuback(packet, *serialize_buffer_);
  if (ret != 0) {
    packet->~UnsubAckPacket();
    allocator_->deallocate(packet, sizeof(UnsubAckPacket));
    return ret;
  }

  // 使用统一的带锁写入函数
  ret = send_data_with_lock(reinterpret_cast<const char*>(serialize_buffer_->data()),
                            serialize_buffer_->size());
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
  int ret = parser_->serialize_disconnect(packet, *serialize_buffer_);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(DisconnectPacket));
    return ret;
  }

  // 使用统一的带锁写入函数
  ret = send_data_with_lock(reinterpret_cast<const char*>(serialize_buffer_->data()),
                            serialize_buffer_->size());
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
  int ret = parser_->serialize_auth(packet, *serialize_buffer_);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(AuthPacket));
    return ret;
  }

  // 使用统一的带锁写入函数
  ret = send_data_with_lock(reinterpret_cast<const char*>(serialize_buffer_->data()),
                            serialize_buffer_->size());
  allocator_->deallocate(packet, sizeof(AuthPacket));
  return ret;
}

int MQTTProtocolHandler::add_subscription(const MQTTString& topic)
{
  int ret = MQ_SUCCESS;

  if (from_mqtt_string(topic).empty()) {
    LOG_ERROR("Topic cannot be empty for subscription");
    ret = MQ_ERR_SUBSCRIBE_TOPIC;
  } else {
    try {
      subscriptions_.push_back(topic);
      LOG_DEBUG("Added subscription for topic: {}", from_mqtt_string(topic));
    } catch (const std::exception& e) {
      LOG_ERROR("Failed to add subscription for topic {}: {}", from_mqtt_string(topic), e.what());
      ret = MQ_ERR_MEMORY_ALLOC;
    }
  }

  return ret;
}

int MQTTProtocolHandler::remove_subscription(const MQTTString& topic)
{
  int ret = MQ_SUCCESS;

  if (from_mqtt_string(topic).empty()) {
    LOG_ERROR("Topic cannot be empty for unsubscription");
    ret = MQ_ERR_SUBSCRIBE_TOPIC;
  } else {
    try {
      auto initial_size = subscriptions_.size();
      subscriptions_.erase(std::remove(subscriptions_.begin(), subscriptions_.end(), topic),
                           subscriptions_.end());
      auto final_size = subscriptions_.size();

      if (initial_size == final_size) {
        LOG_DEBUG("Topic '{}' was not found in subscriptions", from_mqtt_string(topic));
        ret = MQ_ERR_NOT_FOUND_V2;
      } else {
        LOG_DEBUG("Removed {} subscription(s) for topic: {}", initial_size - final_size,
                  from_mqtt_string(topic));
      }
    } catch (const std::exception& e) {
      LOG_ERROR("Failed to remove subscription for topic {}: {}", from_mqtt_string(topic),
                e.what());
      ret = MQ_ERR_INTERNAL;
    }
  }

  return ret;
}

int MQTTProtocolHandler::get_subscriptions(MQTTVector<MQTTString>& subscriptions) const
{
  int ret = MQ_SUCCESS;

  try {
    subscriptions = subscriptions_;
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to copy subscriptions: {}", e.what());
    ret = MQ_ERR_MEMORY_ALLOC;
  }

  return ret;
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
  int ret = parser_->serialize_suback(packet, *serialize_buffer_);
  if (ret != 0) {
    packet->~SubAckPacket();
    allocator_->deallocate(packet, sizeof(SubAckPacket));
    return ret;
  }

  // 使用统一的带锁写入函数
  ret = send_data_with_lock(reinterpret_cast<const char*>(serialize_buffer_->data()),
                            serialize_buffer_->size());
  packet->~SubAckPacket();
  allocator_->deallocate(packet, sizeof(SubAckPacket));
  return ret;
}

int MQTTProtocolHandler::send_puback(uint16_t packet_id, ReasonCode reason_code)
{
  LOG_DEBUG("Sending PUBACK to client {}:{} (packet_id: {}, reason: 0x{:02x})", client_ip_.c_str(),
            client_port_, packet_id, static_cast<uint8_t>(reason_code));

  if (!socket_) {
    LOG_ERROR("Cannot send PUBACK: socket is null for client {}:{}", client_ip_.c_str(),
              client_port_);
    return MQ_ERR_SOCKET;
  }

  // 创建PUBACK包
  PubAckPacket* packet = new (allocator_->allocate(sizeof(PubAckPacket))) PubAckPacket();
  if (!packet) {
    LOG_ERROR("Failed to allocate PUBACK packet for client {}:{}", client_ip_.c_str(),
              client_port_);
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::PUBACK;
  packet->packet_id = packet_id;
  packet->reason_code = reason_code;

  // 序列化包
  int ret = parser_->serialize_puback(packet, *serialize_buffer_);
  if (ret != 0) {
    LOG_ERROR("Failed to serialize PUBACK packet for client {}:{}, error: {}", client_ip_.c_str(),
              client_port_, ret);
    allocator_->deallocate(packet, sizeof(PubAckPacket));
    return ret;
  }

  // 使用统一的带锁写入函数
  ret = send_data_with_lock(reinterpret_cast<const char*>(serialize_buffer_->data()),
                            serialize_buffer_->size());
  if (ret != 0) {
    LOG_ERROR("Failed to send PUBACK packet to client {}:{}, error: {}", client_ip_.c_str(),
              client_port_, ret);
  } else {
    LOG_DEBUG("Successfully sent PUBACK packet to client {}:{} (size: {})", client_ip_.c_str(),
              client_port_, serialize_buffer_->size());
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
  int ret = parser_->serialize_pingresp(packet, *serialize_buffer_);
  if (ret != 0) {
    allocator_->deallocate(packet, sizeof(PingRespPacket));
    return ret;
  }

  // 使用统一的带锁写入函数
  ret = send_data_with_lock(reinterpret_cast<const char*>(serialize_buffer_->data()),
                            serialize_buffer_->size());
  allocator_->deallocate(packet, sizeof(PingRespPacket));
  return ret;
}

void MQTTProtocolHandler::cleanup_session_registration(const char* context)
{
  if (connected_ && session_manager_ && !client_id_.empty()) {
    int ret = session_manager_->unregister_session(client_id_);
    if (ret != 0) {
      LOG_WARN("Failed to unregister session for client {} {}, error: {}",
               from_mqtt_string(client_id_), context ? context : "", ret);
    } else {
      LOG_DEBUG("Client {} unregistered from session manager {}", from_mqtt_string(client_id_),
                context ? context : "");
    }
    connected_ = false;
  }
}

int MQTTProtocolHandler::register_session_with_manager()
{
  if (!session_manager_) {
    LOG_WARN("Session manager not available for client {}", from_mqtt_string(client_id_));
    return MQ_SUCCESS;  // Not an error, just no session manager
  }

  if (client_id_.empty()) {
    LOG_ERROR("Cannot register session: client ID is empty");
    return MQ_ERR_CONNECT_CLIENT_ID;
  }

  int ret = session_manager_->register_session(client_id_, this);
  if (ret != 0) {
    LOG_ERROR("Failed to register session for client {}, error: {}", from_mqtt_string(client_id_),
              ret);
    return MQ_ERR_SESSION_REGISTER;
  }

  LOG_INFO("Client {} successfully registered with session manager", from_mqtt_string(client_id_));
  return MQ_SUCCESS;
}

int MQTTProtocolHandler::send_publish(const MQTTString& topic, const MQTTByteVector& payload,
                                      uint8_t qos, bool retain, bool dup,
                                      const Properties& properties)
{
  LOG_DEBUG("Sending PUBLISH to client {}:{} (topic: {}, qos: {}, retain: {}, dup: {})",
            client_ip_.c_str(), client_port_, from_mqtt_string(topic), qos, retain, dup);

  if (!socket_) {
    LOG_ERROR("Cannot send PUBLISH: socket is null for client {}:{}", client_ip_.c_str(),
              client_port_);
    return MQ_ERR_SOCKET;
  }

  // 创建PUBLISH包
  PublishPacket* packet = new (allocator_->allocate(sizeof(PublishPacket))) PublishPacket();
  if (!packet) {
    LOG_ERROR("Failed to allocate PUBLISH packet for client {}:{}", client_ip_.c_str(),
              client_port_);
    return MQ_ERR_MEMORY_ALLOC;
  }

  packet->type = PacketType::PUBLISH;
  packet->topic_name = topic;
  packet->payload = payload;
  packet->qos = qos;
  packet->retain = retain;
  packet->dup = dup;
  packet->properties = properties;

  // 如果QoS > 0，需要分配packet_id
  if (qos > 0) {
    packet->packet_id = get_next_packet_id();
  } else {
    packet->packet_id = 0;
  }

  // 序列化包
  int ret = parser_->serialize_publish(packet, *serialize_buffer_);
  if (ret != 0) {
    LOG_ERROR("Failed to serialize PUBLISH packet for client {}:{}, error: {}", client_ip_.c_str(),
              client_port_, ret);
    allocator_->deallocate(packet, sizeof(PublishPacket));
    return ret;
  }

  // 使用统一的带锁写入函数
  ret = send_data_with_lock(reinterpret_cast<const char*>(serialize_buffer_->data()),
                            serialize_buffer_->size());
  if (ret != 0) {
    LOG_ERROR("Failed to send PUBLISH packet to client {}:{}, error: {}", client_ip_.c_str(),
              client_port_, ret);
  } else {
    LOG_DEBUG("Successfully sent PUBLISH packet to client {}:{} (size: {}, packet_id: {})",
              client_ip_.c_str(), client_port_, serialize_buffer_->size(), packet->packet_id);
  }

  allocator_->deallocate(packet, sizeof(PublishPacket));
  return ret;
}

int MQTTProtocolHandler::send_publish(const PublishPacket& packet)
{
  return send_publish(packet.topic_name, packet.payload, packet.qos, packet.retain, packet.dup,
                      packet.properties);
}

}  // namespace mqtt