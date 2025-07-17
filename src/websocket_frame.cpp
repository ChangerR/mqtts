#include "websocket_frame.h"
#include <arpa/inet.h>
#include <cstring>
#include <sstream>

namespace websocket {

WebSocketFrameParser::WebSocketFrameParser() {}

WebSocketFrameParser::~WebSocketFrameParser() {}

int WebSocketFrameParser::parse_frame(const char* buffer, size_t buffer_size, 
                                    WebSocketFrame& frame, size_t& bytes_consumed) {
    if (buffer_size < 2) {
        return -1; // Need at least 2 bytes for header
    }
    
    size_t header_size = 0;
    int ret = parse_header(buffer, buffer_size, frame, header_size);
    if (ret != 0) {
        return ret;
    }
    
    if (buffer_size < header_size + frame.payload_length) {
        return -1; // Not enough data for complete frame
    }
    
    // Read payload
    if (frame.payload_length > 0) {
        frame.payload.resize(frame.payload_length);
        memcpy(frame.payload.data(), buffer + header_size, frame.payload_length);
        
        // Apply masking if needed
        if (frame.masked) {
            frame.apply_mask();
        }
    }
    
    bytes_consumed = header_size + frame.payload_length;
    return 0;
}

int WebSocketFrameParser::parse_header(const char* buffer, size_t buffer_size, 
                                     WebSocketFrame& frame, size_t& header_size) {
    if (buffer_size < 2) {
        return -1;
    }
    
    uint8_t first_byte = buffer[0];
    uint8_t second_byte = buffer[1];
    
    // Parse first byte
    frame.fin = (first_byte & 0x80) != 0;
    frame.rsv1 = (first_byte & 0x40) != 0;
    frame.rsv2 = (first_byte & 0x20) != 0;
    frame.rsv3 = (first_byte & 0x10) != 0;
    frame.opcode = static_cast<WebSocketOpcode>(first_byte & 0x0F);
    
    // Validate opcode
    if (!is_valid_opcode(static_cast<uint8_t>(frame.opcode))) {
        return -1;
    }
    
    // Parse second byte
    frame.masked = (second_byte & 0x80) != 0;
    uint8_t payload_len = second_byte & 0x7F;
    
    size_t offset = 2;
    
    // Parse extended payload length
    if (payload_len == 126) {
        if (buffer_size < offset + 2) {
            return -1;
        }
        frame.payload_length = ntohs(*reinterpret_cast<const uint16_t*>(buffer + offset));
        offset += 2;
    } else if (payload_len == 127) {
        if (buffer_size < offset + 8) {
            return -1;
        }
        frame.payload_length = be64toh(*reinterpret_cast<const uint64_t*>(buffer + offset));
        offset += 8;
    } else {
        frame.payload_length = payload_len;
    }
    
    // Parse masking key
    if (frame.masked) {
        if (buffer_size < offset + 4) {
            return -1;
        }
        memcpy(frame.masking_key, buffer + offset, 4);
        offset += 4;
    }
    
    header_size = offset;
    return 0;
}

int WebSocketFrameParser::serialize_frame(const WebSocketFrame& frame, std::vector<uint8_t>& buffer) {
    buffer.clear();
    
    // First byte: FIN, RSV1-3, opcode
    uint8_t first_byte = 0;
    if (frame.fin) first_byte |= 0x80;
    if (frame.rsv1) first_byte |= 0x40;
    if (frame.rsv2) first_byte |= 0x20;
    if (frame.rsv3) first_byte |= 0x10;
    first_byte |= static_cast<uint8_t>(frame.opcode) & 0x0F;
    buffer.push_back(first_byte);
    
    // Second byte: MASK, payload length
    uint8_t second_byte = 0;
    if (frame.masked) second_byte |= 0x80;
    
    if (frame.payload_length < 126) {
        second_byte |= static_cast<uint8_t>(frame.payload_length);
        buffer.push_back(second_byte);
    } else if (frame.payload_length < 65536) {
        second_byte |= 126;
        buffer.push_back(second_byte);
        uint16_t len = htons(static_cast<uint16_t>(frame.payload_length));
        buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&len), 
                     reinterpret_cast<const uint8_t*>(&len) + 2);
    } else {
        second_byte |= 127;
        buffer.push_back(second_byte);
        uint64_t len = htobe64(frame.payload_length);
        buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&len), 
                     reinterpret_cast<const uint8_t*>(&len) + 8);
    }
    
    // Masking key
    if (frame.masked) {
        buffer.insert(buffer.end(), frame.masking_key, frame.masking_key + 4);
    }
    
    // Payload
    if (!frame.payload.empty()) {
        if (frame.masked) {
            // Apply masking
            std::vector<uint8_t> masked_payload = frame.payload;
            for (size_t i = 0; i < masked_payload.size(); ++i) {
                masked_payload[i] ^= frame.masking_key[i % 4];
            }
            buffer.insert(buffer.end(), masked_payload.begin(), masked_payload.end());
        } else {
            buffer.insert(buffer.end(), frame.payload.begin(), frame.payload.end());
        }
    }
    
    return 0;
}

