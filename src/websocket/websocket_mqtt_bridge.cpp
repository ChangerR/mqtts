#include "websocket_mqtt_bridge.h"
#include "mqtt_connack_limits.h"
#include "mqtt_connection_watch.h"
#include "mqtt_protocol_handler.h"
#include "mqtt_will.h"
#include <chrono>
#include <mutex>
#include <new>
#include <thread>
#include <algorithm>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <cstring>
#include <sstream>
#include "logger.h"
#include "mqtt_parser.h"
#include "websocket_protocol_handler.h"

namespace websocket {

static const size_t kRetainedPendingCapacity = 64 * 1024;

static int mqtt_packet_total_length(const uint8_t* data, size_t data_len, size_t max_packet,
                                     size_t& total_len) {
    total_len = 0;
    if (!data || data_len < 2) {
        return MQ_ERR_PACKET_INCOMPLETE;
    }

    // Byte 0 is packet type+flags, remaining length starts at byte 1.
    uint32_t remaining_length = 0;
    uint32_t multiplier = 1;
    size_t remaining_length_bytes = 0;
    const size_t max_remaining_length_bytes = 4;

    while (remaining_length_bytes < max_remaining_length_bytes) {
        const size_t idx = 1 + remaining_length_bytes;
        if (idx >= data_len) {
            return MQ_ERR_PACKET_INCOMPLETE;
        }

        uint8_t encoded = data[idx];
        remaining_length += static_cast<uint32_t>(encoded & 0x7F) * multiplier;
        remaining_length_bytes++;

        if ((encoded & 0x80) == 0) {
            total_len = 1 + remaining_length_bytes + static_cast<size_t>(remaining_length);
            if (max_packet != 0 && total_len > max_packet) {
                return MQ_ERR_PACKET_TOO_LARGE;
            }
            if (data_len < total_len) {
                return MQ_ERR_PACKET_INCOMPLETE;
            }
            return MQ_SUCCESS;
        }

        multiplier *= 128;
    }

    return MQ_ERR_PACKET_INVALID;
}

static bool is_supported_protocol_name_version(const mqtt::MQTTString& protocol_name,
                                               uint8_t protocol_version) {
    const std::string name = mqtt::from_mqtt_string(protocol_name);
    if (protocol_version == 3) {
        return name == "MQIsdp";
    }
    if (protocol_version == 4 || protocol_version == 5) {
        return name == "MQTT";
    }
    return false;
}

// Simple JSON parsing helpers
static bool json_get_string(const std::string& json, const std::string& key, std::string& value) {
    std::string search_key = "\"" + key + "\"";
    size_t key_pos = json.find(search_key);
    if (key_pos == std::string::npos) {
        return false;
    }

    // Find the colon
    size_t colon_pos = json.find(':', key_pos);
    if (colon_pos == std::string::npos) {
        return false;
    }

    // Skip whitespace and find opening quote
    size_t value_start = json.find('"', colon_pos);
    if (value_start == std::string::npos) {
        return false;
    }
    value_start++;

    // Find closing quote
    size_t value_end = json.find('"', value_start);
    if (value_end == std::string::npos) {
        return false;
    }

    value = json.substr(value_start, value_end - value_start);
    return true;
}

static bool json_get_int(const std::string& json, const std::string& key, int& value) {
    std::string search_key = "\"" + key + "\"";
    size_t key_pos = json.find(search_key);
    if (key_pos == std::string::npos) {
        return false;
    }

    // Find the colon
    size_t colon_pos = json.find(':', key_pos);
    if (colon_pos == std::string::npos) {
        return false;
    }

    // Skip whitespace
    size_t value_start = colon_pos + 1;
    while (value_start < json.length() && (json[value_start] == ' ' || json[value_start] == '\t')) {
        value_start++;
    }

    // Parse integer
    char* end = nullptr;
    value = std::strtol(json.c_str() + value_start, &end, 10);
    return end != (json.c_str() + value_start);
}

static bool json_get_bool(const std::string& json, const std::string& key, bool& value) {
    std::string search_key = "\"" + key + "\"";
    size_t key_pos = json.find(search_key);
    if (key_pos == std::string::npos) {
        return false;
    }

    // Find the colon
    size_t colon_pos = json.find(':', key_pos);
    if (colon_pos == std::string::npos) {
        return false;
    }

    // Skip whitespace
    size_t value_start = colon_pos + 1;
    while (value_start < json.length() && (json[value_start] == ' ' || json[value_start] == '\t')) {
        value_start++;
    }

    if (json.substr(value_start, 4) == "true") {
        value = true;
        return true;
    } else if (json.substr(value_start, 5) == "false") {
        value = false;
        return true;
    }

    return false;
}

// Base64 encode/decode
std::string WebSocketMQTTBridge::base64_encode(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return "";
    }

    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    BIO_write(bio, data.data(), data.size());
    BIO_flush(bio);

    BUF_MEM* buffer_ptr;
    BIO_get_mem_ptr(bio, &buffer_ptr);

    std::string result(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio);

    return result;
}

std::vector<uint8_t> WebSocketMQTTBridge::base64_decode(const std::string& encoded) {
    if (encoded.empty()) {
        return {};
    }

    BIO* bio = BIO_new_mem_buf(encoded.c_str(), encoded.length());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    std::vector<uint8_t> result(encoded.length());
    int decoded_length = BIO_read(bio, result.data(), result.size());

    BIO_free_all(bio);

    if (decoded_length > 0) {
        result.resize(decoded_length);
    } else {
        result.clear();
    }

    return result;
}

// WebSocketMQTTBridge implementation

WebSocketMQTTBridge::WebSocketMQTTBridge(MQTTAllocator* allocator, MessageFormat format)
    : allocator_(allocator),
      format_(format),
      session_manager_(nullptr),
      handlers_(mqtt::mqtt_stl_allocator<std::pair<const std::string, WebSocketProtocolHandler*>>(allocator)),
      subscriptions_(mqtt::mqtt_stl_allocator<std::pair<const std::string, std::vector<SubscriptionInfo, mqtt::mqtt_stl_allocator<SubscriptionInfo>>>>(allocator)),
      pending_binary_buffers_(mqtt::mqtt_stl_allocator<std::pair<const std::string, BinaryBuffer>>(allocator)),
      client_protocol_versions_(mqtt::mqtt_stl_allocator<std::pair<const std::string, uint8_t>>(allocator)),
      allow_mqtt3x_(true) {
}

const mqtt::ProtocolLimits& WebSocketMQTTBridge::effective_limits() const {
    if (protocol_limits_) return *protocol_limits_;
    static const mqtt::ProtocolLimits defaults = mqtt::ProtocolLimits::defaults();
    return defaults;
}

void WebSocketMQTTBridge::fill_connack(mqtt::ConnAckPacket& connack, const mqtt::ConnectPacket* packet) {
    const bool persistence = session_manager_ && session_manager_->durable_store();
    uint8_t version = 5;
    if (packet && (packet->protocol_version == 3 || packet->protocol_version == 4)) {
        version = packet->protocol_version;
    }
    const uint16_t keep_alive = packet ? packet->keep_alive : 0;
    const bool requested_expiry = packet && packet->properties.has_session_expiry_interval;
    mqtt::apply_connack_capabilities(connack, effective_limits(), version, keep_alive, requested_expiry,
                                     persistence);
}

WebSocketMQTTBridge::~WebSocketMQTTBridge() {
    std::vector<std::string> transports;
    transports.reserve(handlers_.size());
    for (const auto& pair : handlers_) {
        transports.push_back(pair.first);
    }
    for (const auto& transport_id : transports) {
        dispose_will(transport_id);
    }
    // Unregister all handlers
    for (auto& pair : handlers_) {
        if (session_manager_) {
            mqtt::MQTTString client_id = mqtt::to_mqtt_string(pair.first, allocator_);
            session_manager_->unregister_session(client_id);
        }
    }
    handlers_.clear();
    subscriptions_.clear();
    pending_binary_buffers_.clear();
    client_protocol_versions_.clear();
    session_handlers_.clear();
    mqtt_client_ids_.clear();
}

int WebSocketMQTTBridge::init(mqtt::GlobalSessionManager* session_manager) {
    if (!session_manager) {
        LOG_ERROR("Session manager is null");
        return MQ_ERR_INVALID_ARGS;
    }

    session_manager_ = session_manager;
    LOG_INFO("WebSocket MQTT bridge initialized");
    return MQ_SUCCESS;
}

int WebSocketMQTTBridge::register_handler(const std::string& client_id, WebSocketProtocolHandler* handler) {
    if (!handler) {
        LOG_ERROR("Handler is null");
        return MQ_ERR_INVALID_ARGS;
    }

    handlers_[client_id] = handler;
    client_protocol_versions_.erase(client_id);
    auth_contexts_.erase(client_id);
    mqtt_client_ids_.erase(client_id);
    LOG_INFO("Registered WebSocket handler for client {}", client_id);

    return MQ_SUCCESS;
}

