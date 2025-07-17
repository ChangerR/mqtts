#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace websocket {

enum class WebSocketOpcode : uint8_t {
    CONTINUATION = 0x0,
    TEXT = 0x1,
    BINARY = 0x2,
    CLOSE = 0x8,
    PING = 0x9,
    PONG = 0xA
};

enum class WebSocketCloseCode : uint16_t {
    NORMAL_CLOSURE = 1000,
    GOING_AWAY = 1001,
    PROTOCOL_ERROR = 1002,
    UNSUPPORTED_DATA = 1003,
    NO_STATUS_RCVD = 1005,
    ABNORMAL_CLOSURE = 1006,
    INVALID_FRAME_PAYLOAD_DATA = 1007,
    POLICY_VIOLATION = 1008,
    MESSAGE_TOO_BIG = 1009,
    MANDATORY_EXTENSION = 1010,
    INTERNAL_SERVER_ERROR = 1011,
    SERVICE_RESTART = 1012,
    TRY_AGAIN_LATER = 1013,
    BAD_GATEWAY = 1014,
    TLS_HANDSHAKE = 1015
};

struct WebSocketFrame {
    bool fin;
    bool rsv1;
    bool rsv2;
    bool rsv3;
    WebSocketOpcode opcode;
    bool masked;
    uint64_t payload_length;
    uint8_t masking_key[4];
    std::vector<uint8_t> payload;
    
    WebSocketFrame() : fin(false), rsv1(false), rsv2(false), rsv3(false), 
                      opcode(WebSocketOpcode::TEXT), masked(false), payload_length(0) {
        masking_key[0] = masking_key[1] = masking_key[2] = masking_key[3] = 0;
    }
    
    // Helper methods
    bool is_control_frame() const {
        return opcode == WebSocketOpcode::CLOSE || 
               opcode == WebSocketOpcode::PING || 
               opcode == WebSocketOpcode::PONG;
    }
    
    bool is_data_frame() const {
        return opcode == WebSocketOpcode::TEXT || 
               opcode == WebSocketOpcode::BINARY || 
               opcode == WebSocketOpcode::CONTINUATION;
    }
    
    std::string get_opcode_name() const {
        switch (opcode) {
            case WebSocketOpcode::CONTINUATION: return "CONTINUATION";
            case WebSocketOpcode::TEXT: return "TEXT";
            case WebSocketOpcode::BINARY: return "BINARY";
            case WebSocketOpcode::CLOSE: return "CLOSE";
            case WebSocketOpcode::PING: return "PING";
            case WebSocketOpcode::PONG: return "PONG";
            default: return "UNKNOWN";
        }
    }
    
    // Apply/remove masking
    void apply_mask() {
        if (masked && !payload.empty()) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] ^= masking_key[i % 4];
            }
        }
    }
};

class WebSocketFrameParser {
public:
    WebSocketFrameParser();
    ~WebSocketFrameParser();
    
    // Parse frame from buffer
    int parse_frame(const char* buffer, size_t buffer_size, WebSocketFrame& frame, size_t& bytes_consumed);
    
    // Serialize frame to buffer
    int serialize_frame(const WebSocketFrame& frame, std::vector<uint8_t>& buffer);
    
    // Validate frame
    bool is_valid_frame(const WebSocketFrame& frame) const;
    
    // Get expected frame size from header
    int get_frame_size(const char* buffer, size_t buffer_size, size_t& frame_size);
    
private:
    // Helper methods
    int parse_header(const char* buffer, size_t buffer_size, WebSocketFrame& frame, size_t& header_size);
    int parse_payload_length(const char* buffer, size_t buffer_size, size_t& offset, uint64_t& payload_length);
    int parse_masking_key(const char* buffer, size_t buffer_size, size_t& offset, uint8_t masking_key[4]);
    
    // Validation helpers
    bool is_valid_opcode(uint8_t opcode) const;
    bool is_valid_control_frame(const WebSocketFrame& frame) const;
    bool is_valid_data_frame(const WebSocketFrame& frame) const;
};

// Utility functions
std::string websocket_close_code_to_string(WebSocketCloseCode code);
WebSocketCloseCode websocket_close_code_from_uint16(uint16_t code);

} // namespace websocket