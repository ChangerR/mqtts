#include "websocket_mqtt_bridge.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sstream>
#include "logger.h"
#include "mqtt_packet.h"

namespace websocket {

WebSocketMQTTBridge::WebSocketMQTTBridge(MQTTAllocator* allocator)
    : allocator_(allocator)
    , session_manager_(nullptr)
    , message_format_(MessageFormat::JSON) {
}

WebSocketMQTTBridge::~WebSocketMQTTBridge() {
    // Cleanup any resources
}

int WebSocketMQTTBridge::init(mqtt::GlobalSessionManager* session_manager) {
    if (!session_manager) {
        LOG_ERROR("Session manager is null");
        return -1;
    }
    
    session_manager_ = session_manager;
    return 0;
}

int WebSocketMQTTBridge::register_websocket_handler(const std::string& client_id, 
                                                   WebSocketProtocolHandler* handler) {
    if (!handler) {
        LOG_ERROR("WebSocket handler is null for client: {}", client_id);
        return -1;
    }
    
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    websocket_handlers_[client_id] = handler;
    stats_.active_clients++;
    
    LOG_DEBUG("Registered WebSocket handler for client: {}", client_id);
    return 0;
}

int WebSocketMQTTBridge::unregister_websocket_handler(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    
    auto it = websocket_handlers_.find(client_id);
    if (it != websocket_handlers_.end()) {
        websocket_handlers_.erase(it);
        if (stats_.active_clients > 0) {
            stats_.active_clients--;
        }
        LOG_DEBUG("Unregistered WebSocket handler for client: {}", client_id);
        return 0;
    }
    
    return -1;
}

int WebSocketMQTTBridge::handle_websocket_message(const std::string& client_id, 
                                                 const std::string& message) {
    stats_.websocket_messages_received++;
    
    switch (message_format_) {
        case MessageFormat::JSON:
            return handle_json_message(client_id, message);
        case MessageFormat::TEXT_PROTOCOL:
            return handle_text_protocol_message(client_id, message);
        default:
            LOG_ERROR("Unsupported message format for text message");
            stats_.translation_errors++;
            return -1;
    }
}

int WebSocketMQTTBridge::handle_websocket_binary(const std::string& client_id, 
                                                const std::vector<uint8_t>& data) {
    stats_.websocket_messages_received++;
    
    if (message_format_ == MessageFormat::MQTT_PACKET) {
        return handle_mqtt_packet_message(client_id, data);
    } else {
        LOG_ERROR("Binary messages only supported in MQTT_PACKET format");
        stats_.translation_errors++;
        return -1;
    }
}

int WebSocketMQTTBridge::handle_json_message(const std::string& client_id, 
                                            const std::string& json_message) {
    try {
        // Simple JSON parsing (in real implementation, use a proper JSON library)
        if (json_message.find("\"type\":\"publish\"") != std::string::npos) {
            std::string topic;
            std::vector<uint8_t> payload;
            uint8_t qos;
            bool retain;
            
            int ret = parse_json_publish(json_message, topic, payload, qos, retain);
            if (ret != 0) {
                stats_.translation_errors++;
                return ret;
            }
            
            // Forward to MQTT
            if (session_manager_) {
                mqtt::MQTTString mqtt_topic(topic.c_str(), topic.length());
                mqtt::MQTTString mqtt_client_id(client_id.c_str(), client_id.length());
                
                mqtt::PublishPacket packet;
                packet.topic_name = mqtt_topic;
                packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end());
                packet.qos = qos;
                packet.retain = retain;
                
                session_manager_->forward_publish_by_topic(mqtt_topic, packet, mqtt_client_id);
                stats_.mqtt_messages_sent++;
            }
            
        } else if (json_message.find("\"type\":\"subscribe\"") != std::string::npos) {
            std::string topic_filter;
            uint8_t qos;
            
            int ret = parse_json_subscribe(json_message, topic_filter, qos);
            if (ret != 0) {
                stats_.translation_errors++;
                return ret;
            }
            
            return subscribe_topic(client_id, topic_filter, qos);
            
        } else if (json_message.find("\"type\":\"unsubscribe\"") != std::string::npos) {
            std::string topic_filter;
            
            int ret = parse_json_unsubscribe(json_message, topic_filter);
            if (ret != 0) {
                stats_.translation_errors++;
                return ret;
            }
            
            return unsubscribe_topic(client_id, topic_filter);
        }
        
        return 0;
    } catch (const std::exception& e) {
        LOG_ERROR("JSON message parsing error: {}", e.what());
        stats_.translation_errors++;
        return -1;
    }
}

