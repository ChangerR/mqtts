#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_connection_watch.h"
#include "mqtt_define.h"
#include "mqtt_packet.h"
#include "mqtt_socket.h"
#include "mqtt_takeover.h"
#include "mqtt_stl_allocator.h"
#include "websocket_frame.h"

namespace websocket {

// Forward declarations
class WebSocketServer;
class WebSocketMQTTBridge;

// WebSocket connection state
enum class WebSocketState {
    CONNECTING,     // Waiting for handshake
    OPEN,          // Connection established
    CLOSING,       // Close frame sent/received
    CLOSED         // Connection closed
};

// WebSocket protocol handler statistics
struct WebSocketStatistics {
    std::atomic<uint64_t> frames_received{0};
    std::atomic<uint64_t> frames_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> text_frames{0};
    std::atomic<uint64_t> binary_frames{0};
    std::atomic<uint64_t> ping_frames{0};
    std::atomic<uint64_t> pong_frames{0};
    std::atomic<uint64_t> protocol_errors{0};

    void reset() {
        frames_received = 0;
        frames_sent = 0;
        bytes_received = 0;
        bytes_sent = 0;
        text_frames = 0;
        binary_frames = 0;
        ping_frames = 0;
        pong_frames = 0;
        protocol_errors = 0;
    }
};

// WebSocket protocol handler
class WebSocketProtocolHandler {
public:
    explicit WebSocketProtocolHandler(MQTTAllocator* allocator);
    ~WebSocketProtocolHandler();

    // Initialize with socket and client info
    int init(MQTTSocket* socket, const std::string& client_ip, int client_port);

    // Main processing loop (called in coroutine)
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
    int send_frame(const WebSocketFrame& frame);
    int send_text(const std::string& text);
    int send_binary(const std::vector<uint8_t>& data);
    int send_close(uint16_t code = 1000, const std::string& reason = "");
    int send_ping(const std::vector<uint8_t>& payload = {});
    int send_pong(const std::vector<uint8_t>& payload = {});

    // State management
    WebSocketState get_state() const { return state_; }
    bool is_connected() const { return state_ == WebSocketState::OPEN; }
    void close_connection();

    // Client info
    const std::string& get_client_id() const { return client_id_; }
    void set_client_id(const std::string& client_id) { client_id_ = client_id; }
    const std::string& get_client_ip() const { return client_ip_; }
    int get_client_port() const { return client_port_; }
    const std::string& get_selected_subprotocol() const { return selected_subprotocol_; }

    // Bridge integration
    void set_bridge(WebSocketMQTTBridge* bridge) { bridge_ = bridge; }
    WebSocketMQTTBridge* get_bridge() const { return bridge_; }

    void set_connection_established(std::chrono::steady_clock::time_point when)
    {
        established_ = when;
        established_set_ = true;
    }
    void observe_mqtt_connect(uint16_t negotiated_keep_alive);
    void observe_mqtt_packet();
    mqtt::ConnectionWatch& connection_watch() { return watch_; }
    void request_session_takeover(const std::shared_ptr<mqtt::SessionTakeoverGate>& gate);

    // Server integration
    void set_server(WebSocketServer* server) { server_ = server; }
    WebSocketServer* get_server() const { return server_; }

    // Statistics
    WebSocketStatistics& get_statistics() { return stats_; }
    const WebSocketStatistics& get_statistics() const { return stats_; }

    // Ping/Pong management
    int send_periodic_ping();
    bool should_send_ping() const;
    bool is_pong_timeout() const;

    // Allocator
    MQTTAllocator* get_allocator() const { return allocator_; }

private:
    // Internal helpers
    int read_handshake_request(std::string& request);
    int parse_handshake_request(const std::string& request, std::string& ws_key);
    int send_handshake_response(const std::string& ws_key);
    std::string compute_accept_key(const std::string& ws_key);

    int read_frame(WebSocketFrame& frame);
    int write_frame_data(const std::vector<uint8_t>& data);

    // State
    MQTTAllocator* allocator_;
    MQTTSocket* socket_;
    WebSocketState state_;
    std::string client_id_;
    std::string client_ip_;
    std::string selected_subprotocol_;
    int client_port_;

    // Components
    WebSocketFrameParser parser_;
    WebSocketMQTTBridge* bridge_;
    WebSocketServer* server_;

    // Buffers
    using Buffer = std::vector<uint8_t, mqtt::mqtt_stl_allocator<uint8_t>>;
    Buffer read_buffer_;
    size_t read_buffer_offset_;

    // Ping/Pong tracking
    std::chrono::steady_clock::time_point last_ping_sent_;
    std::chrono::steady_clock::time_point last_pong_received_;
    bool waiting_for_pong_;

    // Configuration
    int ping_interval_ms_;      // Ping interval in milliseconds
    int pong_timeout_ms_;       // Pong timeout in milliseconds

    // Statistics
    WebSocketStatistics stats_;

    // Fragmentation state for multi-frame messages
    WebSocketOpcode fragmented_opcode_;
    Buffer fragmented_payload_;
    bool is_fragmenting_;

    // MQTT 子协议下二进制帧的载荷边到边交给桥。WS 帧和消息边界对 MQTT 没有意义，
    // 桥按 MQTT 报文切分并按 max_packet_size 限长，所以这里既不攒整帧也不攒整条消息。
    uint64_t stream_remaining_ = 0;
    uint8_t stream_mask_[4] = {0, 0, 0, 0};
    size_t stream_mask_offset_ = 0;
    bool stream_masked_ = false;

    mqtt::ConnectionWatch watch_;
    std::chrono::steady_clock::time_point established_{};
    bool established_set_ = false;
    std::shared_ptr<mqtt::SessionTakeoverGate> takeover_gate_;
    std::atomic<bool> takeover_pending_{false};
    bool takeover_finished_ = false;

    void arm_connection_watch();
    int64_t read_deadline_ms() const;
    int reject_oversized_mqtt_fragment(size_t incoming);
    bool streams_mqtt_payload(const WebSocketFrame& header) const;
    int begin_streamed_frame(const WebSocketFrame& header, size_t header_len);
    int pump_streamed_payload();
    void compact_read_buffer();
    void finish_session_takeover();
    void signal_takeover_done();
};

}  // namespace websocket
