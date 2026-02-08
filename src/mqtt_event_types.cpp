#include "mqtt_event_types.h"
#include <random>
#include <sstream>
#include <iomanip>

namespace mqtt {
namespace events {

std::string generateEventId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    
    std::stringstream ss;
    ss << std::hex;
    
    auto now = std::chrono::high_resolution_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    
    ss << timestamp << "-";
    
    for (int i = 0; i < 8; ++i) {
        ss << dis(gen);
    }
    
    return ss.str();
}

EventMetadata createEventMetadata(const MQTTString& server_id, 
                                  const MQTTString& client_id,
                                  const MQTTString& client_ip,
                                  int client_port) {
    EventMetadata metadata;
    metadata.event_id = to_mqtt_string(generateEventId(), MQTTMemoryManager::get_instance().get_root_allocator());
    metadata.timestamp = std::chrono::system_clock::now();
    metadata.server_id = server_id;
    metadata.client_id = client_id;
    metadata.client_ip = client_ip;
    metadata.client_port = client_port;
    
    return metadata;
}

} // namespace events
} // namespace mqtt