bool WebSocketFrameParser::is_valid_frame(const WebSocketFrame& frame) const {
    // Check opcode
    if (!is_valid_opcode(static_cast<uint8_t>(frame.opcode))) {
        return false;
    }
    
    // Check control frames
    if (frame.is_control_frame()) {
        return is_valid_control_frame(frame);
    }
    
    // Check data frames
    if (frame.is_data_frame()) {
        return is_valid_data_frame(frame);
    }
    
    return false;
}

bool WebSocketFrameParser::is_valid_opcode(uint8_t opcode) const {
    return opcode == 0x0 || opcode == 0x1 || opcode == 0x2 || 
           opcode == 0x8 || opcode == 0x9 || opcode == 0xA;
}

bool WebSocketFrameParser::is_valid_control_frame(const WebSocketFrame& frame) const {
    // Control frames must have FIN=1
    if (!frame.fin) {
        return false;
    }
    
    // Control frames must have payload length <= 125
    if (frame.payload_length > 125) {
        return false;
    }
    
    return true;
}

bool WebSocketFrameParser::is_valid_data_frame(const WebSocketFrame& frame) const {
    // Data frames can have any payload length
    return true;
}

int WebSocketFrameParser::get_frame_size(const char* buffer, size_t buffer_size, size_t& frame_size) {
    if (buffer_size < 2) {
        return -1;
    }
    
    uint8_t second_byte = buffer[1];
    bool masked = (second_byte & 0x80) != 0;
    uint8_t payload_len = second_byte & 0x7F;
    
    size_t header_size = 2;
    uint64_t payload_length = 0;
    
    if (payload_len == 126) {
        if (buffer_size < header_size + 2) {
            return -1;
        }
        payload_length = ntohs(*reinterpret_cast<const uint16_t*>(buffer + header_size));
        header_size += 2;
    } else if (payload_len == 127) {
        if (buffer_size < header_size + 8) {
            return -1;
        }
        payload_length = be64toh(*reinterpret_cast<const uint64_t*>(buffer + header_size));
        header_size += 8;
    } else {
        payload_length = payload_len;
    }
    
    if (masked) {
        header_size += 4;
    }
    
    frame_size = header_size + payload_length;
    return 0;
}

// apply_mask() method is already implemented in the header file

std::string websocket_close_code_to_string(WebSocketCloseCode code) {
    switch (code) {
        case WebSocketCloseCode::NORMAL_CLOSURE:
            return "Normal Closure";
        case WebSocketCloseCode::GOING_AWAY:
            return "Going Away";
        case WebSocketCloseCode::PROTOCOL_ERROR:
            return "Protocol Error";
        case WebSocketCloseCode::UNSUPPORTED_DATA:
            return "Unsupported Data";
        case WebSocketCloseCode::NO_STATUS_RCVD:
            return "No Status Received";
        case WebSocketCloseCode::ABNORMAL_CLOSURE:
            return "Abnormal Closure";
        case WebSocketCloseCode::INVALID_FRAME_PAYLOAD_DATA:
            return "Invalid Frame Payload Data";
        case WebSocketCloseCode::POLICY_VIOLATION:
            return "Policy Violation";
        case WebSocketCloseCode::MESSAGE_TOO_BIG:
            return "Message Too Big";
        case WebSocketCloseCode::MANDATORY_EXTENSION:
            return "Mandatory Extension";
        case WebSocketCloseCode::INTERNAL_SERVER_ERROR:
            return "Internal Server Error";
        case WebSocketCloseCode::SERVICE_RESTART:
            return "Service Restart";
        case WebSocketCloseCode::TRY_AGAIN_LATER:
            return "Try Again Later";
        case WebSocketCloseCode::BAD_GATEWAY:
            return "Bad Gateway";
        case WebSocketCloseCode::TLS_HANDSHAKE:
            return "TLS Handshake";
        default:
            return "Unknown";
    }
}

WebSocketCloseCode websocket_close_code_from_uint16(uint16_t code) {
    switch (code) {
        case 1000: return WebSocketCloseCode::NORMAL_CLOSURE;
        case 1001: return WebSocketCloseCode::GOING_AWAY;
        case 1002: return WebSocketCloseCode::PROTOCOL_ERROR;
        case 1003: return WebSocketCloseCode::UNSUPPORTED_DATA;
        case 1005: return WebSocketCloseCode::NO_STATUS_RCVD;
        case 1006: return WebSocketCloseCode::ABNORMAL_CLOSURE;
        case 1007: return WebSocketCloseCode::INVALID_FRAME_PAYLOAD_DATA;
        case 1008: return WebSocketCloseCode::POLICY_VIOLATION;
        case 1009: return WebSocketCloseCode::MESSAGE_TOO_BIG;
        case 1010: return WebSocketCloseCode::MANDATORY_EXTENSION;
        case 1011: return WebSocketCloseCode::INTERNAL_SERVER_ERROR;
        case 1012: return WebSocketCloseCode::SERVICE_RESTART;
        case 1013: return WebSocketCloseCode::TRY_AGAIN_LATER;
        case 1014: return WebSocketCloseCode::BAD_GATEWAY;
        case 1015: return WebSocketCloseCode::TLS_HANDSHAKE;
        default: return WebSocketCloseCode::NORMAL_CLOSURE;
    }
}

} // namespace websocket