int WebSocketMQTTBridge::parse_json_publish(const std::string& json_str, std::string& topic,
                                           std::vector<uint8_t>& payload, uint8_t& qos, bool& retain) {
    // Simple JSON parsing (in production, use a proper JSON library like nlohmann/json)
    
    // Extract topic
    size_t topic_start = json_str.find("\"topic\":\"");
    if (topic_start == std::string::npos) return -1;
    topic_start += 9; // length of "\"topic\":\""
    
    size_t topic_end = json_str.find("\"", topic_start);
    if (topic_end == std::string::npos) return -1;
    
    topic = json_str.substr(topic_start, topic_end - topic_start);
    
    // Extract payload (base64 encoded)
    size_t payload_start = json_str.find("\"payload\":\"");
    if (payload_start != std::string::npos) {
        payload_start += 11; // length of "\"payload\":\""
        size_t payload_end = json_str.find("\"", payload_start);
        if (payload_end != std::string::npos) {
            std::string encoded_payload = json_str.substr(payload_start, payload_end - payload_start);
            payload = base64_decode(encoded_payload);
        }
    }
    
    // Extract QoS
    size_t qos_start = json_str.find("\"qos\":");
    if (qos_start != std::string::npos) {
        qos_start += 6; // length of "\"qos\":"
        qos = static_cast<uint8_t>(std::stoi(json_str.substr(qos_start, 1)));
    } else {
        qos = 0;
    }
    
    // Extract retain
    retain = json_str.find("\"retain\":true") != std::string::npos;
    
    return 0;
}

int WebSocketMQTTBridge::parse_json_subscribe(const std::string& json_str, 
                                             std::string& topic_filter, uint8_t& qos) {
    // Extract topic_filter
    size_t topic_start = json_str.find("\"topic_filter\":\"");
    if (topic_start == std::string::npos) return -1;
    topic_start += 16; // length of "\"topic_filter\":\""
    
    size_t topic_end = json_str.find("\"", topic_start);
    if (topic_end == std::string::npos) return -1;
    
    topic_filter = json_str.substr(topic_start, topic_end - topic_start);
    
    // Extract QoS
    size_t qos_start = json_str.find("\"qos\":");
    if (qos_start != std::string::npos) {
        qos_start += 6; // length of "\"qos\":"
        qos = static_cast<uint8_t>(std::stoi(json_str.substr(qos_start, 1)));
    } else {
        qos = 0;
    }
    
    return 0;
}

int WebSocketMQTTBridge::parse_json_unsubscribe(const std::string& json_str, 
                                               std::string& topic_filter) {
    // Extract topic_filter
    size_t topic_start = json_str.find("\"topic_filter\":\"");
    if (topic_start == std::string::npos) return -1;
    topic_start += 16; // length of "\"topic_filter\":\""
    
    size_t topic_end = json_str.find("\"", topic_start);
    if (topic_end == std::string::npos) return -1;
    
    topic_filter = json_str.substr(topic_start, topic_end - topic_start);
    
    return 0;
}

std::string WebSocketMQTTBridge::generate_json_publish(const std::string& topic, 
                                                       const std::vector<uint8_t>& payload,
                                                       uint8_t qos, bool retain) {
    std::ostringstream json;
    json << "{";
    json << "\"type\":\"publish\",";
    json << "\"topic\":\"" << escape_json_string(topic) << "\",";
    json << "\"payload\":\"" << base64_encode(payload) << "\",";
    json << "\"qos\":" << static_cast<int>(qos) << ",";
    json << "\"retain\":" << (retain ? "true" : "false");
    json << "}";
    
    return json.str();
}

