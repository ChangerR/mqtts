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
      remaining_length_(0)
{
  // Allocate initial buffer
  buffer_ = (char*)allocator_->allocate(INITIAL_BUFFER_SIZE);
  if (!buffer_) {
    LOG->error("Failed to allocate initial buffer");
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
  client_ip_ = client_ip;
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
    if (state_ == ReadState::READ_PAYLOAD && bytes_read_ >= remaining_length_) {
      ret = parse_packet();
      if (ret != 0) {
        // Reset state machine on parse error
        state_ = ReadState::READ_HEADER;
        bytes_needed_ = 1;
        bytes_read_ = 0;
        continue;
      }
    }
  }

  return ret;
}

int MQTTProtocolHandler::ensure_buffer_size(size_t needed_size)
{
  if (bytes_read_ + needed_size > current_buffer_size_) {
    size_t new_size = std::min(current_buffer_size_ * 2, MAX_BUFFER_SIZE);
    if (bytes_read_ + needed_size > new_size) {
      LOG->error("Packet too large from client {}:{} (needed: {}, max: {})", client_ip_,
                 client_port_, bytes_read_ + needed_size, MAX_BUFFER_SIZE);
      return -1;
    }

    // Allocate new buffer
    char* new_buffer = (char*)allocator_->allocate(new_size);
    if (!new_buffer) {
      LOG->error("Failed to resize buffer for client {}:{} (new size: {})", client_ip_,
                 client_port_, new_size);
      return -1;
    }

    // Copy existing data
    memcpy(new_buffer, buffer_, bytes_read_);

    // Free old buffer and update pointer
    allocator_->deallocate(buffer_, current_buffer_size_);
    buffer_ = new_buffer;
    current_buffer_size_ = new_size;

    LOG->debug("Buffer resized to {} bytes for client {}:{}", new_size, client_ip_, client_port_);
  }
  return 0;
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
    LOG->warn("Client {}:{} disconnected", client_ip_, client_port_);
    return MQ_ERR_SOCKET;
  }

  // Try to receive data
  int len = bytes_needed_;
  int ret = socket_->recv(buffer_ + bytes_read_, len);
  if (MQ_FAIL(ret)) {
    LOG->warn("Client {}:{} disconnected unexpectedly", client_ip_, client_port_);
    return ret;
  }

  // Check if connection closed
  if (len == 0) {
    LOG->warn("Client {}:{} closed connection", client_ip_, client_port_);
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
        // Move remaining data to start of buffer
        if (bytes_read_ > 1) {
          memmove(buffer_, buffer_ + 1, bytes_read_ - 1);
          bytes_read_ = bytes_read_ - 1;
        } else {
          bytes_read_ = 0;
        }
      } else {
        // Still need more data for header
        bytes_needed_ = 1 - bytes_read_;
      }
      break;
    }

    case ReadState::READ_REMAINING: {
      if (bytes_read_ >= 1) {
        uint8_t byte = buffer_[bytes_read_ - 1];
        remaining_length_ += (byte & 0x7F) * (1 << (7 * (bytes_read_ - 1)));

        if ((byte & 0x80) == 0) {
          // Check if remaining length is too large
          if (remaining_length_ > MAX_BUFFER_SIZE - 2) {  // -2 for header and remaining length
            LOG->error("Packet too large from client {}:{} (size: {}, max: {})", client_ip_,
                       client_port_, remaining_length_, MAX_BUFFER_SIZE - 2);
            return -1;
          }
          // Remaining length complete
          state_ = ReadState::READ_PAYLOAD;
          bytes_needed_ = remaining_length_;
          // Move remaining data to start of buffer
          if (bytes_read_ > 1) {
            memmove(buffer_, buffer_ + bytes_read_ - 1, 1);
            bytes_read_ = 1;
          } else {
            bytes_read_ = 0;
          }
        } else if (bytes_read_ >= 4) {
          // Invalid remaining length (more than 4 bytes)
          LOG->error("Invalid remaining length from client {}:{}", client_ip_, client_port_);
          return -1;
        } else {
          // Need more bytes for remaining length
          bytes_needed_ = 1;
        }
      } else {
        // Still need more data for remaining length
        bytes_needed_ = 1 - bytes_read_;
      }
      break;
    }

    case ReadState::READ_PAYLOAD: {
      if (bytes_read_ < remaining_length_) {
        // Still need more data for payload
        bytes_needed_ = remaining_length_ - bytes_read_;
      }
      // Packet will be parsed in the main loop when complete
      break;
    }
  }

  return 0;
}

int MQTTProtocolHandler::parse_packet()
{
  // Complete packet received, parse it
  Packet* packet = NULL;
  int ret = parser_->parse_packet(reinterpret_cast<const uint8_t*>(buffer_), remaining_length_ + 2,
                                  &packet);  // +2 for header and remaining length
  if (ret != 0) {
    LOG->error("Failed to parse MQTT packet from client {}:{}", client_ip_, client_port_);
    return ret;
  }

  // Handle packet based on type
  switch (packet->type) {
    case PacketType::CONNECT:
      ret = handle_connect(reinterpret_cast<ConnectPacket*>(packet));
      break;
    case PacketType::PUBLISH:
      ret = handle_publish(reinterpret_cast<PublishPacket*>(packet));
      break;
    case PacketType::SUBSCRIBE:
      ret = handle_subscribe(reinterpret_cast<SubscribePacket*>(packet));
      break;
    case PacketType::PINGREQ:
      ret = handle_pingreq();
      break;
    case PacketType::DISCONNECT:
      ret = handle_disconnect();
      break;
    default:
      LOG->warn("Unsupported packet type: 0x{:02x} from client {}:{}",
                static_cast<uint8_t>(packet->type), client_ip_, client_port_);
      break;
  }

  // Free packet
  allocator_->deallocate(packet, sizeof(Packet));

  // Reset state machine for next packet
  state_ = ReadState::READ_HEADER;
  bytes_needed_ = 1;
  bytes_read_ = 0;

  return ret;
}

