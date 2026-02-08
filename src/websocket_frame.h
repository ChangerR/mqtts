#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_define.h"
#include "mqtt_stl_allocator.h"

namespace websocket {

// WebSocket error codes (-1100 to -1199)
#define MQ_ERR_WS_PROTOCOL -1100
#define MQ_ERR_WS_INVALID_FRAME -1101
#define MQ_ERR_WS_FRAME_TOO_LARGE -1102
#define MQ_ERR_WS_INVALID_OPCODE -1103
#define MQ_ERR_WS_FRAGMENTATION -1104
#define MQ_ERR_WS_PAYLOAD_LENGTH -1105
#define MQ_ERR_WS_INCOMPLETE_FRAME -1106
#define MQ_ERR_WS_RESERVED_BITS -1107
#define MQ_ERR_WS_CLOSE_CODE -1108
#define MQ_ERR_WS_INVALID_UTF8 -1109
#define MQ_ERR_WS_HANDSHAKE -1110

// WebSocket opcodes (RFC 6455)
enum class WebSocketOpcode : uint8_t {
    CONTINUATION = 0x0,  // Continuation frame
    TEXT = 0x1,          // Text frame
    BINARY = 0x2,        // Binary frame
    // 0x3-0x7 reserved for future non-control frames
    CLOSE = 0x8,         // Connection close
    PING = 0x9,          // Ping
    PONG = 0xA,          // Pong
    // 0xB-0xF reserved for future control frames
};

// WebSocket close status codes (RFC 6455)
enum class WebSocketCloseCode : uint16_t {
    NORMAL = 1000,              // Normal closure
    GOING_AWAY = 1001,          // Going away
    PROTOCOL_ERROR = 1002,      // Protocol error
    UNSUPPORTED_DATA = 1003,    // Unsupported data
    RESERVED = 1004,            // Reserved
    NO_STATUS_RECEIVED = 1005,  // No status received (not sent)
    ABNORMAL_CLOSURE = 1006,    // Abnormal closure (not sent)
    INVALID_PAYLOAD = 1007,     // Invalid frame payload data
    POLICY_VIOLATION = 1008,    // Policy violation
    MESSAGE_TOO_BIG = 1009,     // Message too big
    MANDATORY_EXT = 1010,       // Mandatory extension
    INTERNAL_ERROR = 1011,      // Internal server error
    SERVICE_RESTART = 1012,     // Service restart
    TRY_AGAIN_LATER = 1013,     // Try again later
    TLS_HANDSHAKE = 1015,       // TLS handshake (not sent)
};

// WebSocket frame structure (RFC 6455)
struct WebSocketFrame {
    // Frame header
    bool fin;                    // Final fragment flag
    bool rsv1;                   // Reserved bit 1
    bool rsv2;                   // Reserved bit 2
    bool rsv3;                   // Reserved bit 3
    WebSocketOpcode opcode;      // Opcode
    bool masked;                 // Mask flag
    uint64_t payload_length;     // Payload length
    uint8_t masking_key[4];      // Masking key (if masked)

    // Frame payload
    using PayloadVector = std::vector<uint8_t, mqtt::mqtt_stl_allocator<uint8_t>>;
    PayloadVector payload;

    // Constructor
    WebSocketFrame(MQTTAllocator* allocator = nullptr);

    // Helper methods
    bool is_control_frame() const;
    bool is_data_frame() const;
    void clear();

    // Get/set close code and reason from payload
    int get_close_info(uint16_t& code, std::string& reason) const;
    int set_close_info(uint16_t code, const std::string& reason);
};

// WebSocket frame parser and serializer
class WebSocketFrameParser {
public:
    explicit WebSocketFrameParser(MQTTAllocator* allocator, size_t max_payload_size = 10 * 1024 * 1024);
    ~WebSocketFrameParser();

    // Parse a frame from buffer
    // Returns:
    //   MQ_SUCCESS: frame parsed successfully, bytes_consumed indicates consumed bytes
    //   MQ_ERR_WS_INCOMPLETE_FRAME: need more data
    //   Other negative values: parsing error
    int parse_frame(const uint8_t* data, size_t data_len, WebSocketFrame& frame, size_t& bytes_consumed);

    // Serialize a frame to buffer
    // Returns:
    //   MQ_SUCCESS: success
    //   Negative value: error
    int serialize_frame(const WebSocketFrame& frame, std::vector<uint8_t>& output);

    // Apply/remove masking
    static void apply_mask(uint8_t* data, size_t data_len, const uint8_t* mask);

    // Validate frame
    static int validate_frame(const WebSocketFrame& frame);

    // Check if opcode is valid
    static bool is_valid_opcode(uint8_t opcode);

    // Check if UTF-8 is valid
    static bool is_valid_utf8(const uint8_t* data, size_t len);

    // Get allocator
    MQTTAllocator* get_allocator() const { return allocator_; }

private:
    MQTTAllocator* allocator_;
    size_t max_payload_size_;

    // Internal parsing helpers
    int parse_header(const uint8_t* data, size_t data_len, WebSocketFrame& frame, size_t& header_len);
    int parse_payload(const uint8_t* data, size_t data_len, WebSocketFrame& frame, size_t header_len, size_t& bytes_consumed);
};

// Helper functions for opcode names
const char* opcode_to_string(WebSocketOpcode opcode);
const char* close_code_to_string(WebSocketCloseCode code);

}  // namespace websocket