void WebSocketMQTTBridge::bind_session_handler(const std::string& client_id,
                                              mqtt::MQTTProtocolHandler* handler) {
    session_handlers_[client_id] = handler;
}

uint8_t WebSocketMQTTBridge::mqtt_protocol_version(const std::string& client_id) const {
    auto found = client_protocol_versions_.find(client_id);
    if (found == client_protocol_versions_.end()) {
        return 5;
    }
    return found->second;
}

int WebSocketMQTTBridge::send_mqtt_disconnect(const std::string& client_id, mqtt::ReasonCode reason) {
    // MQTT-3.14.0-1: nothing may precede a successful CONNACK, so an unknown version means silence.
    auto version = client_protocol_versions_.find(client_id);
    if (version == client_protocol_versions_.end() || version->second < 5 ||
        format_ != MessageFormat::MQTT_PACKET) {
        return MQ_SUCCESS;
    }
    mqtt::DisconnectPacket packet(allocator_);
    packet.type = mqtt::PacketType::DISCONNECT;
    packet.reason_code = reason;
    return send_serialized_mqtt_packet(client_id, packet);
}

int WebSocketMQTTBridge::unregister_handler(const std::string& client_id) {
    // 先发布遗嘱，No Local 才能看见这条连接自己的订阅。
    dispose_will(client_id);
    // Unsubscribe from all topics
    auto sub_it = subscriptions_.find(client_id);
    if (sub_it != subscriptions_.end()) {
        if (session_manager_) {
            mqtt::MQTTString mqtt_client_id = mqtt::to_mqtt_string(client_id, allocator_);
            for (const auto& sub : sub_it->second) {
                mqtt::MQTTString mqtt_topic = mqtt::to_mqtt_string(sub.topic_filter, allocator_);
                session_manager_->unsubscribe_topic(mqtt_topic, mqtt_client_id);
            }
        }
        subscriptions_.erase(sub_it);
    }

    // Unregister from session manager. MQTT Client ID 和传输层 id 可能不是同一个。
    if (session_manager_) {
        auto mqtt_id = mqtt_client_ids_.find(client_id);
        if (mqtt_id != mqtt_client_ids_.end() && mqtt_id->second != client_id) {
            session_manager_->unregister_session(mqtt::to_mqtt_string(mqtt_id->second, allocator_));
        }
        mqtt::MQTTString transport_id = mqtt::to_mqtt_string(client_id, allocator_);
        session_manager_->unregister_session(transport_id);
    }

    // Remove handler
    handlers_.erase(client_id);
    pending_binary_buffers_.erase(client_id);
    client_protocol_versions_.erase(client_id);
    auth_contexts_.erase(client_id);
    session_handlers_.erase(client_id);
    mqtt_client_ids_.erase(client_id);
    connection_wills_.erase(client_id);
    LOG_INFO("Unregistered WebSocket handler for client {}", client_id);

    return MQ_SUCCESS;
}

WebSocketProtocolHandler* WebSocketMQTTBridge::get_handler(const std::string& client_id) const {
    auto it = handlers_.find(client_id);
    return (it != handlers_.end()) ? it->second : nullptr;
}

int WebSocketMQTTBridge::handle_websocket_text(const std::string& client_id, const std::string& message) {
    if (session_manager_) session_manager_->note_bytes_received(message.size());
    stats_.ws_messages_received++;
    if (auth_manager_) return MQ_ERR_CONNECT_NOT_AUTHORIZED; // Authenticated mode uses MQTT CONNECT, never legacy text commands.

    LOG_DEBUG("Handling WebSocket text message from {}: {}", client_id, message);

    int ret = MQ_SUCCESS;

    switch (format_) {
        case MessageFormat::JSON:
            {
                // Check if this is a control operation (subscribe/unsubscribe) or publish
                std::string type;
                if (json_get_string(message, "type", type)) {
                    // Handle control operations
                    if (type == "subscribe") {
                        std::string topic;
                        if (!json_get_string(message, "topic", topic)) {
                            LOG_ERROR("Missing 'topic' field in subscribe JSON");
                            stats_.translation_errors++;
                            return MQ_ERR_PROTOCOL;
                        }
                        int qos = 0;
                        json_get_int(message, "qos", qos);
                        mqtt::SubscriptionOptions options;
                        options.qos = static_cast<uint8_t>(qos);
                        bool existed = false;
                        std::unique_lock<std::recursive_mutex> retained_replay;
                        if (session_manager_) {
                            retained_replay = session_manager_->lock_retain_replay();
                        }
                        ret = subscribe_topic(client_id, topic, options, &existed);
                        if (ret == MQ_SUCCESS && session_manager_) {
                            session_manager_->deliver_retained(
                                mqtt::to_mqtt_string(client_id, allocator_),
                                mqtt::to_mqtt_string(topic, allocator_), options, existed);
                        }
                    } else if (type == "unsubscribe") {
                        std::string topic;
                        if (!json_get_string(message, "topic", topic)) {
                            LOG_ERROR("Missing 'topic' field in unsubscribe JSON");
                            stats_.translation_errors++;
                            return MQ_ERR_PROTOCOL;
                        }
                        ret = unsubscribe_topic(client_id, topic);
                    } else {
                        LOG_ERROR("Unknown operation type: {}", type);
                        stats_.translation_errors++;
                        return MQ_ERR_PROTOCOL;
                    }
                } else {
                    // No type field - assume it's a publish operation (backward compatibility)
                    std::string topic;
                    std::vector<uint8_t> payload;
                    uint8_t qos = 0;
                    bool retain = false;

                    ret = parse_json_message(message, topic, payload, qos, retain);
                    if (ret != MQ_SUCCESS) {
                        LOG_ERROR("Failed to parse JSON message: {}", ret);
                        stats_.translation_errors++;
                        return ret;
                    }

                    // Count at ingestion, including later authorization or quota failure.
                    if (session_manager_) session_manager_->note_publish_received(payload.size());
                    ret = publish_message(client_id, topic, payload, qos, retain);
                }
            }
            break;

        case MessageFormat::TEXT_PROTOCOL:
            {
                std::string topic;
                std::vector<uint8_t> payload;

                ret = parse_text_protocol(message, topic, payload);
                if (ret != MQ_SUCCESS) {
                    LOG_ERROR("Failed to parse text protocol message: {}", ret);
                    stats_.translation_errors++;
                    return ret;
                }

                if (session_manager_) session_manager_->note_publish_received(payload.size());
                ret = publish_message(client_id, topic, payload, 0, false);
            }
            break;

        default:
            LOG_ERROR("Unsupported message format for text message");
            stats_.translation_errors++;
            return MQ_ERR_INVALID_STATE;
    }

    return ret;
}

int WebSocketMQTTBridge::handle_websocket_binary(const std::string& client_id, const std::vector<uint8_t>& data) {
    return handle_websocket_binary(client_id, data.data(), data.size());
}

