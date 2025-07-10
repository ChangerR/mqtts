#ifndef MQTT_EVENT_TYPES_H
#define MQTT_EVENT_TYPES_H

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "mqtt_parser.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {
namespace events {

// Event types enumeration
enum class EventType {
    LOGIN = 1,
    LOGOUT = 2,
    PUBLISH = 3
};

// Base event metadata
struct EventMetadata {
    MQTTString event_id;
    std::chrono::system_clock::time_point timestamp;
    MQTTString server_id;
    MQTTString client_id;
    MQTTString client_ip;
    int client_port;
    
    EventMetadata() : client_port(0) {}
};

// Login event data
struct LoginEventData {
    EventMetadata metadata;
    MQTTString username;
    MQTTString protocol_version;
    int keep_alive;
    bool clean_session;
    MQTTMap<MQTTString, MQTTString> properties;
    
    LoginEventData() : keep_alive(0), clean_session(false) {}
};

// Logout event data
struct LogoutEventData {
    EventMetadata metadata;
    MQTTString reason;
    int session_duration_seconds;
    int64_t bytes_sent;
    int64_t bytes_received;
    int messages_sent;
    int messages_received;
    MQTTMap<MQTTString, MQTTString> properties;
    
    LogoutEventData() : session_duration_seconds(0), bytes_sent(0), bytes_received(0), 
                        messages_sent(0), messages_received(0) {}
};

// Publish event data
struct PublishEventData {
    EventMetadata metadata;
    MQTTString topic;
    int qos;
    bool retain;
    bool duplicate;
    int payload_size;
    MQTTVector<uint8_t> payload;  // Optional, can be empty to save bandwidth
    MQTTMap<MQTTString, MQTTString> properties;
    
    PublishEventData() : qos(0), retain(false), duplicate(false), payload_size(0) {}
};

// Generic event container
class MQTTEvent {
public:
    EventType type;
    std::shared_ptr<void> data;
    
    MQTTEvent(EventType t, std::shared_ptr<void> d) : type(t), data(d) {}
    
    // Type-safe getters
    std::shared_ptr<LoginEventData> getLoginData() const {
        return (type == EventType::LOGIN) ? std::static_pointer_cast<LoginEventData>(data) : nullptr;
    }
    
    std::shared_ptr<LogoutEventData> getLogoutData() const {
        return (type == EventType::LOGOUT) ? std::static_pointer_cast<LogoutEventData>(data) : nullptr;
    }
    
    std::shared_ptr<PublishEventData> getPublishData() const {
        return (type == EventType::PUBLISH) ? std::static_pointer_cast<PublishEventData>(data) : nullptr;
    }
};

// Event batch for efficient transmission
struct EventBatch {
    MQTTVector<MQTTEvent> events;
    std::chrono::system_clock::time_point batch_timestamp;
    
    EventBatch() : batch_timestamp(std::chrono::system_clock::now()) {}
    
    size_t size() const { return events.size(); }
    bool empty() const { return events.empty(); }
    void clear() { events.clear(); }
    
    void addEvent(const MQTTEvent& event) {
        events.push_back(event);
    }
};

// Factory functions for creating events
inline std::shared_ptr<MQTTEvent> createLoginEvent(const LoginEventData& data) {
    return std::make_shared<MQTTEvent>(EventType::LOGIN, std::make_shared<LoginEventData>(data));
}

inline std::shared_ptr<MQTTEvent> createLogoutEvent(const LogoutEventData& data) {
    return std::make_shared<MQTTEvent>(EventType::LOGOUT, std::make_shared<LogoutEventData>(data));
}

inline std::shared_ptr<MQTTEvent> createPublishEvent(const PublishEventData& data) {
    return std::make_shared<MQTTEvent>(EventType::PUBLISH, std::make_shared<PublishEventData>(data));
}

// Helper function to generate unique event ID
std::string generateEventId();

// Helper function to create event metadata
EventMetadata createEventMetadata(const MQTTString& server_id, 
                                  const MQTTString& client_id,
                                  const MQTTString& client_ip = MQTTString(),
                                  int client_port = 0);

} // namespace events
} // namespace mqtt

#endif // MQTT_EVENT_TYPES_H