std::string WebSocketMQTTBridge::generate_json_response(const std::string& type, bool success,
                                                       const std::string& message) {
    std::ostringstream json;
    json << "{";
    json << "\"type\":\"" << escape_json_string(type) << "\",";
    json << "\"success\":" << (success ? "true" : "false");
    if (!message.empty()) {
        json << ",\"message\":\"" << escape_json_string(message) << "\"";
    }
    json << "}";
    
    return json.str();
}

int WebSocketMQTTBridge::forward_mqtt_publish(const std::string& client_id, 
                                             const mqtt::PublishPacket& packet) {
    WebSocketProtocolHandler* handler = get_websocket_handler(client_id);
    if (!handler) {
        return -1;
    }
    
    std::string topic(packet.topic_name.data(), packet.topic_name.size());
    std::vector<uint8_t> payload(packet.payload.begin(), packet.payload.end());
    
    return forward_mqtt_message(client_id, topic, payload, packet.qos, packet.retain);
}

int WebSocketMQTTBridge::forward_mqtt_message(const std::string& client_id, 
                                             const std::string& topic,
                                             const std::vector<uint8_t>& payload, 
                                             uint8_t qos, bool retain) {
    WebSocketProtocolHandler* handler = get_websocket_handler(client_id);
    if (!handler) {
        return -1;
    }
    
    int ret = 0;
    
    switch (message_format_) {
        case MessageFormat::JSON: {
            std::string json_message = generate_json_publish(topic, payload, qos, retain);
            ret = handler->send_text(json_message);
            break;
        }
        case MessageFormat::TEXT_PROTOCOL: {
            std::string text_message = "PUB " + topic + " " + std::to_string(qos) + " " +
                                     std::to_string(retain ? 1 : 0) + " " +
                                     std::string(payload.begin(), payload.end());
            ret = handler->send_text(text_message);
            break;
        }
        case MessageFormat::MQTT_PACKET: {
            // Convert to MQTT packet format and send as binary
            // This would require serializing the PublishPacket
            ret = handler->send_binary(payload);
            break;
        }
    }
    
    if (ret == 0) {
        stats_.websocket_messages_sent++;
    }
    
    return ret;
}

int WebSocketMQTTBridge::subscribe_topic(const std::string& client_id, 
                                        const std::string& topic_filter, uint8_t qos) {
    if (!session_manager_) {
        return -1;
    }
    
    mqtt::MQTTString mqtt_topic_filter(topic_filter.c_str(), topic_filter.length());
    mqtt::MQTTString mqtt_client_id(client_id.c_str(), client_id.length());
    
    int ret = session_manager_->subscribe_topic(mqtt_topic_filter, mqtt_client_id, qos);
    if (ret == 0) {
        // Track subscription
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        SubscriptionInfo sub_info;
        sub_info.topic_filter = topic_filter;
        sub_info.qos = qos;
        sub_info.subscribe_time = std::chrono::steady_clock::now();
        
        client_subscriptions_[client_id].push_back(sub_info);
        stats_.active_subscriptions++;
        
        LOG_DEBUG("Client {} subscribed to topic: {}", client_id, topic_filter);
    }
    
    return ret;
}

int WebSocketMQTTBridge::unsubscribe_topic(const std::string& client_id, 
                                          const std::string& topic_filter) {
    if (!session_manager_) {
        return -1;
    }
    
    mqtt::MQTTString mqtt_topic_filter(topic_filter.c_str(), topic_filter.length());
    mqtt::MQTTString mqtt_client_id(client_id.c_str(), client_id.length());
    
    int ret = session_manager_->unsubscribe_topic(mqtt_topic_filter, mqtt_client_id);
    if (ret == 0) {
        // Remove from tracking
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        auto it = client_subscriptions_.find(client_id);
        if (it != client_subscriptions_.end()) {
            auto& subscriptions = it->second;
            subscriptions.erase(
                std::remove_if(subscriptions.begin(), subscriptions.end(),
                              [&topic_filter](const SubscriptionInfo& sub) {
                                  return sub.topic_filter == topic_filter;
                              }),
                subscriptions.end());
            
            if (stats_.active_subscriptions > 0) {
                stats_.active_subscriptions--;
            }
        }
        
        LOG_DEBUG("Client {} unsubscribed from topic: {}", client_id, topic_filter);
    }
    
    return ret;
}

