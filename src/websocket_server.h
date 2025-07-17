#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "co_routine.h"
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_config.h"
#include "mqtt_define.h"
#include "mqtt_session_manager_v2.h"
#include "mqtt_socket.h"
#include "websocket_protocol_handler.h"

namespace websocket {

struct WebSocketConfig {
    std::string bind_address = "127.0.0.1";
    int port = 8080;
    int max_connections = 10000;
    int thread_count = 2;
    int backlog = 128;
    
    // WebSocket specific settings
    size_t max_frame_size = 1048576;  // 1MB
    size_t max_message_size = 10485760;  // 10MB
    int handshake_timeout = 10;  // seconds
    int ping_interval = 30;  // seconds
    int pong_timeout = 10;  // seconds
};

struct WebSocketClientContext {
    WebSocketServer* server;
    MQTTSocket* client;
    std::string client_id;
    std::string client_ip;
    int client_port;
    MQTTAllocator* allocator;
    std::chrono::steady_clock::time_point connect_time;
};

class WebSocketServer {
public:
    WebSocketServer(const WebSocketConfig& config, const mqtt::MemoryConfig& memory_config);
    virtual ~WebSocketServer();
    
    // Server lifecycle
    int start();
    void run();
    void stop();
    bool is_running() const { return running_; }
    
    // Connection management
    bool can_accept_connection() const;
    int add_connection();
    int remove_connection();
    int get_connection_count() const { return current_connections_.load(); }
    
    // Session manager integration
    void set_session_manager(mqtt::GlobalSessionManager* session_manager) {
        session_manager_ = session_manager;
    }
    mqtt::GlobalSessionManager* get_session_manager() const { 
        return session_manager_; 
    }
    
    // Configuration
    const WebSocketConfig& get_config() const { return config_; }
    
    // Statistics
    struct Statistics {
        std::atomic<uint64_t> total_connections{0};
        std::atomic<uint64_t> active_connections{0};
        std::atomic<uint64_t> total_messages{0};
        std::atomic<uint64_t> total_bytes_sent{0};
        std::atomic<uint64_t> total_bytes_received{0};
        std::atomic<uint64_t> handshake_errors{0};
        std::atomic<uint64_t> protocol_errors{0};
    };
    
    Statistics& get_statistics() { return stats_; }
    const Statistics& get_statistics() const { return stats_; }
    
private:
    // Coroutine callbacks
    static void* accept_routine(void* arg);
    static void* client_routine(void* arg);
    static void handle_client(WebSocketClientContext* ctx);
    
    // Event loop callback for pending message processing
    static int eventloop_callback(void* arg);
    
    // Helper methods
    int initialize_server_socket();
    void cleanup_resources();
    
private:
    // Server configuration
    WebSocketConfig config_;
    mqtt::MemoryConfig memory_config_;
    
    // Network components
    stCoRoutine_t* accept_co_;
    MQTTSocket* server_socket_;
    std::atomic<bool> running_;
    
    // Connection management
    std::atomic<int> current_connections_;
    
    // Session manager integration
    mqtt::GlobalSessionManager* session_manager_;
    
    // Statistics
    Statistics stats_;
    
    // Memory management
    MQTTAllocator* server_allocator_;
};

} // namespace websocket