int WebSocketMQTTBridge::handle_websocket_binary(const std::string& client_id, const uint8_t* data,
                                                 size_t len) {
    stats_.ws_messages_received++;

    LOG_DEBUG("Handling WebSocket binary data from {}: {} bytes", client_id, len);

    if (format_ != MessageFormat::MQTT_PACKET) {
        LOG_ERROR("Binary messages only supported in MQTT_PACKET format");
        stats_.translation_errors++;
        return MQ_ERR_INVALID_STATE;
    }

    pending_binary_buffers_[client_id].insert(pending_binary_buffers_[client_id].end(), data,
                                              data + len);

    int ret = MQ_SUCCESS;
    while (true) {
        // Packet handlers may unregister the client (DISCONNECT, takeover), which drops this buffer.
        auto pending_it = pending_binary_buffers_.find(client_id);
        if (pending_it == pending_binary_buffers_.end() || pending_it->second.empty()) {
            break;
        }
        BinaryBuffer& pending = pending_it->second;
        size_t packet_len = 0;
        ret = mqtt_packet_total_length(pending.data(), pending.size(),
                                       effective_limits().max_packet_size, packet_len);
        if (ret == MQ_ERR_PACKET_INCOMPLETE) {
            // Size the buffer once from Remaining Length; geometric growth would briefly hold ~3x the packet.
            if (packet_len > pending.capacity()) {
                pending.reserve(packet_len);
            }
            LOG_TRACE("MQTT packet for client {} is incomplete, buffered {} bytes",
                      client_id, pending.size());
            return MQ_SUCCESS;
        }

        if (ret == MQ_ERR_PACKET_TOO_LARGE) {
            LOG_WARN("WebSocket MQTT packet from {} exceeds {} bytes", client_id,
                     effective_limits().max_packet_size);
            pending.clear();
            (void)send_mqtt_disconnect(client_id, mqtt::ReasonCode::PacketTooLarge);
            if (auto* socket_handler = get_handler(client_id)) {
                socket_handler->close_connection();
            }
            return ret;
        }

        if (ret != MQ_SUCCESS) {
            LOG_ERROR("Invalid MQTT packet framing for client {}: {}", client_id, ret);
            stats_.translation_errors++;
            pending.clear();
            if (session_manager_) {
                session_manager_->note_malformed_disconnect();
            }
            (void)send_mqtt_disconnect(client_id, mqtt::ReasonCode::MalformedPacket);
            return ret;
        }

        if (session_manager_) session_manager_->note_bytes_received(packet_len);
        uint8_t protocol_version_hint = 5;
        auto version_it = client_protocol_versions_.find(client_id);
        if (version_it != client_protocol_versions_.end()) {
            protocol_version_hint = version_it->second;
        }
        mqtt::Packet* packet = nullptr;
        ret = parse_mqtt_packet(pending.data(), packet_len, packet, protocol_version_hint);
        // The parser copies everything it keeps, so the bytes can go before the handlers run.
        pending.erase(pending.begin(), pending.begin() + packet_len);
        if (pending.empty() && pending.capacity() > kRetainedPendingCapacity) {
            BinaryBuffer(pending.get_allocator()).swap(pending);
        }
        if (ret != MQ_SUCCESS || !packet) {
            // Remaining Length already says the packet is complete; a short field inside it is malformed.
            LOG_ERROR("Failed to parse MQTT packet for client {}: {}", client_id, ret);
            stats_.translation_errors++;
            pending.clear();
            if (session_manager_) {
                session_manager_->note_malformed_disconnect();
            }
            const mqtt::ReasonCode reason = (ret == MQ_ERR_PROTOCOL)
                                                ? mqtt::ReasonCode::ProtocolError
                                                : mqtt::ReasonCode::MalformedPacket;
            (void)send_mqtt_disconnect(client_id, reason);
            return ret;
        }

        if (version_it == client_protocol_versions_.end() &&
            packet->type != mqtt::PacketType::CONNECT) {
            LOG_WARN("WebSocket client {} must send CONNECT as first MQTT packet", client_id);
            destroy_packet(packet);
            pending.clear();
            stats_.translation_errors++;
            if (session_manager_) {
                session_manager_->note_malformed_disconnect();
            }
            (void)send_mqtt_disconnect(client_id, mqtt::ReasonCode::ProtocolError);
            return MQ_ERR_PROTOCOL;
        }

        // Handle different packet types
        switch (packet->type) {
            case mqtt::PacketType::CONNECT:
                ret = handle_mqtt_connect(client_id, static_cast<const mqtt::ConnectPacket*>(packet));
                break;
            case mqtt::PacketType::SUBSCRIBE:
                ret = handle_mqtt_subscribe(client_id, static_cast<const mqtt::SubscribePacket*>(packet));
                break;
            case mqtt::PacketType::UNSUBSCRIBE:
                ret = handle_mqtt_unsubscribe(client_id, static_cast<const mqtt::UnsubscribePacket*>(packet));
                break;
            case mqtt::PacketType::PUBLISH:
                ret = handle_mqtt_publish_packet(client_id, static_cast<mqtt::PublishPacket*>(packet));
                break;
            case mqtt::PacketType::PINGREQ:
                ret = handle_mqtt_pingreq(client_id);
                break;
            case mqtt::PacketType::PINGRESP:
                // Client may send PINGRESP in some stacks; it's harmless for broker-side bridge.
                LOG_TRACE("Received MQTT PINGRESP from WebSocket client {}", client_id);
                ret = MQ_SUCCESS;
                break;
            case mqtt::PacketType::DISCONNECT:
                ret = handle_mqtt_disconnect(client_id, static_cast<const mqtt::DisconnectPacket*>(packet));
                break;
            case mqtt::PacketType::PUBACK:
                {
                    const auto* puback = static_cast<const mqtt::PubAckPacket*>(packet);
                    LOG_DEBUG("Received MQTT PUBACK from WebSocket client {} (packet_id={}, reason=0x{:02x})",
                              client_id, puback->packet_id, static_cast<uint8_t>(puback->reason_code));
                    auto session = session_handlers_.find(client_id);
                    if (session != session_handlers_.end() && session->second) {
                        if (session->second->classify_outbound_ack(
                                puback->packet_id, mqtt::ConnectionFlow::AckKind::PubAck) ==
                            mqtt::ConnectionFlow::AckDisposition::Unexpected) {
                            (void)send_mqtt_disconnect(client_id, mqtt::ReasonCode::ProtocolError);
                            if (auto* socket_handler = get_handler(client_id)) {
                                socket_handler->close_connection();
                            }
                            ret = MQ_ERR_PACKET_TYPE;
                            break;
                        }
                        session->second->release_outbound_packet_id(puback->packet_id);
                    }
                    ret = MQ_SUCCESS;
                }
                break;
            case mqtt::PacketType::PUBREC:
                {
                    const auto* pubrec = static_cast<const mqtt::PubRecPacket*>(packet);
                    LOG_DEBUG("Received MQTT PUBREC from WebSocket client {} (packet_id={}, reason=0x{:02x})",
                              client_id, pubrec->packet_id, static_cast<uint8_t>(pubrec->reason_code));
                    auto session = session_handlers_.find(client_id);
                    mqtt::MQTTProtocolHandler* flow =
                        session == session_handlers_.end() ? nullptr : session->second;
                    if (flow && flow->classify_outbound_ack(pubrec->packet_id,
                                                           mqtt::ConnectionFlow::AckKind::PubRec) ==
                                   mqtt::ConnectionFlow::AckDisposition::Unexpected) {
                        (void)send_mqtt_disconnect(client_id, mqtt::ReasonCode::ProtocolError);
                        if (auto* socket_handler = get_handler(client_id)) {
                            socket_handler->close_connection();
                        }
                        ret = MQ_ERR_PACKET_TYPE;
                        break;
                    }
                    if (static_cast<uint8_t>(pubrec->reason_code) >= 0x80) {
                        if (flow) flow->release_outbound_packet_id(pubrec->packet_id);
                        ret = MQ_SUCCESS;
                        break;
                    }
                    if (flow) flow->mark_outbound_awaiting_pubcomp(pubrec->packet_id);
                    mqtt::PubRelPacket pubrel(allocator_);
                    pubrel.type = mqtt::PacketType::PUBREL;
                    pubrel.packet_id = pubrec->packet_id;
                    pubrel.reason_code = mqtt::ReasonCode::Success;
                    ret = send_serialized_mqtt_packet(client_id, pubrel);
                }
                break;
            case mqtt::PacketType::PUBREL:
                {
                    const auto* pubrel = static_cast<const mqtt::PubRelPacket*>(packet);
                    LOG_DEBUG("Received MQTT PUBREL from WebSocket client {} (packet_id={}, reason=0x{:02x})",
                              client_id, pubrel->packet_id, static_cast<uint8_t>(pubrel->reason_code));
                    auto session = session_handlers_.find(client_id);
                    if (session != session_handlers_.end() && session->second) {
                        session->second->release_inbound_qos2(pubrel->packet_id);
                    }
                    mqtt::PubCompPacket pubcomp(allocator_);
                    pubcomp.type = mqtt::PacketType::PUBCOMP;
                    pubcomp.packet_id = pubrel->packet_id;
                    pubcomp.reason_code = mqtt::ReasonCode::Success;
                    ret = send_serialized_mqtt_packet(client_id, pubcomp);
                }
                break;
            case mqtt::PacketType::PUBCOMP:
                {
                    const auto* pubcomp = static_cast<const mqtt::PubCompPacket*>(packet);
                    LOG_DEBUG("Received MQTT PUBCOMP from WebSocket client {} (packet_id={}, reason=0x{:02x})",
                              client_id, pubcomp->packet_id, static_cast<uint8_t>(pubcomp->reason_code));
                    auto session = session_handlers_.find(client_id);
                    if (session != session_handlers_.end() && session->second) {
                        if (session->second->classify_outbound_ack(
                                pubcomp->packet_id, mqtt::ConnectionFlow::AckKind::PubComp) ==
                            mqtt::ConnectionFlow::AckDisposition::Unexpected) {
                            (void)send_mqtt_disconnect(client_id, mqtt::ReasonCode::ProtocolError);
                            if (auto* socket_handler = get_handler(client_id)) {
                                socket_handler->close_connection();
                            }
                            ret = MQ_ERR_PACKET_TYPE;
                            break;
                        }
                        session->second->release_outbound_packet_id(pubcomp->packet_id);
                    }
                    ret = MQ_SUCCESS;
                }
                break;
            default:
                LOG_WARN("Unsupported MQTT packet type from WebSocket: {}", static_cast<int>(packet->type));
                ret = MQ_ERR_PACKET_TYPE;
                break;
        }

        destroy_packet(packet);
        if (auto* handler = get_handler(client_id)) {
            handler->observe_mqtt_packet();
        }

        if (ret != MQ_SUCCESS) {
            stats_.translation_errors++;
            if (ret == MQ_ERR_PACKET_TYPE) {
                if (session_manager_) {
                    session_manager_->note_malformed_disconnect();
                }
                (void)send_mqtt_disconnect(client_id, mqtt::ReasonCode::ProtocolError);
            }
            return ret;
        }
    }

    return MQ_SUCCESS;
}