int WebSocketMQTTBridge::unsubscribe_all_topics(const std::string& client_id) {
    if (!session_manager_) {
        return -1;
    }
    
    mqtt::MQTTString mqtt_client_id(client_id.c_str(), client_id.length());
    int count = session_manager_->unsubscribe_all_topics(mqtt_client_id);
    
    // Remove from tracking
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    auto it = client_subscriptions_.find(client_id);
    if (it != client_subscriptions_.end()) {
        size_t removed_count = it->second.size();
        client_subscriptions_.erase(it);
        
        if (stats_.active_subscriptions >= removed_count) {
            stats_.active_subscriptions -= removed_count;
        } else {
            stats_.active_subscriptions = 0;
        }
    }
    
    LOG_DEBUG("Client {} unsubscribed from all topics", client_id);
    return count;
}

WebSocketProtocolHandler* WebSocketMQTTBridge::get_websocket_handler(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    auto it = websocket_handlers_.find(client_id);
    return (it != websocket_handlers_.end()) ? it->second : nullptr;
}

std::string WebSocketMQTTBridge::escape_json_string(const std::string& str) {
    std::string escaped;
    escaped.reserve(str.length() + 10);
    
    for (char c : str) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    
    return escaped;
}

std::string WebSocketMQTTBridge::base64_encode(const std::vector<uint8_t>& data) {
    // Simple base64 encoding
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    
    size_t i = 0;
    uint8_t char_array_3[3];
    uint8_t char_array_4[4];
    
    for (uint8_t byte : data) {
        char_array_3[i++] = byte;
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            
            for (i = 0; i < 4; i++) {
                result += chars[char_array_4[i]];
            }
            i = 0;
        }
    }
    
    if (i) {
        for (size_t j = i; j < 3; j++) {
            char_array_3[j] = '\0';
        }
        
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        
        for (size_t j = 0; j < i + 1; j++) {
            result += chars[char_array_4[j]];
        }
        
        while (i++ < 3) {
            result += '=';
        }
    }
    
    return result;
}

std::vector<uint8_t> WebSocketMQTTBridge::base64_decode(const std::string& encoded) {
    // Simple base64 decoding
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> result;
    
    size_t in_len = encoded.length();
    size_t i = 0;
    size_t in = 0;
    uint8_t char_array_4[4], char_array_3[3];
    
    while (in_len-- && (encoded[in] != '=') && 
           (std::isalnum(encoded[in]) || (encoded[in] == '+') || (encoded[in] == '/'))) {
        char_array_4[i++] = encoded[in]; in++;
        if (i == 4) {
            for (i = 0; i < 4; i++) {
                char_array_4[i] = chars.find(char_array_4[i]);
            }
            
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            
            for (i = 0; (i < 3); i++) {
                result.push_back(char_array_3[i]);
            }
            i = 0;
        }
    }
    
    if (i) {
        for (size_t j = i; j < 4; j++) {
            char_array_4[j] = 0;
        }
        
        for (size_t j = 0; j < 4; j++) {
            char_array_4[j] = chars.find(char_array_4[j]);
        }
        
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
        
        for (size_t j = 0; (j < i - 1); j++) {
            result.push_back(char_array_3[j]);
        }
    }
    
    return result;
}

int WebSocketMQTTBridge::handle_client_connect(const std::string& client_id, 
                                              const std::string& username) {
    LOG_INFO("WebSocket client connected: {} (username: {})", client_id, username);
    
    if (session_manager_) {
        mqtt::MQTTString mqtt_client_id(client_id.c_str(), client_id.length());
        mqtt::MQTTString mqtt_username(username.c_str(), username.length());
        
        // Trigger login event in session manager
        session_manager_->trigger_login_event(mqtt_client_id, mqtt_username, 
                                            mqtt::MQTTString("WebSocket", 9), 
                                            60, true);
    }
    
    return 0;
}

int WebSocketMQTTBridge::handle_client_disconnect(const std::string& client_id, 
                                                 const std::string& reason) {
    LOG_INFO("WebSocket client disconnected: {} (reason: {})", client_id, reason);
    
    // Clean up subscriptions
    unsubscribe_all_topics(client_id);
    
    // Unregister handler
    unregister_websocket_handler(client_id);
    
    if (session_manager_) {
        mqtt::MQTTString mqtt_client_id(client_id.c_str(), client_id.length());
        mqtt::MQTTString mqtt_reason(reason.c_str(), reason.length());
        
        // Trigger logout event in session manager
        session_manager_->trigger_logout_event(mqtt_client_id, mqtt_reason, 
                                             0, 0, 0, 0, 0);
    }
    
    return 0;
}

