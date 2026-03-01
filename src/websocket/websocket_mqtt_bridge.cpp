#include "websocket_mqtt_bridge.h"
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

static int mqtt_packet_total_length(const uint8_t* data, size_t data_len, size_t& total_len) {
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

WebSocketMQTTBridge::~WebSocketMQTTBridge() {
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
    LOG_INFO("Registered WebSocket handler for client {}", client_id);

    return MQ_SUCCESS;
}

int WebSocketMQTTBridge::unregister_handler(const std::string& client_id) {
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

    // Unregister from session manager
    if (session_manager_) {
        mqtt::MQTTString mqtt_client_id = mqtt::to_mqtt_string(client_id, allocator_);
        session_manager_->unregister_session(mqtt_client_id);
    }

    // Remove handler
    handlers_.erase(client_id);
    pending_binary_buffers_.erase(client_id);
    client_protocol_versions_.erase(client_id);
    LOG_INFO("Unregistered WebSocket handler for client {}", client_id);

    return MQ_SUCCESS;
}

WebSocketProtocolHandler* WebSocketMQTTBridge::get_handler(const std::string& client_id) const {
    auto it = handlers_.find(client_id);
    return (it != handlers_.end()) ? it->second : nullptr;
}

int WebSocketMQTTBridge::handle_websocket_text(const std::string& client_id, const std::string& message) {
    stats_.ws_messages_received++;

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
                        ret = subscribe_topic(client_id, topic, static_cast<uint8_t>(qos));
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
    stats_.ws_messages_received++;

    LOG_DEBUG("Handling WebSocket binary message from {}: {} bytes", client_id, data.size());

    if (format_ != MessageFormat::MQTT_PACKET) {
        LOG_ERROR("Binary messages only supported in MQTT_PACKET format");
        stats_.translation_errors++;
        return MQ_ERR_INVALID_STATE;
    }

    BinaryBuffer& pending = pending_binary_buffers_[client_id];
    pending.insert(pending.end(), data.begin(), data.end());

    int ret = MQ_SUCCESS;
    while (!pending.empty()) {
        size_t packet_len = 0;
        ret = mqtt_packet_total_length(pending.data(), pending.size(), packet_len);
        if (ret == MQ_ERR_PACKET_INCOMPLETE) {
            LOG_TRACE("MQTT packet for client {} is incomplete, buffered {} bytes",
                      client_id, pending.size());
            return MQ_SUCCESS;
        }

        if (ret != MQ_SUCCESS) {
            LOG_ERROR("Invalid MQTT packet framing for client {}: {}", client_id, ret);
            stats_.translation_errors++;
            pending.clear();
            return ret;
        }

        std::vector<uint8_t> packet_bytes(pending.begin(), pending.begin() + packet_len);
        uint8_t protocol_version_hint = 5;
        auto version_it = client_protocol_versions_.find(client_id);
        if (version_it != client_protocol_versions_.end()) {
            protocol_version_hint = version_it->second;
        }
        mqtt::Packet* packet = nullptr;
        ret = parse_mqtt_packet(packet_bytes, packet, protocol_version_hint);
        if (ret != MQ_SUCCESS || !packet) {
            if (ret == MQ_ERR_PACKET_INCOMPLETE) {
                // Parser认为不完整时，继续等待更多数据，避免误断开。
                LOG_TRACE("Parser reported incomplete MQTT packet for client {}, buffered {} bytes",
                          client_id, pending.size());
                return MQ_SUCCESS;
            }

            LOG_ERROR("Failed to parse MQTT packet for client {}: {}", client_id, ret);
            stats_.translation_errors++;
            pending.erase(pending.begin(), pending.begin() + packet_len);
            return ret;
        }

        if (version_it == client_protocol_versions_.end() &&
            packet->type != mqtt::PacketType::CONNECT) {
            LOG_WARN("WebSocket client {} must send CONNECT as first MQTT packet", client_id);
            destroy_packet(packet);
            pending.erase(pending.begin(), pending.begin() + packet_len);
            stats_.translation_errors++;
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
                ret = handle_mqtt_publish_packet(client_id, static_cast<const mqtt::PublishPacket*>(packet));
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
                ret = handle_mqtt_disconnect(client_id);
                break;
            case mqtt::PacketType::PUBACK:
                {
                    const auto* puback = static_cast<const mqtt::PubAckPacket*>(packet);
                    LOG_DEBUG("Received MQTT PUBACK from WebSocket client {} (packet_id={}, reason=0x{:02x})",
                              client_id, puback->packet_id, static_cast<uint8_t>(puback->reason_code));
                    ret = MQ_SUCCESS;
                }
                break;
            case mqtt::PacketType::PUBREC:
                {
                    const auto* pubrec = static_cast<const mqtt::PubRecPacket*>(packet);
                    LOG_DEBUG("Received MQTT PUBREC from WebSocket client {} (packet_id={}, reason=0x{:02x})",
                              client_id, pubrec->packet_id, static_cast<uint8_t>(pubrec->reason_code));
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
                    ret = MQ_SUCCESS;
                }
                break;
            default:
                LOG_WARN("Unsupported MQTT packet type from WebSocket: {}", static_cast<int>(packet->type));
                ret = MQ_ERR_PACKET_TYPE;
                break;
        }

        destroy_packet(packet);
        pending.erase(pending.begin(), pending.begin() + packet_len);

        if (ret != MQ_SUCCESS) {
            stats_.translation_errors++;
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

int WebSocketMQTTBridge::parse_mqtt_packet(const std::vector<uint8_t>& data, mqtt::Packet*& packet,
                                           uint8_t protocol_version_hint) {
    if (!allocator_) {
        LOG_ERROR("Allocator is null");
        return MQ_ERR_MEMORY_ALLOC;
    }

    mqtt::MQTTParser parser(allocator_);
    parser.set_protocol_version_hint(protocol_version_hint);
    return parser.parse_packet(data.data(), data.size(), &packet);
}

int WebSocketMQTTBridge::handle_mqtt_publish(const std::string& client_id, const mqtt::PublishPacket& packet) {
    stats_.mqtt_messages_received++;

    LOG_DEBUG("Forwarding MQTT publish to WebSocket client {}: topic={}, qos={}",
              client_id, packet.topic_name, packet.qos);

    WebSocketProtocolHandler* handler = get_handler(client_id);
    if (!handler || !handler->is_connected()) {
        LOG_WARN("No active WebSocket handler for client {}", client_id);
        return MQ_ERR_SESSION_INVALID_HANDLER;
    }

    int ret = MQ_SUCCESS;

    switch (format_) {
        case MessageFormat::JSON:
            {
                std::string topic_str = mqtt::from_mqtt_string(packet.topic_name);
                std::vector<uint8_t> payload_vec(packet.payload.begin(), packet.payload.end());
                std::string json = format_json_message(topic_str, payload_vec,
                                                       packet.qos, packet.retain);
                ret = handler->send_text(json);
            }
            break;

        case MessageFormat::TEXT_PROTOCOL:
            {
                std::string topic_str = mqtt::from_mqtt_string(packet.topic_name);
                std::vector<uint8_t> payload_vec(packet.payload.begin(), packet.payload.end());
                std::string text = format_text_protocol(topic_str, payload_vec);
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
    }

    return ret;
}

int WebSocketMQTTBridge::subscribe_topic(const std::string& client_id, const std::string& topic_filter, uint8_t qos) {
    if (!session_manager_) {
        LOG_ERROR("Session manager not initialized");
        return MQ_ERR_SESSION_MANAGER_NOT_READY;
    }

    LOG_INFO("Subscribing WebSocket client {} to topic {}, qos={}", client_id, topic_filter, qos);

    // Add to subscription tracking
    subscriptions_[client_id].push_back(SubscriptionInfo(topic_filter, qos));
    stats_.active_subscriptions++;

    // Subscribe via session manager
    mqtt::MQTTString mqtt_topic = mqtt::to_mqtt_string(topic_filter, allocator_);
    mqtt::MQTTString mqtt_client_id = mqtt::to_mqtt_string(client_id, allocator_);
    return session_manager_->subscribe_topic(mqtt_topic, mqtt_client_id, qos);
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
                                         const std::vector<uint8_t>& payload, uint8_t qos, bool retain) {
    if (!session_manager_) {
        LOG_ERROR("Session manager not initialized");
        return MQ_ERR_SESSION_MANAGER_NOT_READY;
    }

    LOG_DEBUG("Publishing MQTT message from WebSocket client {}: topic={}, qos={}",
              client_id, topic, qos);

    // Create MQTT publish packet
    mqtt::PublishPacket packet(allocator_);
    packet.topic_name = mqtt::to_mqtt_string(topic, allocator_);
    packet.payload = mqtt::to_mqtt_bytes(payload, allocator_);
    packet.qos = qos;
    packet.retain = retain;
    packet.dup = false;

    // Forward to MQTT broker via session manager
    mqtt::MQTTString mqtt_topic = mqtt::to_mqtt_string(topic, allocator_);
    mqtt::MQTTString mqtt_client_id = mqtt::to_mqtt_string(client_id, allocator_);
    int ret = session_manager_->forward_publish_by_topic(mqtt_topic, packet, mqtt_client_id);

    if (ret == MQ_SUCCESS) {
        stats_.mqtt_messages_sent++;
    }

    return ret;
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
        int ret = send_serialized_mqtt_packet(client_id, connack);
        client_protocol_versions_.erase(client_id);
        return (ret == MQ_SUCCESS) ? MQ_ERR_CONNECT_PROTOCOL : ret;
    }

    client_protocol_versions_[client_id] = packet->protocol_version;

    mqtt::ConnAckPacket connack(allocator_);
    connack.type = mqtt::PacketType::CONNACK;
    connack.session_present = false;
    connack.reason_code = mqtt::ReasonCode::Success;
    int ret = send_serialized_mqtt_packet(client_id, connack);
    if (ret != MQ_SUCCESS) {
        client_protocol_versions_.erase(client_id);
    }
    return ret;
}

int WebSocketMQTTBridge::handle_mqtt_subscribe(const std::string& client_id, const mqtt::SubscribePacket* packet) {
    LOG_INFO("Received MQTT SUBSCRIBE from WebSocket client {}", client_id);

    mqtt::SubAckPacket suback(allocator_);
    suback.type = mqtt::PacketType::SUBACK;
    suback.packet_id = packet->packet_id;

    for (const auto& sub : packet->subscriptions) {
        std::string topic_filter = mqtt::from_mqtt_string(sub.first);
        int sub_ret = subscribe_topic(client_id, topic_filter, sub.second);
        if (sub_ret == MQ_SUCCESS) {
            switch (sub.second) {
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
            suback.reason_codes.push_back(mqtt::ReasonCode::UnspecifiedError);
        }
    }

    return send_serialized_mqtt_packet(client_id, suback);
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

int WebSocketMQTTBridge::handle_mqtt_publish_packet(const std::string& client_id, const mqtt::PublishPacket* packet) {
    std::string topic = mqtt::from_mqtt_string(packet->topic_name);
    std::vector<uint8_t> payload(packet->payload.begin(), packet->payload.end());
    int ret = publish_message(client_id, topic, payload, packet->qos, packet->retain);
    if (ret != MQ_SUCCESS) {
        return ret;
    }

    if (packet->qos == 1) {
        mqtt::PubAckPacket puback(allocator_);
        puback.type = mqtt::PacketType::PUBACK;
        puback.packet_id = packet->packet_id;
        puback.reason_code = mqtt::ReasonCode::Success;
        return send_serialized_mqtt_packet(client_id, puback);
    }

    if (packet->qos == 2) {
        mqtt::PubRecPacket pubrec(allocator_);
        pubrec.type = mqtt::PacketType::PUBREC;
        pubrec.packet_id = packet->packet_id;
        pubrec.reason_code = mqtt::ReasonCode::Success;
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

int WebSocketMQTTBridge::handle_mqtt_disconnect(const std::string& client_id) {
    LOG_INFO("Received MQTT DISCONNECT from WebSocket client {}", client_id);
    unregister_handler(client_id);
    return MQ_SUCCESS;
}

int WebSocketMQTTBridge::send_serialized_mqtt_packet(const std::string& client_id, const mqtt::Packet& packet) {
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

    std::vector<uint8_t> payload(serialized.data(), serialized.data() + serialized.size());
    ret = handler->send_binary(payload);
    if (ret == MQ_SUCCESS) {
        stats_.ws_messages_sent++;
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