int WebSocketMQTTBridge::parse_json_message(const std::string& json, std::string& topic,
                                            std::vector<uint8_t>& payload, uint8_t& qos, bool& retain) {
    // Parse JSON: {"topic": "test", "payload": "hello", "qos": 0, "retain": false}
    if (!json_get_string(json, "topic", topic)) {
        LOG_ERROR("Missing 'topic' field in JSON");
        return MQ_ERR_PROTOCOL;
    }

    std::string payload_str;
    if (json_get_string(json, "payload", payload_str)) {
        // Try to decode as base64 first
        payload = base64_decode(payload_str);
        if (payload.empty() && !payload_str.empty()) {
            // Not base64, use as plain text
            payload.assign(payload_str.begin(), payload_str.end());
        }
    }

    int qos_int = 0;
    if (json_get_int(json, "qos", qos_int)) {
        qos = static_cast<uint8_t>(qos_int);
    }

    json_get_bool(json, "retain", retain);

    return MQ_SUCCESS;
}

std::string WebSocketMQTTBridge::format_json_message(const std::string& topic,
                                                     const std::vector<uint8_t>& payload,
                                                     uint8_t qos, bool retain) {
    std::ostringstream json;
    json << "{\"topic\":\"" << topic << "\","
         << "\"payload\":\"" << base64_encode(payload) << "\","
         << "\"qos\":" << static_cast<int>(qos) << ","
         << "\"retain\":" << (retain ? "true" : "false") << "}";

    return json.str();
}

int WebSocketMQTTBridge::parse_text_protocol(const std::string& text, std::string& topic,
                                             std::vector<uint8_t>& payload) {
    // Parse: "PUBLISH:topic:payload"
    size_t first_colon = text.find(':');
    if (first_colon == std::string::npos) {
        LOG_ERROR("Invalid text protocol format");
        return MQ_ERR_PROTOCOL;
    }

    std::string command = text.substr(0, first_colon);
    if (command != "PUBLISH") {
        LOG_ERROR("Unsupported command: {}", command);
        return MQ_ERR_PROTOCOL;
    }

    size_t second_colon = text.find(':', first_colon + 1);
    if (second_colon == std::string::npos) {
        LOG_ERROR("Invalid text protocol format");
        return MQ_ERR_PROTOCOL;
    }

    topic = text.substr(first_colon + 1, second_colon - first_colon - 1);
    std::string payload_str = text.substr(second_colon + 1);
    payload.assign(payload_str.begin(), payload_str.end());

    return MQ_SUCCESS;
}

std::string WebSocketMQTTBridge::format_text_protocol(const std::string& topic,
                                                      const std::vector<uint8_t>& payload) {
    std::ostringstream text;
    text << "PUBLISH:" << topic << ":";
    text.write(reinterpret_cast<const char*>(payload.data()), payload.size());

    return text.str();
}

int WebSocketMQTTBridge::parse_mqtt_packet(const uint8_t* data, size_t len, mqtt::Packet*& packet,
                                           uint8_t protocol_version_hint) {
    if (!allocator_) {
        LOG_ERROR("Allocator is null");
        return MQ_ERR_MEMORY_ALLOC;
    }

    mqtt::MQTTParser parser(allocator_);
    parser.set_protocol_version_hint(protocol_version_hint);
    return parser.parse_packet(data, len, &packet);
}

int WebSocketMQTTBridge::handle_mqtt_publish(const std::string& client_id, const mqtt::PublishPacket& packet) {
    stats_.mqtt_messages_received++;
    if (!topic_allowed(client_id, mqtt::from_mqtt_string(packet.topic_name), mqtt::auth::Permission::READ)) return MQ_ERR_CONNECT_NOT_AUTHORIZED;

    LOG_DEBUG("Forwarding MQTT publish to WebSocket client {}: topic={}, qos={}",
              client_id, packet.topic_name, packet.qos);

    WebSocketProtocolHandler* handler = get_handler(client_id);
    if (!handler || !handler->is_connected()) {
        LOG_WARN("No active WebSocket handler for client {}", client_id);
        return MQ_ERR_SESSION_INVALID_HANDLER;
    }

    int ret = MQ_SUCCESS;
    uint64_t text_bytes = 0;

    switch (format_) {
        case MessageFormat::JSON:
            {
                std::string topic_str = mqtt::from_mqtt_string(packet.topic_name);
                std::vector<uint8_t> payload_vec(packet.payload.begin(), packet.payload.end());
                std::string json = format_json_message(topic_str, payload_vec,
                                                       packet.qos, packet.retain);
                text_bytes = json.size();
                ret = handler->send_text(json);
            }
            break;

        case MessageFormat::TEXT_PROTOCOL:
            {
                std::string topic_str = mqtt::from_mqtt_string(packet.topic_name);
                std::vector<uint8_t> payload_vec(packet.payload.begin(), packet.payload.end());
                std::string text = format_text_protocol(topic_str, payload_vec);
                text_bytes = text.size();
                ret = handler->send_text(text);
            }
            break;

        case MessageFormat::MQTT_PACKET:
            {
                mqtt::PublishPacket outbound(allocator_);
                outbound.type = mqtt::PacketType::PUBLISH;
                outbound.dup = packet.dup;
                outbound.qos = packet.qos;
                outbound.retain = packet.retain;
                outbound.packet_id = packet.packet_id;
                outbound.topic_name = mqtt::to_mqtt_string(mqtt::from_mqtt_string(packet.topic_name), allocator_);
                std::vector<uint8_t> payload_vec(packet.payload.begin(), packet.payload.end());
                outbound.payload = mqtt::to_mqtt_bytes(payload_vec, allocator_);
                ret = send_serialized_mqtt_packet(client_id, outbound);
            }
            break;
    }

    if (ret == MQ_SUCCESS) {
        stats_.mqtt_messages_sent++;
        if (session_manager_) {
            session_manager_->note_publish_sent(packet.payload.size());
            if (text_bytes > 0) session_manager_->note_bytes_sent(text_bytes);
        }
    }

    return ret;
}

int WebSocketMQTTBridge::subscribe_topic(const std::string& client_id, const std::string& topic_filter, uint8_t qos) {
    mqtt::SubscriptionOptions options;
    options.qos = qos;
    return subscribe_topic(client_id, topic_filter, options);
}

int WebSocketMQTTBridge::subscribe_topic(const std::string& client_id, const std::string& topic_filter,
                                         const mqtt::SubscriptionOptions& options, bool* already_present) {
    if (!topic_allowed(client_id, topic_filter, mqtt::auth::Permission::READ)) return MQ_ERR_CONNECT_NOT_AUTHORIZED;
    if (!session_manager_) {
        LOG_ERROR("Session manager not initialized");
        return MQ_ERR_SESSION_MANAGER_NOT_READY;
    }

    LOG_INFO("Subscribing WebSocket client {} to topic {}, qos={}", client_id, topic_filter, options.qos);

    bool existed = false;
    std::vector<SubscriptionInfo, mqtt::mqtt_stl_allocator<SubscriptionInfo>>& subs = subscriptions_[client_id];
    for (size_t i = 0; i < subs.size(); ++i) {
        if (subs[i].topic_filter == topic_filter) {
            existed = true;
            break;
        }
    }
    if (already_present) {
        *already_present = existed;
    }

    mqtt::MQTTString mqtt_topic = mqtt::to_mqtt_string(topic_filter, allocator_);
    mqtt::MQTTString mqtt_client_id = mqtt::to_mqtt_string(client_id, allocator_);
    const int subscribed = session_manager_->subscribe_topic(mqtt_topic, mqtt_client_id, options);
    if (subscribed != MQ_SUCCESS) {
        return subscribed;
    }
    if (existed) {
        for (size_t i = 0; i < subs.size(); ++i) {
            if (subs[i].topic_filter == topic_filter) {
                subs[i].qos = options.qos;
            }
        }
    } else {
        subs.push_back(SubscriptionInfo(topic_filter, options.qos));
        stats_.active_subscriptions++;
    }
    return MQ_SUCCESS;
}

int WebSocketMQTTBridge::unsubscribe_topic(const std::string& client_id, const std::string& topic_filter) {
    if (!session_manager_) {
        LOG_ERROR("Session manager not initialized");
        return MQ_ERR_SESSION_MANAGER_NOT_READY;
    }

    LOG_INFO("Unsubscribing WebSocket client {} from topic {}", client_id, topic_filter);

    // Remove from subscription tracking
    auto it = subscriptions_.find(client_id);
    if (it != subscriptions_.end()) {
        auto& subs = it->second;
        subs.erase(std::remove_if(subs.begin(), subs.end(),
                                  [&topic_filter](const SubscriptionInfo& sub) {
                                      return sub.topic_filter == topic_filter;
                                  }), subs.end());
        stats_.active_subscriptions--;
    }

    // Unsubscribe via session manager
    mqtt::MQTTString mqtt_topic = mqtt::to_mqtt_string(topic_filter, allocator_);
    mqtt::MQTTString mqtt_client_id = mqtt::to_mqtt_string(client_id, allocator_);
    return session_manager_->unsubscribe_topic(mqtt_topic, mqtt_client_id);
}

