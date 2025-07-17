#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_define.h"
#include "mqtt_packet.h"
#include "mqtt_session_manager_v2.h"
#include "mqtt_stl_allocator.h"
#include "websocket_protocol_handler.h"

namespace websocket {

/**
 * @brief WebSocket-MQTT bridge that handles protocol translation
 * 
 * This class provides bidirectional communication between WebSocket clients
 * and the MQTT broker system. It translates WebSocket messages to MQTT
 * packets and vice versa.
 */
class WebSocketMQTTBridge {
public:
    WebSocketMQTTBridge(MQTTAllocator* allocator);
    ~WebSocketMQTTBridge();
    
    // Initialize bridge with session manager
    int init(mqtt::GlobalSessionManager* session_manager);
    
    // Register/unregister WebSocket handler
    int register_websocket_handler(const std::string& client_id, WebSocketProtocolHandler* handler);
    int unregister_websocket_handler(const std::string& client_id);
    
    // Message translation: WebSocket -> MQTT
    int handle_websocket_message(const std::string& client_id, const std::string& message);
    int handle_websocket_binary(const std::string& client_id, const std::vector<uint8_t>& data);
    
    // Message translation: MQTT -> WebSocket
    int forward_mqtt_publish(const std::string& client_id, const mqtt::PublishPacket& packet);
    int forward_mqtt_message(const std::string& client_id, const std::string& topic, 
                           const std::vector<uint8_t>& payload, uint8_t qos = 0, bool retain = false);
    
    // Subscription management
    int subscribe_topic(const std::string& client_id, const std::string& topic_filter, uint8_t qos = 0);
    int unsubscribe_topic(const std::string& client_id, const std::string& topic_filter);
    int unsubscribe_all_topics(const std::string& client_id);
    
    // Client management
    int handle_client_connect(const std::string& client_id, const std::string& username = "");
    int handle_client_disconnect(const std::string& client_id, const std::string& reason = "");
    
    // Protocol formats
    enum class MessageFormat {
        JSON,           // JSON format messages
        MQTT_PACKET,    // Direct MQTT packet format
        TEXT_PROTOCOL   // Simple text protocol
    };
    
    void set_message_format(MessageFormat format) { message_format_ = format; }
    MessageFormat get_message_format() const { return message_format_; }
    
    // Statistics
    struct Statistics {
        std::atomic<uint64_t> websocket_messages_received{0};
        std::atomic<uint64_t> websocket_messages_sent{0};
        std::atomic<uint64_t> mqtt_messages_received{0};
        std::atomic<uint64_t> mqtt_messages_sent{0};
        std::atomic<uint64_t> translation_errors{0};
        std::atomic<uint64_t> active_subscriptions{0};
        std::atomic<uint64_t> active_clients{0};
    };
    
    Statistics& get_statistics() { return stats_; }
    const Statistics& get_statistics() const { return stats_; }
    
private:
    // Message format handlers
    int handle_json_message(const std::string& client_id, const std::string& json_message);
    int handle_mqtt_packet_message(const std::string& client_id, const std::vector<uint8_t>& packet_data);
    int handle_text_protocol_message(const std::string& client_id, const std::string& text_message);
    
    // JSON message parsers
    int parse_json_publish(const std::string& json_str, std::string& topic, 
                          std::vector<uint8_t>& payload, uint8_t& qos, bool& retain);
    int parse_json_subscribe(const std::string& json_str, std::string& topic_filter, uint8_t& qos);
    int parse_json_unsubscribe(const std::string& json_str, std::string& topic_filter);
    
    // JSON message generators
    std::string generate_json_publish(const std::string& topic, const std::vector<uint8_t>& payload, 
                                     uint8_t qos, bool retain);
    std::string generate_json_response(const std::string& type, bool success, const std::string& message = "");
    
    // Text protocol parsers
    int parse_text_publish(const std::string& text, std::string& topic, 
                          std::vector<uint8_t>& payload, uint8_t& qos, bool& retain);
    int parse_text_subscribe(const std::string& text, std::string& topic_filter, uint8_t& qos);
    int parse_text_unsubscribe(const std::string& text, std::string& topic_filter);
    
    // Client management
    WebSocketProtocolHandler* get_websocket_handler(const std::string& client_id);
    
    // Subscription tracking
    struct SubscriptionInfo {
        std::string topic_filter;
        uint8_t qos;
        std::chrono::steady_clock::time_point subscribe_time;
    };
    
    // Memory management
    MQTTAllocator* allocator_;
    
    // Session manager
    mqtt::GlobalSessionManager* session_manager_;
    
    // Handler registry
    mutable std::mutex handlers_mutex_;
    std::unordered_map<std::string, WebSocketProtocolHandler*> websocket_handlers_;
    
    // Subscription tracking
    mutable std::mutex subscriptions_mutex_;
    std::unordered_map<std::string, std::vector<SubscriptionInfo>> client_subscriptions_;
    
    // Configuration
    MessageFormat message_format_;
    
    // Statistics
    Statistics stats_;
    
    // Helper methods
    std::string escape_json_string(const std::string& str);
    std::string unescape_json_string(const std::string& str);
    std::vector<uint8_t> base64_decode(const std::string& encoded);
    std::string base64_encode(const std::vector<uint8_t>& data);
};

} // namespace websocket