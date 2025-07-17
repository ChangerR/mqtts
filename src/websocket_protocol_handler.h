#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_coroutine_utils.h"
#include "mqtt_define.h"
#include "mqtt_session_manager_v2.h"
#include "mqtt_socket.h"
#include "mqtt_stl_allocator.h"
#include "websocket_frame.h"

namespace websocket {

// Forward declaration
class WebSocketServer;

enum class WebSocketState {
    CONNECTING,
    OPEN,
    CLOSING,
    CLOSED
};

class WebSocketProtocolHandler {
public:
    WebSocketProtocolHandler(MQTTAllocator* allocator);
    ~WebSocketProtocolHandler();
    
    // Initialize handler with socket
    int init(MQTTSocket* socket, const std::string& client_ip, int client_port);
    
    // Main processing loop
    int process();
    
    // WebSocket handshake
    int handle_handshake();
    
    // Frame handlers
    int handle_frame(const WebSocketFrame& frame);
    int handle_text_frame(const WebSocketFrame& frame);
    int handle_binary_frame(const WebSocketFrame& frame);
    int handle_close_frame(const WebSocketFrame& frame);
    int handle_ping_frame(const WebSocketFrame& frame);
    int handle_pong_frame(const WebSocketFrame& frame);
    
    // Send methods
    int send_text(const std::string& text);
    int send_binary(const std::vector<uint8_t>& data);
    int send_close(uint16_t code = 1000, const std::string& reason = "");
    int send_ping(const std::vector<uint8_t>& payload = {});
    int send_pong(const std::vector<uint8_t>& payload = {});
    
    // MQTT integration
    int send_mqtt_message(const std::string& topic, const std::vector<uint8_t>& payload, 
                         uint8_t qos = 0, bool retain = false);
    int handle_mqtt_subscribe(const std::string& topic_filter, uint8_t qos = 0);
    int handle_mqtt_unsubscribe(const std::string& topic_filter);
    
    // Session management
    bool is_connected() const { return state_ == WebSocketState::OPEN; }
    void set_client_id(const std::string& client_id) { client_id_ = client_id; }
    const std::string& get_client_id() const { return client_id_; }
    
    // Server integration
    void set_server(WebSocketServer* server) { server_ = server; }
    WebSocketServer* get_server() const { return server_; }
    
    // Session manager integration
    void set_session_manager(mqtt::GlobalSessionManager* session_manager) {
        session_manager_ = session_manager;
    }
    mqtt::GlobalSessionManager* get_session_manager() const { 
        return session_manager_; 
    }
    
    // Statistics
    struct Statistics {
        std::atomic<uint64_t> frames_received{0};
        std::atomic<uint64_t> frames_sent{0};
        std::atomic<uint64_t> bytes_received{0};
        std::atomic<uint64_t> bytes_sent{0};
        std::atomic<uint64_t> text_frames{0};
        std::atomic<uint64_t> binary_frames{0};
        std::atomic<uint64_t> control_frames{0};
        std::atomic<uint64_t> ping_frames{0};
        std::atomic<uint64_t> pong_frames{0};
        std::atomic<uint64_t> close_frames{0};
        std::atomic<uint64_t> protocol_errors{0};
    };
    
    Statistics& get_statistics() { return stats_; }
    const Statistics& get_statistics() const { return stats_; }
    
private:
    // Buffer management
    int ensure_buffer_size(size_t needed_size);
    int read_frame_header();
    int read_frame_payload(WebSocketFrame& frame);
    
    // WebSocket protocol helpers
    int send_frame(const WebSocketFrame& frame);
    int generate_websocket_key_response(const std::string& key, std::string& response);
    int parse_http_request(const std::string& request, 
                          std::unordered_map<std::string, std::string>& headers);
    
    // Connection management
    int close_connection(uint16_t code = 1000, const std::string& reason = "");
    void cleanup_session_registration();
    int register_session_with_manager();
    
    // Ping/Pong management
    int start_ping_timer();
    int handle_ping_timeout();
    
    // Buffer management
    char* buffer_;
    size_t current_buffer_size_;
    size_t bytes_read_;
    
    // Socket and connection info
    MQTTSocket* socket_;
    std::string client_ip_;
    int client_port_;
    std::string client_id_;
    WebSocketState state_;
    
    // WebSocket protocol state
    bool handshake_completed_;
    std::string websocket_key_;
    std::string websocket_protocol_;
    std::string websocket_extensions_;
    
    // Ping/Pong management
    std::chrono::steady_clock::time_point last_ping_time_;
    std::chrono::steady_clock::time_point last_pong_time_;
    bool ping_pending_;
    
    // Memory management
    MQTTAllocator* allocator_;
    
    // Server reference
    WebSocketServer* server_;
    
    // Session manager
    mqtt::GlobalSessionManager* session_manager_;
    
    // Statistics
    Statistics stats_;
    
    // Write synchronization
    mutable std::atomic<bool> write_lock_acquired_;
    mutable CoroCondition write_lock_condition_;
    
    // Topic subscriptions (for MQTT integration)
    std::vector<std::string> subscriptions_;
    
    // Frame parsing state
    enum class FrameReadState {
        READ_HEADER,
        READ_PAYLOAD
    };
    FrameReadState frame_read_state_;
    size_t frame_bytes_needed_;
};

} // namespace websocket