int WebSocketMQTTBridge::publish_message(const std::string& client_id, const std::string& topic,
                                         const std::vector<uint8_t>& payload, uint8_t qos,
                                         bool retain, const mqtt::Properties& properties)
{
  if (retain && !effective_limits().retain_available) {
    auto handler = handlers_.find(client_id);
    if (handler != handlers_.end() && handler->second) {
      handler->second->close_connection();
    }
    return MQ_ERR_FLOW_CLOSED;
  }
  if (auth_manager_) {
    const auto found = auth_contexts_.find(client_id);
    if (found == auth_contexts_.end())
      return MQ_ERR_CONNECT_NOT_AUTHORIZED;
    const auto context = found->second;
    mqtt::auth::AuthResult result = mqtt::auth::AuthResult::ACCESS_DENIED;
    auth_manager_->check_publish(*context, mqtt::to_mqtt_string(topic, allocator_),
                                 mqtt::to_mqtt_bytes(payload, allocator_), result);
    if (result != mqtt::auth::AuthResult::SUCCESS) {
      const bool denied = result == mqtt::auth::AuthResult::ACCESS_DENIED ||
                          result == mqtt::auth::AuthResult::INVALID_CREDENTIALS ||
                          result == mqtt::auth::AuthResult::USER_NOT_FOUND;
      return denied ? MQ_ERR_CONNECT_NOT_AUTHORIZED : MQ_ERR_AUTH_UNAVAILABLE;
    }
  }
  if (!session_manager_) {
    LOG_ERROR("Session manager not initialized");
    return MQ_ERR_SESSION_MANAGER_NOT_READY;
  }

  LOG_DEBUG("Publishing MQTT message from WebSocket client {}: topic={}, qos={}", client_id, topic,
            qos);

  // Create MQTT publish packet
  mqtt::PublishPacket packet(allocator_);
  packet.topic_name = mqtt::to_mqtt_string(topic, allocator_);
  packet.payload = mqtt::to_mqtt_bytes(payload, allocator_);
  packet.qos = qos;
  packet.retain = retain;
  packet.dup = false;
  packet.properties = properties;

  // Forward to MQTT broker via session manager
  mqtt::MQTTString mqtt_topic = mqtt::to_mqtt_string(topic, allocator_);
  mqtt::MQTTString mqtt_client_id = mqtt::to_mqtt_string(client_id, allocator_);
  int ret = session_manager_->forward_publish_by_topic(mqtt_topic, packet, mqtt_client_id);

  if (ret >= 0) {
    stats_.mqtt_messages_sent++;
    return MQ_SUCCESS;  // Session manager returns the recipient count.
  }

  return ret;
}

bool WebSocketMQTTBridge::topic_allowed(const std::string& client_id, const std::string& topic, mqtt::auth::Permission permission) {
    if (!auth_manager_) return true;
    const auto found = auth_contexts_.find(client_id);
    if (found == auth_contexts_.end()) return false;
    // Keep the context alive across coroutine-aware HTTP I/O.
    const auto context = found->second;
    mqtt::auth::AuthResult result = mqtt::auth::AuthResult::ACCESS_DENIED;
    auth_manager_->check_topic_access(*context, mqtt::to_mqtt_string(topic, allocator_), permission, result);
    return result == mqtt::auth::AuthResult::SUCCESS;
}

// MQTT packet handlers (for binary protocol mode)

int WebSocketMQTTBridge::handle_mqtt_connect(const std::string& client_id, const mqtt::ConnectPacket* packet) {
    LOG_INFO("Received MQTT CONNECT from WebSocket client {} (mqtt_client_id='{}')",
             client_id, mqtt::from_mqtt_string(packet->client_id));

    if (!is_supported_protocol_name_version(packet->protocol_name, packet->protocol_version)) {
        LOG_WARN("WebSocket client {} provided unsupported protocol name/version: '{}' v{}",
                 client_id, mqtt::from_mqtt_string(packet->protocol_name), packet->protocol_version);
        if (packet->protocol_version == 3 || packet->protocol_version == 4) {
            client_protocol_versions_[client_id] = packet->protocol_version;
        } else {
            client_protocol_versions_[client_id] = 5;
        }
        mqtt::ConnAckPacket connack(allocator_);
        connack.type = mqtt::PacketType::CONNACK;
        connack.session_present = false;
        connack.reason_code = mqtt::ReasonCode::UnsupportedProtocolVersion;
        fill_connack(connack, packet);
        int ret = send_serialized_mqtt_packet(client_id, connack);
        client_protocol_versions_.erase(client_id);
        return (ret == MQ_SUCCESS) ? MQ_ERR_CONNECT_PROTOCOL : ret;
    }

    if ((packet->protocol_version == 3 || packet->protocol_version == 4) && !allow_mqtt3x_) {
        LOG_WARN("WebSocket client {} uses MQTT 3.x while it is disabled", client_id);
        client_protocol_versions_[client_id] = packet->protocol_version;
        mqtt::ConnAckPacket connack(allocator_);
        connack.type = mqtt::PacketType::CONNACK;
        connack.session_present = false;
        connack.reason_code = mqtt::ReasonCode::UnsupportedProtocolVersion;
        fill_connack(connack, packet);
        int ret = send_serialized_mqtt_packet(client_id, connack);
        client_protocol_versions_.erase(client_id);
        return (ret == MQ_SUCCESS) ? MQ_ERR_CONNECT_PROTOCOL : ret;
    }

    if (client_protocol_versions_.count(client_id)) {
        if (session_manager_) {
            session_manager_->note_malformed_disconnect();
        }
        (void)send_mqtt_disconnect(client_id, mqtt::ReasonCode::ProtocolError);
        return MQ_ERR_PROTOCOL;
    }
    client_protocol_versions_[client_id] = packet->protocol_version;
    mqtt::ConnAckPacket connack(allocator_);
    connack.type = mqtt::PacketType::CONNACK;
    connack.session_present = false;
    connack.reason_code = mqtt::ReasonCode::Success;
    if (session_manager_ && session_manager_->durable_store()) {
      // Persistent consumers use TCP; reject unsupported WS session semantics.
      if ((packet->protocol_version < 5 && !packet->flags.clean_start) ||
          packet->properties.session_expiry_interval)
        connack.reason_code = mqtt::ReasonCode::ServerUnavailable;
    }
    if (packet->client_id.empty()) connack.reason_code = mqtt::ReasonCode::ClientIdentifierNotValid;
    if (packet->flags.will_flag && connack.reason_code == mqtt::ReasonCode::Success) {
        uint8_t qos_limit = effective_limits().max_qos;
        auto session = session_handlers_.find(client_id);
        if (session != session_handlers_.end() && session->second) {
            qos_limit = session->second->granted_qos_limit();
        }
        const mqtt::WillConnectDecision decision = mqtt::validate_will_connect(
            packet->flags.will_qos, packet->flags.will_retain != 0,
            mqtt::from_mqtt_string(packet->will_topic), qos_limit,
            effective_limits().retain_available);
        if (!decision.accept) {
            LOG_WARN("CONNECT will rejected for WebSocket client {} reason 0x{:02x}", client_id,
                     static_cast<uint8_t>(decision.reason));
            connack.reason_code = decision.reason;
        }
    }
    if (auth_manager_ && connack.reason_code == mqtt::ReasonCode::Success) {
        auto context = std::make_shared<mqtt::auth::ClientAuthContext>(allocator_);
        mqtt::auth::AuthResult result = mqtt::auth::AuthResult::ACCESS_DENIED;
        mqtt::MQTTString address{mqtt::MQTTStrAllocator(allocator_)};
        const int authenticated = auth_manager_->authenticate_user(packet->username, packet->password, packet->client_id, address, 0, result, *context);
        if (authenticated != MQ_SUCCESS || result != mqtt::auth::AuthResult::SUCCESS) {
            connack.reason_code = mqtt::ReasonCode::NotAuthorized;
        } else {
            auth_contexts_[client_id] = std::move(context);
        }
    }
    fill_connack(connack, packet);
    if (connack.reason_code == mqtt::ReasonCode::Success && session_manager_) {
        auto found = session_handlers_.find(client_id);
        mqtt::MQTTProtocolHandler* session_handler =
            found == session_handlers_.end() ? nullptr : found->second;
        if (session_handler) {
            const std::string mqtt_id = mqtt::from_mqtt_string(packet->client_id);
            const int registered = session_manager_->register_session(
                mqtt::to_mqtt_string(mqtt_id, allocator_), session_handler);
            if (registered != MQ_SUCCESS) {
                connack.reason_code = mqtt::ReasonCode::ServerUnavailable;
            } else {
                mqtt_client_ids_[client_id] = mqtt_id;
                // 旧连接清理已经结束。在下一次让出之前取消或立刻到期它的延迟遗嘱。
                session_manager_->will_scheduler().observe_connect(mqtt_id, packet->flags.clean_start != 0);
                try {
                    remember_connection_will(client_id, mqtt_id, packet);
                } catch (const std::bad_alloc&) {
                    LOG_WARN("Will allocation failed for WebSocket client {}", mqtt_id);
                    session_manager_->unregister_session(mqtt::to_mqtt_string(mqtt_id, allocator_));
                    mqtt_client_ids_.erase(client_id);
                    connection_wills_.erase(client_id);
                    connack.reason_code = mqtt::ReasonCode::QuotaExceeded;
                }
            }
        }
    }
    if (connack.reason_code != mqtt::ReasonCode::Success) {
        send_serialized_mqtt_packet(client_id, connack);
        client_protocol_versions_.erase(client_id);
        auth_contexts_.erase(client_id);
        return connack.reason_code == mqtt::ReasonCode::ServerUnavailable
                   ? MQ_ERR_TIMEOUT_V2
                   : MQ_ERR_CONNECT_NOT_AUTHORIZED;
    }
    if (auto* handler = get_handler(client_id)) {
        handler->observe_mqtt_connect(mqtt::negotiated_keep_alive_seconds(
            packet->keep_alive, effective_limits().keep_alive_max, packet->protocol_version));
    }
    auto session = session_handlers_.find(client_id);
    if (session != session_handlers_.end() && session->second) {
        session->second->observe_client_flow(packet->protocol_version,
                                             packet->properties.receive_maximum,
                                             packet->properties.maximum_packet_size);
        session->second->set_client_id(
            mqtt::to_mqtt_string(mqtt::from_mqtt_string(packet->client_id), allocator_));
    }
    int ret = send_serialized_mqtt_packet(client_id, connack);
    if (ret != MQ_SUCCESS) {
        client_protocol_versions_.erase(client_id);
    }
    return ret;
}