int MQTTProtocolHandler::handle_connect(const ConnectPacket* packet)
{
  // Check protocol version
  if (packet->protocol_version != 5) {
    LOG->error("Unsupported protocol version: {} from client {}:{}", packet->protocol_version,
               client_ip_, client_port_);
    return -1;
  }

  // Check if client ID is valid
  if (packet->client_id.empty()) {
    LOG->error("Empty client ID from client {}:{}", client_ip_, client_port_);
    return send_connack(ReasonCode::ClientIdentifierNotValid);
  }

  // Update session state
  client_id_ = packet->client_id;
  connected_ = true;

  // Send CONNACK
  return send_connack(ReasonCode::Success);
}

int MQTTProtocolHandler::handle_publish(const PublishPacket* packet)
{
  if (!connected_) {
    LOG->error("Client {}:{} not connected", client_ip_, client_port_);
    return -1;
  }

  // Check if topic is valid
  if (packet->topic_name.empty()) {
    LOG->error("Empty topic name from client {}:{}", client_ip_, client_port_);
    return -1;
  }

  // Send PUBACK for QoS 1
  if (packet->qos == 1) {
    return send_puback(packet->packet_id, ReasonCode::Success);
  }

  return 0;
}

int MQTTProtocolHandler::handle_subscribe(const SubscribePacket* packet)
{
  if (!connected_) {
    LOG->error("Client {}:{} not connected", client_ip_, client_port_);
    return -1;
  }

  std::vector<ReasonCode> reason_codes;
  for (const auto& sub : packet->subscriptions) {
    const std::string& topic_filter = sub.first;
    uint8_t qos = sub.second;

    // Check if topic filter is valid
    if (topic_filter.empty()) {
      reason_codes.push_back(ReasonCode::TopicFilterInvalid);
      continue;
    }

    // Add subscription
    add_subscription(topic_filter);
    reason_codes.push_back(static_cast<ReasonCode>(qos));
  }

  // Send SUBACK
  return send_suback(packet->packet_id, reason_codes);
}

int MQTTProtocolHandler::handle_pingreq()
{
  if (!connected_) {
    LOG->error("Client {}:{} not connected", client_ip_, client_port_);
    return -1;
  }

  return send_pingresp();
}

int MQTTProtocolHandler::handle_disconnect()
{
  if (!connected_) {
    LOG->error("Client {}:{} not connected", client_ip_, client_port_);
    return -1;
  }

  // Remove all subscriptions
  subscriptions_.clear();
  connected_ = false;

  return 0;
}

int MQTTProtocolHandler::send_connack(ReasonCode reason_code)
{
  std::vector<uint8_t> response;
  response.push_back(static_cast<uint8_t>(PacketType::CONNACK));
  response.push_back(0);  // Remaining length
  response.push_back(0);  // Session present
  response.push_back(static_cast<uint8_t>(reason_code));

  return socket_->send(reinterpret_cast<const char*>(response.data()), response.size());
}

int MQTTProtocolHandler::send_puback(uint16_t packet_id, ReasonCode reason_code)
{
  std::vector<uint8_t> response;
  response.push_back(static_cast<uint8_t>(PacketType::PUBACK));
  response.push_back(3);  // Remaining length
  response.push_back((packet_id >> 8) & 0xFF);
  response.push_back(packet_id & 0xFF);
  response.push_back(static_cast<uint8_t>(reason_code));

  return socket_->send(reinterpret_cast<const char*>(response.data()), response.size());
}

int MQTTProtocolHandler::send_suback(uint16_t packet_id,
                                     const std::vector<ReasonCode>& reason_codes)
{
  std::vector<uint8_t> response;
  response.push_back(static_cast<uint8_t>(PacketType::SUBACK));
  response.push_back(2 + reason_codes.size());  // Remaining length
  response.push_back((packet_id >> 8) & 0xFF);
  response.push_back(packet_id & 0xFF);
  for (ReasonCode code : reason_codes) {
    response.push_back(static_cast<uint8_t>(code));
  }

  return socket_->send(reinterpret_cast<const char*>(response.data()), response.size());
}

int MQTTProtocolHandler::send_pingresp()
{
  std::vector<uint8_t> response;
  response.push_back(static_cast<uint8_t>(PacketType::PINGRESP));
  response.push_back(0);  // Remaining length

  return socket_->send(reinterpret_cast<const char*>(response.data()), response.size());
}

void MQTTProtocolHandler::add_subscription(const std::string& topic)
{
  subscriptions_.push_back(topic);
}

void MQTTProtocolHandler::remove_subscription(const std::string& topic)
{
  subscriptions_.erase(std::remove(subscriptions_.begin(), subscriptions_.end(), topic),
                       subscriptions_.end());
}

}  // namespace mqtt