// Additional method implementations for text protocol and MQTT packet handling
int WebSocketMQTTBridge::handle_text_protocol_message(const std::string& client_id, 
                                                     const std::string& text_message) {
    // Parse text protocol commands: PUB, SUB, UNSUB
    std::istringstream iss(text_message);
    std::string command;
    iss >> command;
    
    if (command == "PUB") {
        std::string topic;
        std::vector<uint8_t> payload;
        uint8_t qos;
        bool retain;
        
        int ret = parse_text_publish(text_message, topic, payload, qos, retain);
        if (ret != 0) {
            stats_.translation_errors++;
            return ret;
        }
        
        // Forward to MQTT
        if (session_manager_) {
            mqtt::MQTTString mqtt_topic(topic.c_str(), topic.length());
            mqtt::MQTTString mqtt_client_id(client_id.c_str(), client_id.length());
            
            mqtt::PublishPacket packet;
            packet.topic_name = mqtt_topic;
            packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end());
            packet.qos = qos;
            packet.retain = retain;
            
            session_manager_->forward_publish_by_topic(mqtt_topic, packet, mqtt_client_id);
            stats_.mqtt_messages_sent++;
        }
        
    } else if (command == "SUB") {
        std::string topic_filter;
        uint8_t qos;
        
        int ret = parse_text_subscribe(text_message, topic_filter, qos);
        if (ret != 0) {
            stats_.translation_errors++;
            return ret;
        }
        
        return subscribe_topic(client_id, topic_filter, qos);
        
    } else if (command == "UNSUB") {
        std::string topic_filter;
        
        int ret = parse_text_unsubscribe(text_message, topic_filter);
        if (ret != 0) {
            stats_.translation_errors++;
            return ret;
        }
        
        return unsubscribe_topic(client_id, topic_filter);
    }
    
    return 0;
}

int WebSocketMQTTBridge::parse_text_publish(const std::string& text, std::string& topic,
                                           std::vector<uint8_t>& payload, uint8_t& qos, bool& retain) {
    std::istringstream iss(text);
    std::string command;
    std::string qos_str, retain_str;
    
    if (!(iss >> command >> topic >> qos_str >> retain_str)) {
        return -1;
    }
    
    qos = static_cast<uint8_t>(std::stoi(qos_str));
    retain = (std::stoi(retain_str) != 0);
    
    // Read the rest as payload
    std::string payload_str;
    std::getline(iss, payload_str);
    if (!payload_str.empty() && payload_str[0] == ' ') {
        payload_str = payload_str.substr(1); // Remove leading space
    }
    
    payload = std::vector<uint8_t>(payload_str.begin(), payload_str.end());
    
    return 0;
}

int WebSocketMQTTBridge::parse_text_subscribe(const std::string& text, 
                                             std::string& topic_filter, uint8_t& qos) {
    std::istringstream iss(text);
    std::string command;
    std::string qos_str;
    
    if (!(iss >> command >> topic_filter >> qos_str)) {
        return -1;
    }
    
    qos = static_cast<uint8_t>(std::stoi(qos_str));
    
    return 0;
}

int WebSocketMQTTBridge::parse_text_unsubscribe(const std::string& text, 
                                               std::string& topic_filter) {
    std::istringstream iss(text);
    std::string command;
    
    if (!(iss >> command >> topic_filter)) {
        return -1;
    }
    
    return 0;
}

int WebSocketMQTTBridge::handle_mqtt_packet_message(const std::string& client_id, 
                                                   const std::vector<uint8_t>& packet_data) {
    // This would parse the raw MQTT packet and handle it
    // For now, just log and increment stats
    LOG_DEBUG("Received MQTT packet from WebSocket client: {} (size: {})", 
              client_id, packet_data.size());
    stats_.mqtt_messages_received++;
    
    return 0;
}

} // namespace websocket