int WebSocketMQTTBridge::handle_mqtt_subscribe(const std::string& client_id, const mqtt::SubscribePacket* packet) {
    LOG_INFO("Received MQTT SUBSCRIBE from WebSocket client {}", client_id);

    if (packet->properties.subscription_identifier != 0) {
        (void)send_mqtt_disconnect(client_id, mqtt::ReasonCode::SubscriptionIdentifiersNotSupported);
        auto handler = handlers_.find(client_id);
        if (handler != handlers_.end())
            handler->second->close_connection();
        return MQ_ERR_FLOW_CLOSED;
    }

    const uint8_t protocol = mqtt_protocol_version(client_id);
    const auto suback_failure = [protocol](mqtt::ReasonCode code) {
        return protocol >= 5 ? code : mqtt::ReasonCode::UnspecifiedError;
    };

    mqtt::SubAckPacket suback(allocator_);
    suback.type = mqtt::PacketType::SUBACK;
    suback.packet_id = packet->packet_id;
    struct GrantedRetain {
        std::string topic;
        mqtt::SubscriptionOptions options;
        bool existed;
    };
    std::vector<GrantedRetain> retained_grants;
    std::unique_lock<std::recursive_mutex> retained_replay;

    auto session = session_handlers_.find(client_id);
    mqtt::MQTTProtocolHandler* flow = session == session_handlers_.end() ? nullptr : session->second;
    const uint8_t qos_cap = flow ? flow->granted_qos_limit() : effective_limits().max_qos;
    for (const auto& sub : packet->subscriptions) {
        mqtt::MQTTString topic = sub.topic_filter;
        mqtt::SubscriptionOptions granted = sub.options;
        const uint8_t requested_qos = granted.qos;
        if (mqtt::is_shared_subscription_filter(topic)) {
            suback.reason_codes.push_back(
                suback_failure(mqtt::ReasonCode::SharedSubscriptionsNotSupported));
            continue;
        }
        if (!effective_limits().wildcard_subscription_available &&
            mqtt::topic_filter_has_wildcard(topic)) {
            suback.reason_codes.push_back(
                suback_failure(mqtt::ReasonCode::WildcardSubscriptionsNotSupported));
            continue;
        }
        if (requested_qos > 2) {
            suback.reason_codes.push_back(mqtt::ReasonCode::QoSNotSupported);
            continue;
        }
        if (granted.qos > qos_cap) {
            granted.qos = qos_cap;
        }
        bool existed = false;
        if (!retained_replay.owns_lock() && session_manager_) {
            retained_replay = session_manager_->lock_retain_replay();
        }
        int sub_ret = subscribe_topic(client_id, mqtt::from_mqtt_string(topic), granted, &existed);
        if (sub_ret == MQ_SUCCESS) {
          retained_grants.push_back(GrantedRetain{mqtt::from_mqtt_string(topic), granted, existed});
          switch (granted.qos) {
            case 0:
              suback.reason_codes.push_back(mqtt::ReasonCode::GrantedQoS0);
              break;
            case 1:
              suback.reason_codes.push_back(mqtt::ReasonCode::GrantedQoS1);
              break;
            case 2:
              suback.reason_codes.push_back(mqtt::ReasonCode::GrantedQoS2);
              break;
            default:
              suback.reason_codes.push_back(mqtt::ReasonCode::UnspecifiedError);
              break;
          }
        } else {
            suback.reason_codes.push_back(sub_ret == MQ_ERR_CONNECT_NOT_AUTHORIZED ? mqtt::ReasonCode::NotAuthorized : mqtt::ReasonCode::UnspecifiedError);
        }
    }

    const int sent = send_serialized_mqtt_packet(client_id, suback);
    if (sent == MQ_SUCCESS && session_manager_) {
        mqtt::MQTTString subscriber = mqtt::to_mqtt_string(client_id, allocator_);
        for (size_t i = 0; i < retained_grants.size(); ++i) {
            const GrantedRetain& grant = retained_grants[i];
            (void)session_manager_->deliver_retained(
                subscriber, mqtt::to_mqtt_string(grant.topic, allocator_), grant.options, grant.existed);
        }
    }
    return sent;
}

int WebSocketMQTTBridge::handle_mqtt_unsubscribe(const std::string& client_id, const mqtt::UnsubscribePacket* packet) {
    LOG_INFO("Received MQTT UNSUBSCRIBE from WebSocket client {}", client_id);

    mqtt::UnsubAckPacket unsuback(allocator_);
    unsuback.type = mqtt::PacketType::UNSUBACK;
    unsuback.packet_id = packet->packet_id;

    for (const auto& topic : packet->topic_filters) {
        std::string topic_filter = mqtt::from_mqtt_string(topic);
        int unsub_ret = unsubscribe_topic(client_id, topic_filter);
        unsuback.reason_codes.push_back(unsub_ret == MQ_SUCCESS
                                            ? mqtt::ReasonCode::Success
                                            : mqtt::ReasonCode::NoSubscriptionExisted);
    }

    return send_serialized_mqtt_packet(client_id, unsuback);
}

