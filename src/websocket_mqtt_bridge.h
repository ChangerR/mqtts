#pragma once

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_define.h"
#include "mqtt_packet.h"
#include "mqtt_session_manager_v2.h"
#include "mqtt_stl_allocator.h"

namespace websocket {

// Forward declarations
class WebSocketProtocolHandler;

// Message format for WebSocket-MQTT bridge
enum class MessageFormat {
    JSON,           // {"topic": "...", "payload": "...", "qos": 0}
    MQTT_PACKET,    // Direct MQTT packet binary format
    TEXT_PROTOCOL   // Simple text: "PUBLISH:topic:payload"
};

// Bridge statistics
struct BridgeStatistics {
    std::atomic<uint64_t> ws_messages_received{0};
    std::atomic<uint64_t> ws_messages_sent{0};
    std::atomic<uint64_t> mqtt_messages_received{0};
    std::atomic<uint64_t> mqtt_messages_sent{0};
    std::atomic<uint64_t> translation_errors{0};
    std::atomic<uint64_t> active_subscriptions{0};

    void reset() {
        ws_messages_received = 0;
        ws_messages_sent = 0;
        mqtt_messages_received = 0;
        mqtt_messages_sent = 0;
        translation_errors = 0;
        active_subscriptions = 0;
    }
};

// Subscription info
struct SubscriptionInfo {
    std::string topic_filter;
    uint8_t qos;

    SubscriptionInfo(const std::string& topic = "", uint8_t q = 0)
        : topic_filter(topic), qos(q) {}
};

// WebSocket MQTT Bridge
class WebSocketMQTTBridge {
public:
    explicit WebSocketMQTTBridge(MQTTAllocator* allocator, MessageFormat format = MessageFormat::JSON);
    ~WebSocketMQTTBridge();

    // Initialize with session manager
    int init(mqtt::GlobalSessionManager* session_manager);

    // Register/unregister WebSocket handlers
    int register_handler(const std::string& client_id, WebSocketProtocolHandler* handler);
    int unregister_handler(const std::string& client_id);
    WebSocketProtocolHandler* get_handler(const std::string& client_id) const;

    // Handle incoming WebSocket messages
    int handle_websocket_text(const std::string& client_id, const std::string& message);
    int handle_websocket_binary(const std::string& client_id, const std::vector<uint8_t>& data);

    // Handle incoming MQTT messages (from session manager)
    int handle_mqtt_publish(const std::string& client_id, const mqtt::PublishPacket& packet);

    // Topic subscription management
    int subscribe_topic(const std::string& client_id, const std::string& topic_filter, uint8_t qos = 0);
    int unsubscribe_topic(const std::string& client_id, const std::string& topic_filter);

    // Publish MQTT message
    int publish_message(const std::string& client_id, const std::string& topic,
                       const std::vector<uint8_t>& payload, uint8_t qos = 0, bool retain = false);

    // Configuration
    void set_message_format(MessageFormat format) { format_ = format; }
    MessageFormat get_message_format() const { return format_; }

    // Statistics
    BridgeStatistics& get_statistics() { return stats_; }
    const BridgeStatistics& get_statistics() const { return stats_; }

    // Session manager
    mqtt::GlobalSessionManager* get_session_manager() const { return session_manager_; }

private:
    // JSON message parsing/formatting
    int parse_json_message(const std::string& json, std::string& topic,
                          std::vector<uint8_t>& payload, uint8_t& qos, bool& retain);
    std::string format_json_message(const std::string& topic, const std::vector<uint8_t>& payload,
                                   uint8_t qos, bool retain);

    // Text protocol parsing/formatting
    int parse_text_protocol(const std::string& text, std::string& topic,
                           std::vector<uint8_t>& payload);
    std::string format_text_protocol(const std::string& topic, const std::vector<uint8_t>& payload);

    // Binary MQTT packet handling
    int parse_mqtt_packet(const std::vector<uint8_t>& data, mqtt::Packet*& packet);
    int handle_mqtt_connect(const std::string& client_id, const mqtt::ConnectPacket* packet);
    int handle_mqtt_subscribe(const std::string& client_id, const mqtt::SubscribePacket* packet);
    int handle_mqtt_unsubscribe(const std::string& client_id, const mqtt::UnsubscribePacket* packet);
    int handle_mqtt_publish_packet(const std::string& client_id, const mqtt::PublishPacket* packet);
    int handle_mqtt_pingreq(const std::string& client_id);
    int handle_mqtt_disconnect(const std::string& client_id);

    // Send response to WebSocket client
    int send_to_websocket(const std::string& client_id, const std::string& text);
    int send_to_websocket_binary(const std::string& client_id, const std::vector<uint8_t>& data);

    // Base64 encode/decode for binary payloads in JSON
    static std::string base64_encode(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> base64_decode(const std::string& encoded);

    MQTTAllocator* allocator_;
    MessageFormat format_;
    mqtt::GlobalSessionManager* session_manager_;

    // Handler registry
    using HandlerMap = std::unordered_map<std::string, WebSocketProtocolHandler*,
                                          std::hash<std::string>,
                                          std::equal_to<std::string>,
                                          mqtt::mqtt_stl_allocator<std::pair<const std::string, WebSocketProtocolHandler*>>>;
    HandlerMap handlers_;

    // Subscription tracking
    using SubscriptionMap = std::unordered_map<std::string, std::vector<SubscriptionInfo, mqtt::mqtt_stl_allocator<SubscriptionInfo>>,
                                               std::hash<std::string>,
                                               std::equal_to<std::string>,
                                               mqtt::mqtt_stl_allocator<std::pair<const std::string, std::vector<SubscriptionInfo, mqtt::mqtt_stl_allocator<SubscriptionInfo>>>>>;
    SubscriptionMap subscriptions_;

    // Statistics
    BridgeStatistics stats_;
};

}  // namespace websocket
