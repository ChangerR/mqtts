#include "websocket_mqtt_bridge.h"
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <cstring>
#include <sstream>
#include "logger.h"
#include "mqtt_parser.h"
#include "websocket_protocol_handler.h"

namespace websocket {

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
      subscriptions_(mqtt::mqtt_stl_allocator<std::pair<const std::string, std::vector<SubscriptionInfo, mqtt::mqtt_stl_allocator<SubscriptionInfo>>>>(allocator)) {
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

    // Parse MQTT packet
    mqtt::Packet* packet = nullptr;
    int ret = parse_mqtt_packet(data, packet);
    if (ret != MQ_SUCCESS || !packet) {
        LOG_ERROR("Failed to parse MQTT packet: {}", ret);
        stats_.translation_errors++;
        return ret;
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
        case mqtt::PacketType::DISCONNECT:
            ret = handle_mqtt_disconnect(client_id);
            break;
        default:
            LOG_WARN("Unsupported MQTT packet type from WebSocket: {}", static_cast<int>(packet->type));
            ret = MQ_ERR_PACKET_TYPE;
    }

    // Clean up packet
    if (allocator_) {
        allocator_->deallocate(packet, sizeof(*packet));
    } else {
        delete packet;
    }

    return ret;
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

int WebSocketMQTTBridge::parse_mqtt_packet(const std::vector<uint8_t>& data, mqtt::Packet*& packet) {
    if (!allocator_) {
        LOG_ERROR("Allocator is null");
        return MQ_ERR_MEMORY_ALLOC;
    }

    mqtt::MQTTParser parser(allocator_);
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
                // TODO: Serialize MQTT packet to binary
                LOG_WARN("MQTT_PACKET format not yet implemented for publishing to WebSocket");
                ret = MQ_ERR_PROTOCOL;
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
    LOG_INFO("Received MQTT CONNECT from WebSocket client {}", client_id);
    // For WebSocket, we don't really need to handle CONNECT
    // Just send CONNACK
    // TODO: Send CONNACK packet
    return MQ_SUCCESS;
}

int WebSocketMQTTBridge::handle_mqtt_subscribe(const std::string& client_id, const mqtt::SubscribePacket* packet) {
    LOG_INFO("Received MQTT SUBSCRIBE from WebSocket client {}", client_id);

    for (const auto& sub : packet->subscriptions) {
        std::string topic_filter = mqtt::from_mqtt_string(sub.first);
        subscribe_topic(client_id, topic_filter, sub.second);
    }

    // TODO: Send SUBACK packet
    return MQ_SUCCESS;
}

int WebSocketMQTTBridge::handle_mqtt_unsubscribe(const std::string& client_id, const mqtt::UnsubscribePacket* packet) {
    LOG_INFO("Received MQTT UNSUBSCRIBE from WebSocket client {}", client_id);

    for (const auto& topic : packet->topic_filters) {
        std::string topic_filter = mqtt::from_mqtt_string(topic);
        unsubscribe_topic(client_id, topic_filter);
    }

    // TODO: Send UNSUBACK packet
    return MQ_SUCCESS;
}

int WebSocketMQTTBridge::handle_mqtt_publish_packet(const std::string& client_id, const mqtt::PublishPacket* packet) {
    std::string topic = mqtt::from_mqtt_string(packet->topic_name);
    std::vector<uint8_t> payload(packet->payload.begin(), packet->payload.end());
    return publish_message(client_id, topic, payload, packet->qos, packet->retain);
}

int WebSocketMQTTBridge::handle_mqtt_pingreq(const std::string& client_id) {
    LOG_TRACE("Received MQTT PINGREQ from WebSocket client {}", client_id);
    // TODO: Send PINGRESP packet
    return MQ_SUCCESS;
}

int WebSocketMQTTBridge::handle_mqtt_disconnect(const std::string& client_id) {
    LOG_INFO("Received MQTT DISCONNECT from WebSocket client {}", client_id);
    unregister_handler(client_id);
    return MQ_SUCCESS;
}

}  // namespace websocket