int WebSocketMQTTBridge::handle_mqtt_publish_packet(const std::string& client_id, mqtt::PublishPacket* packet) {
    // Receipt is independent of authorization, quota, and durable QoS 2 rejection.
    if (session_manager_) session_manager_->note_publish_received(packet->payload.size());
    auto session = session_handlers_.find(client_id);
    mqtt::MQTTProtocolHandler* flow = session == session_handlers_.end() ? nullptr : session->second;
    auto close_flow = [&](mqtt::ReasonCode reason, int err) {
        (void)send_mqtt_disconnect(client_id, reason);
        auto handler = handlers_.find(client_id);
        if (handler != handlers_.end()) handler->second->close_connection();
        return err;
    };
    if (!flow || !packet) {
        return close_flow(mqtt::ReasonCode::ProtocolError, MQ_ERR_FLOW_CLOSED);
    }
    const mqtt::InboundTopicAliases::Result alias = flow->apply_inbound_topic_alias(packet);
    const mqtt::ReasonCode alias_reason = mqtt::inbound_alias_disconnect_reason(alias);
    if (alias_reason != mqtt::ReasonCode::Success) {
        LOG_WARN("Inbound topic alias rejected for WebSocket client {} reason 0x{:02x}", client_id,
                 static_cast<uint8_t>(alias_reason));
        return close_flow(alias_reason, MQ_ERR_FLOW_CLOSED);
    }
    if (packet->qos > 2 || ((packet->qos == 1 || packet->qos == 2) && packet->packet_id == 0)) {
        return close_flow(mqtt::ReasonCode::ProtocolError, MQ_ERR_FLOW_CLOSED);
    }
    if (packet->retain && !effective_limits().retain_available) {
        return close_flow(mqtt::ReasonCode::RetainNotSupported, MQ_ERR_FLOW_CLOSED);
    }
    if (flow && packet->qos > flow->granted_qos_limit()) {
        return close_flow(mqtt::ReasonCode::QoSNotSupported, MQ_ERR_FLOW_CLOSED);
    }
    if (!flow && packet->qos == 2 && session_manager_ && session_manager_->durable_store()) {
        return close_flow(mqtt::ReasonCode::QoSNotSupported, MQ_ERR_PUBLISH_QOS);
    }
    bool qos2_tracked = false;
    if (flow && packet->qos == 2) {
        switch (flow->admit_inbound_qos2(packet->packet_id)) {
            case mqtt::ConnectionFlow::Qos2Admit::Duplicate: {
                mqtt::PubRecPacket pubrec(allocator_);
                pubrec.type = mqtt::PacketType::PUBREC;
                pubrec.packet_id = packet->packet_id;
                pubrec.reason_code = mqtt::ReasonCode::Success;
                return send_serialized_mqtt_packet(client_id, pubrec);
            }
            case mqtt::ConnectionFlow::Qos2Admit::Exceeded:
                return close_flow(mqtt::ReasonCode::ReceiveMaximumExceeded, MQ_ERR_FLOW_CLOSED);
            case mqtt::ConnectionFlow::Qos2Admit::Forward:
                qos2_tracked = true;
                break;
        }
    }
    std::string topic = mqtt::from_mqtt_string(packet->topic_name);
    std::vector<uint8_t> payload(packet->payload.begin(), packet->payload.end());
    // 转发会在同一条连接的预算里再放一份载荷。先释放解析器那份，否则接近上限的 PUBLISH 会 bad_alloc。
    {
        mqtt::MQTTByteVector released(packet->payload.get_allocator());
        packet->payload.swap(released);
    }
    int ret =
        publish_message(client_id, topic, payload, packet->qos, packet->retain, packet->properties);
    const auto protocol = client_protocol_versions_.find(client_id);
    const bool negative_ack = protocol != client_protocol_versions_.end() && protocol->second >= 5 &&
        (packet->qos == 1 || packet->qos == 2) &&
        (ret == MQ_ERR_CONNECT_NOT_AUTHORIZED || ret == MQ_ERR_AUTH_UNAVAILABLE || ret == MQ_ERR_PUBLISH_QUOTA);
    if (ret != MQ_SUCCESS && !negative_ack) {
      if (qos2_tracked && flow) flow->release_inbound_qos2(packet->packet_id);
      auto handler = handlers_.find(client_id);
      if (handler != handlers_.end())
        handler->second->close_connection();
      return ret;
    }
    if (ret != MQ_SUCCESS && qos2_tracked && flow) {
      flow->release_inbound_qos2(packet->packet_id);
    }

    if (packet->qos == 1) {
        mqtt::PubAckPacket puback(allocator_);
        puback.type = mqtt::PacketType::PUBACK;
        puback.packet_id = packet->packet_id;
        puback.reason_code = ret == MQ_SUCCESS ? mqtt::ReasonCode::Success :
            (ret == MQ_ERR_PUBLISH_QUOTA ? mqtt::ReasonCode::QuotaExceeded :
            (ret == MQ_ERR_CONNECT_NOT_AUTHORIZED ? mqtt::ReasonCode::NotAuthorized : mqtt::ReasonCode::UnspecifiedError));
        return send_serialized_mqtt_packet(client_id, puback);
    }

    if (packet->qos == 2) {
        mqtt::PubRecPacket pubrec(allocator_);
        pubrec.type = mqtt::PacketType::PUBREC;
        pubrec.packet_id = packet->packet_id;
        pubrec.reason_code = ret == MQ_SUCCESS ? mqtt::ReasonCode::Success :
            (ret == MQ_ERR_CONNECT_NOT_AUTHORIZED ? mqtt::ReasonCode::NotAuthorized : mqtt::ReasonCode::UnspecifiedError);
        return send_serialized_mqtt_packet(client_id, pubrec);
    }

    return MQ_SUCCESS;
}

int WebSocketMQTTBridge::handle_mqtt_pingreq(const std::string& client_id) {
    LOG_TRACE("Received MQTT PINGREQ from WebSocket client {}", client_id);
    mqtt::PingRespPacket pingresp(allocator_);
    pingresp.type = mqtt::PacketType::PINGRESP;
    return send_serialized_mqtt_packet(client_id, pingresp);
}

bool WebSocketMQTTBridge::remember_connection_will(const std::string& transport_id,
                                                   const std::string& mqtt_client_id,
                                                   const mqtt::ConnectPacket* packet) {
    ConnectionWill stored;
    stored.mqtt_client_id = mqtt_client_id;
    if (packet && packet->protocol_version >= 5) {
        stored.session_expiry_seconds = packet->properties.session_expiry_interval;
    }
    if (packet && packet->flags.will_flag) {
        stored.body = mqtt::will_body_from_connect(*packet);
        stored.present = true;
    }
    connection_wills_[transport_id] = std::move(stored);
    return true;
}

void WebSocketMQTTBridge::dispose_will(const std::string& transport_id) {
    auto found = connection_wills_.find(transport_id);
    if (found == connection_wills_.end() || found->second.settled) {
        return;
    }
    ConnectionWill& will = found->second;
    will.settled = true;
    if (!will.present || will.discard || !session_manager_ || will.mqtt_client_id.empty()) {
        return;
    }
    const bool persistent = session_manager_->durable_store() != nullptr;
    const uint32_t delay = mqtt::effective_will_delay_seconds(
        persistent, will.body.delay_seconds, will.session_expiry_seconds);
    const mqtt::WillBody body = will.body;
    const std::string mqtt_client_id = will.mqtt_client_id;
    std::unique_ptr<mqtt::WillScheduler::Entry> entry(new mqtt::WillScheduler::Entry());
    entry->client_id = mqtt_client_id;
    entry->forward_id = transport_id;
    entry->due = delay == 0 ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::now() + std::chrono::seconds(delay);
    entry->owner = std::this_thread::get_id();
    entry->body = body;
    const auto auth_found = auth_contexts_.find(transport_id);
    if (auth_found != auth_contexts_.end() && auth_found->second &&
        !mqtt::copy_auth_context(*auth_found->second, entry->auth)) {
        LOG_WARN("Failed to copy auth context for delayed will of {}", mqtt_client_id);
        session_manager_->note_will_rejected();
        return;
    }
    session_manager_->will_scheduler().schedule(std::move(entry));
    LOG_INFO("Scheduled WebSocket will for {} in {} seconds", mqtt_client_id, delay);
}

int WebSocketMQTTBridge::handle_mqtt_disconnect(const std::string& client_id,
                                                const mqtt::DisconnectPacket* packet) {
    LOG_INFO("Received MQTT DISCONNECT from WebSocket client {} reason 0x{:02x}", client_id,
             packet ? static_cast<uint8_t>(packet->reason_code) : 0);
    auto found = connection_wills_.find(client_id);
    if (packet && found != connection_wills_.end()) {
        if (session_manager_ && session_manager_->durable_store() &&
            packet->properties.has_session_expiry_interval &&
            found->second.session_expiry_seconds == 0 &&
            packet->properties.session_expiry_interval != 0) {
            // 和 TCP 一样：会话过期从 0 改成非 0 是协议错误，遗嘱仍然发布。
            auto handler = handlers_.find(client_id);
            if (handler != handlers_.end() && handler->second) {
                handler->second->close_connection();
            }
            return MQ_ERR_PACKET_INVALID;
        }
        if (packet->properties.has_session_expiry_interval) {
            found->second.session_expiry_seconds = packet->properties.session_expiry_interval;
        }
        found->second.discard = packet->reason_code == mqtt::ReasonCode::NormalDisconnection;
    }
    WebSocketProtocolHandler* handler = get_handler(client_id);
    unregister_handler(client_id);
    if (handler) {
        handler->close_connection();
    }
    return MQ_SUCCESS;
}

int WebSocketMQTTBridge::send_serialized_mqtt_packet(const std::string& client_id, mqtt::Packet& packet) {
    WebSocketProtocolHandler* handler = get_handler(client_id);
    if (!handler || !handler->is_connected()) {
        LOG_WARN("No active WebSocket handler for client {}", client_id);
        return MQ_ERR_SESSION_INVALID_HANDLER;
    }

    mqtt::MQTTParser parser(allocator_);
    auto version_it = client_protocol_versions_.find(client_id);
    if (version_it != client_protocol_versions_.end()) {
        parser.set_protocol_version_hint(version_it->second);
    } else {
        parser.set_protocol_version_hint(5);
    }
    mqtt::MQTTBuffer serialized(allocator_);
    int ret = MQ_ERR_PACKET_TYPE;

    switch (packet.type) {
        case mqtt::PacketType::CONNACK:
            ret = parser.serialize_connack(reinterpret_cast<const mqtt::ConnAckPacket*>(&packet), serialized);
            break;
        case mqtt::PacketType::SUBACK:
            ret = parser.serialize_suback(reinterpret_cast<const mqtt::SubAckPacket*>(&packet), serialized);
            break;
        case mqtt::PacketType::UNSUBACK:
            ret = parser.serialize_unsuback(reinterpret_cast<const mqtt::UnsubAckPacket*>(&packet), serialized);
            break;
        case mqtt::PacketType::PINGRESP:
            ret = parser.serialize_pingresp(reinterpret_cast<const mqtt::PingRespPacket*>(&packet), serialized);
            break;
        case mqtt::PacketType::DISCONNECT:
            ret = parser.serialize_disconnect(reinterpret_cast<const mqtt::DisconnectPacket*>(&packet), serialized);
            break;
        case mqtt::PacketType::PUBACK:
            ret = parser.serialize_puback(reinterpret_cast<const mqtt::PubAckPacket*>(&packet), serialized);
            break;
        case mqtt::PacketType::PUBREC:
            ret = parser.serialize_pubrec(reinterpret_cast<const mqtt::PubRecPacket*>(&packet), serialized);
            break;
        case mqtt::PacketType::PUBREL:
            ret = parser.serialize_pubrel(reinterpret_cast<const mqtt::PubRelPacket*>(&packet), serialized);
            break;
        case mqtt::PacketType::PUBCOMP:
            ret = parser.serialize_pubcomp(reinterpret_cast<const mqtt::PubCompPacket*>(&packet), serialized);
            break;
        case mqtt::PacketType::PUBLISH:
            ret = parser.serialize_publish(reinterpret_cast<const mqtt::PublishPacket*>(&packet), serialized);
            break;
        default:
            LOG_ERROR("Unsupported packet type for WebSocket binary serialization: 0x{:02x}",
                      static_cast<uint8_t>(packet.type));
            return MQ_ERR_PACKET_TYPE;
    }

    if (ret != MQ_SUCCESS) {
        LOG_ERROR("Failed to serialize MQTT packet type 0x{:02x}: {}",
                  static_cast<uint8_t>(packet.type), ret);
        return ret;
    }

    auto session = session_handlers_.find(client_id);
    mqtt::MQTTProtocolHandler* flow = session == session_handlers_.end() ? nullptr : session->second;
    if (flow && flow->outbound_exceeds_client_maximum(serialized.size())) {
        mqtt::MQTTProtocolHandler::omit_optional_outbound_properties(packet.properties);
        serialized.clear();
        switch (packet.type) {
            case mqtt::PacketType::CONNACK:
                ret = parser.serialize_connack(reinterpret_cast<const mqtt::ConnAckPacket*>(&packet), serialized);
                break;
            case mqtt::PacketType::SUBACK:
                ret = parser.serialize_suback(reinterpret_cast<const mqtt::SubAckPacket*>(&packet), serialized);
                break;
            case mqtt::PacketType::UNSUBACK:
                ret = parser.serialize_unsuback(reinterpret_cast<const mqtt::UnsubAckPacket*>(&packet), serialized);
                break;
            case mqtt::PacketType::PINGRESP:
                ret = parser.serialize_pingresp(reinterpret_cast<const mqtt::PingRespPacket*>(&packet), serialized);
                break;
            case mqtt::PacketType::DISCONNECT:
                ret = parser.serialize_disconnect(reinterpret_cast<const mqtt::DisconnectPacket*>(&packet), serialized);
                break;
            case mqtt::PacketType::PUBACK:
                ret = parser.serialize_puback(reinterpret_cast<const mqtt::PubAckPacket*>(&packet), serialized);
                break;
            case mqtt::PacketType::PUBREC:
                ret = parser.serialize_pubrec(reinterpret_cast<const mqtt::PubRecPacket*>(&packet), serialized);
                break;
            case mqtt::PacketType::PUBREL:
                ret = parser.serialize_pubrel(reinterpret_cast<const mqtt::PubRelPacket*>(&packet), serialized);
                break;
            case mqtt::PacketType::PUBCOMP:
                ret = parser.serialize_pubcomp(reinterpret_cast<const mqtt::PubCompPacket*>(&packet), serialized);
                break;
            case mqtt::PacketType::PUBLISH:
                ret = parser.serialize_publish(reinterpret_cast<const mqtt::PublishPacket*>(&packet), serialized);
                break;
            default:
                return MQ_ERR_PACKET_TYPE;
        }
        if (ret != MQ_SUCCESS) return ret;
        if (flow->outbound_exceeds_client_maximum(serialized.size())) {
            LOG_WARN("Not sending {}-byte WebSocket MQTT packet to {}, client maximum is smaller",
                     serialized.size(), client_id);
            if (auto* socket_handler = get_handler(client_id)) socket_handler->close_connection();
            return MQ_ERR_PACKET_TOO_LARGE;
        }
    }

    std::vector<uint8_t> payload(serialized.data(), serialized.data() + serialized.size());
    ret = handler->send_binary(payload);
    if (ret == MQ_SUCCESS) {
        stats_.ws_messages_sent++;
        if (session_manager_) session_manager_->note_bytes_sent(payload.size());
    }
    return ret;
}

void WebSocketMQTTBridge::destroy_packet(mqtt::Packet* packet) {
    if (!packet) {
        return;
    }

    if (!allocator_) {
        delete packet;
        return;
    }

    switch (packet->type) {
        case mqtt::PacketType::CONNECT:
            reinterpret_cast<mqtt::ConnectPacket*>(packet)->~ConnectPacket();
            allocator_->deallocate(packet, sizeof(mqtt::ConnectPacket));
            break;
        case mqtt::PacketType::CONNACK:
            reinterpret_cast<mqtt::ConnAckPacket*>(packet)->~ConnAckPacket();
            allocator_->deallocate(packet, sizeof(mqtt::ConnAckPacket));
            break;
        case mqtt::PacketType::PUBLISH:
            reinterpret_cast<mqtt::PublishPacket*>(packet)->~PublishPacket();
            allocator_->deallocate(packet, sizeof(mqtt::PublishPacket));
            break;
        case mqtt::PacketType::PUBACK:
            reinterpret_cast<mqtt::PubAckPacket*>(packet)->~PubAckPacket();
            allocator_->deallocate(packet, sizeof(mqtt::PubAckPacket));
            break;
        case mqtt::PacketType::PUBREC:
            reinterpret_cast<mqtt::PubRecPacket*>(packet)->~PubRecPacket();
            allocator_->deallocate(packet, sizeof(mqtt::PubRecPacket));
            break;
        case mqtt::PacketType::PUBREL:
            reinterpret_cast<mqtt::PubRelPacket*>(packet)->~PubRelPacket();
            allocator_->deallocate(packet, sizeof(mqtt::PubRelPacket));
            break;
        case mqtt::PacketType::PUBCOMP:
            reinterpret_cast<mqtt::PubCompPacket*>(packet)->~PubCompPacket();
            allocator_->deallocate(packet, sizeof(mqtt::PubCompPacket));
            break;
        case mqtt::PacketType::SUBSCRIBE:
            reinterpret_cast<mqtt::SubscribePacket*>(packet)->~SubscribePacket();
            allocator_->deallocate(packet, sizeof(mqtt::SubscribePacket));
            break;
        case mqtt::PacketType::SUBACK:
            reinterpret_cast<mqtt::SubAckPacket*>(packet)->~SubAckPacket();
            allocator_->deallocate(packet, sizeof(mqtt::SubAckPacket));
            break;
        case mqtt::PacketType::UNSUBSCRIBE:
            reinterpret_cast<mqtt::UnsubscribePacket*>(packet)->~UnsubscribePacket();
            allocator_->deallocate(packet, sizeof(mqtt::UnsubscribePacket));
            break;
        case mqtt::PacketType::UNSUBACK:
            reinterpret_cast<mqtt::UnsubAckPacket*>(packet)->~UnsubAckPacket();
            allocator_->deallocate(packet, sizeof(mqtt::UnsubAckPacket));
            break;
        case mqtt::PacketType::PINGREQ:
            reinterpret_cast<mqtt::PingReqPacket*>(packet)->~PingReqPacket();
            allocator_->deallocate(packet, sizeof(mqtt::PingReqPacket));
            break;
        case mqtt::PacketType::PINGRESP:
            reinterpret_cast<mqtt::PingRespPacket*>(packet)->~PingRespPacket();
            allocator_->deallocate(packet, sizeof(mqtt::PingRespPacket));
            break;
        case mqtt::PacketType::DISCONNECT:
            reinterpret_cast<mqtt::DisconnectPacket*>(packet)->~DisconnectPacket();
            allocator_->deallocate(packet, sizeof(mqtt::DisconnectPacket));
            break;
        case mqtt::PacketType::AUTH:
            reinterpret_cast<mqtt::AuthPacket*>(packet)->~AuthPacket();
            allocator_->deallocate(packet, sizeof(mqtt::AuthPacket));
            break;
        default:
            packet->~Packet();
            allocator_->deallocate(packet, sizeof(mqtt::Packet));
            break;
    }
}

}  // namespace websocket
