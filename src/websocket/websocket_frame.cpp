#include "websocket_frame.h"
#include <arpa/inet.h>  // for htons, ntohs
#include <cstring>
#include <algorithm>
#include "logger.h"

namespace websocket {

// WebSocketFrame implementation

WebSocketFrame::WebSocketFrame(MQTTAllocator* allocator)
    : fin(false),
      rsv1(false),
      rsv2(false),
      rsv3(false),
      opcode(WebSocketOpcode::TEXT),
      masked(false),
      payload_length(0),
      payload(allocator ? mqtt::mqtt_stl_allocator<uint8_t>(allocator) : mqtt::mqtt_stl_allocator<uint8_t>()) {
    std::memset(masking_key, 0, sizeof(masking_key));
}

bool WebSocketFrame::is_control_frame() const {
    uint8_t op = static_cast<uint8_t>(opcode);
    return (op >= 0x8 && op <= 0xF);
}

bool WebSocketFrame::is_data_frame() const {
    uint8_t op = static_cast<uint8_t>(opcode);
    return (op >= 0x0 && op <= 0x7);
}

void WebSocketFrame::clear() {
    fin = false;
    rsv1 = rsv2 = rsv3 = false;
    opcode = WebSocketOpcode::TEXT;
    masked = false;
    payload_length = 0;
    std::memset(masking_key, 0, sizeof(masking_key));
    payload.clear();
}

int WebSocketFrame::get_close_info(uint16_t& code, std::string& reason) const {
  int __mq_ret = 0;
  do {
      if (opcode != WebSocketOpcode::CLOSE) {
          __mq_ret = MQ_ERR_INVALID_ARGS;
          break;
      }
  
      if (payload.empty()) {
          code = static_cast<uint16_t>(WebSocketCloseCode::NO_STATUS_RECEIVED);
          reason.clear();
          __mq_ret = MQ_SUCCESS;
          break;
      }
  
      if (payload.size() < 2) {
          LOG_ERROR("Invalid close frame: payload too short");
          __mq_ret = MQ_ERR_WS_INVALID_FRAME;
          break;
      }
  
      // Extract status code (network byte order)
      code = (static_cast<uint16_t>(payload[0]) << 8) | static_cast<uint16_t>(payload[1]);
  
      // Extract reason (if present)
      if (payload.size() > 2) {
          reason.assign(payload.begin() + 2, payload.end());
          // Validate UTF-8
          if (!WebSocketFrameParser::is_valid_utf8(
                  reinterpret_cast<const uint8_t*>(reason.data()), reason.size())) {
              LOG_ERROR("Invalid close frame: reason is not valid UTF-8");
              __mq_ret = MQ_ERR_WS_INVALID_UTF8;
              break;
          }
      } else {
          reason.clear();
      }
  
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

int WebSocketFrame::set_close_info(uint16_t code, const std::string& reason) {
  int __mq_ret = 0;
  do {
      if (opcode != WebSocketOpcode::CLOSE) {
          __mq_ret = MQ_ERR_INVALID_ARGS;
          break;
      }
  
      payload.clear();
  
      // Add status code (network byte order)
      payload.push_back(static_cast<uint8_t>(code >> 8));
      payload.push_back(static_cast<uint8_t>(code & 0xFF));
  
      // Add reason (if not empty)
      if (!reason.empty()) {
          // Validate UTF-8
          if (!WebSocketFrameParser::is_valid_utf8(
                  reinterpret_cast<const uint8_t*>(reason.data()), reason.size())) {
              LOG_ERROR("Invalid close reason: not valid UTF-8");
              __mq_ret = MQ_ERR_WS_INVALID_UTF8;
              break;
          }
          payload.insert(payload.end(), reason.begin(), reason.end());
      }
  
      payload_length = payload.size();
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

// WebSocketFrameParser implementation

WebSocketFrameParser::WebSocketFrameParser(MQTTAllocator* allocator, size_t max_payload_size)
    : allocator_(allocator), max_payload_size_(max_payload_size) {
}

WebSocketFrameParser::~WebSocketFrameParser() {
}

int WebSocketFrameParser::parse_frame(const uint8_t* data, size_t data_len,
                                      WebSocketFrame& frame, size_t& bytes_consumed) {
  int __mq_ret = 0;
  do {
      if (!data || data_len == 0) {
          __mq_ret = MQ_ERR_INVALID_ARGS;
          break;
      }
  
      // Parse header
      size_t header_len = 0;
      int ret = parse_header(data, data_len, frame, header_len);
      if (ret != MQ_SUCCESS) {
          __mq_ret = ret;
          break;
      }
  
      // Parse payload
      ret = parse_payload(data, data_len, frame, header_len, bytes_consumed);
      if (ret != MQ_SUCCESS) {
          __mq_ret = ret;
          break;
      }
  
      // Validate the complete frame
      ret = validate_frame(frame);
      if (ret != MQ_SUCCESS) {
          LOG_ERROR("Frame validation failed: {}", ret);
          __mq_ret = ret;
          break;
      }
  
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

int WebSocketFrameParser::parse_header(const uint8_t* data, size_t data_len,
                                       WebSocketFrame& frame, size_t& header_len) {
  int __mq_ret = 0;
  do {
      // Minimum header is 2 bytes
      if (data_len < 2) {
          __mq_ret = MQ_ERR_WS_INCOMPLETE_FRAME;
          break;
      }
  
      // Byte 0: FIN, RSV, Opcode
      uint8_t byte0 = data[0];
      frame.fin = (byte0 & 0x80) != 0;
      frame.rsv1 = (byte0 & 0x40) != 0;
      frame.rsv2 = (byte0 & 0x20) != 0;
      frame.rsv3 = (byte0 & 0x10) != 0;
      frame.opcode = static_cast<WebSocketOpcode>(byte0 & 0x0F);
  
      // Validate opcode
      if (!is_valid_opcode(static_cast<uint8_t>(frame.opcode))) {
          LOG_ERROR("Invalid opcode: {}", static_cast<int>(frame.opcode));
          __mq_ret = MQ_ERR_WS_INVALID_OPCODE;
          break;
      }
  
      // Byte 1: MASK, Payload length (first 7 bits)
      uint8_t byte1 = data[1];
      frame.masked = (byte1 & 0x80) != 0;
      uint8_t payload_len = byte1 & 0x7F;
  
      size_t offset = 2;
  
      // Determine actual payload length
      if (payload_len < 126) {
          frame.payload_length = payload_len;
      } else if (payload_len == 126) {
          // Next 2 bytes are payload length
          if (data_len < offset + 2) {
              __mq_ret = MQ_ERR_WS_INCOMPLETE_FRAME;
              break;
          }
          frame.payload_length = (static_cast<uint64_t>(data[offset]) << 8) |
                                static_cast<uint64_t>(data[offset + 1]);
          offset += 2;
      } else {  // payload_len == 127
          // Next 8 bytes are payload length
          if (data_len < offset + 8) {
              __mq_ret = MQ_ERR_WS_INCOMPLETE_FRAME;
              break;
          }
          frame.payload_length = 0;
          for (int i = 0; i < 8; ++i) {
              frame.payload_length = (frame.payload_length << 8) | static_cast<uint64_t>(data[offset + i]);
          }
          offset += 8;
      }
  
      // Check payload length limit
      if (frame.payload_length > max_payload_size_) {
          LOG_ERROR("Payload length {} exceeds maximum {}", frame.payload_length, max_payload_size_);
          __mq_ret = MQ_ERR_WS_FRAME_TOO_LARGE;
          break;
      }
  
      // Control frames must have payload <= 125 bytes
      if (frame.is_control_frame() && frame.payload_length > 125) {
          LOG_ERROR("Control frame payload too large: {}", frame.payload_length);
          __mq_ret = MQ_ERR_WS_PROTOCOL;
          break;
      }
  
      // Control frames must not be fragmented
      if (frame.is_control_frame() && !frame.fin) {
          LOG_ERROR("Control frame must not be fragmented");
          __mq_ret = MQ_ERR_WS_PROTOCOL;
          break;
      }
  
      // Read masking key if present
      if (frame.masked) {
          if (data_len < offset + 4) {
              __mq_ret = MQ_ERR_WS_INCOMPLETE_FRAME;
              break;
          }
          std::memcpy(frame.masking_key, data + offset, 4);
          offset += 4;
      } else {
          std::memset(frame.masking_key, 0, sizeof(frame.masking_key));
      }
  
      header_len = offset;
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

int WebSocketFrameParser::parse_payload(const uint8_t* data, size_t data_len,
                                        WebSocketFrame& frame, size_t header_len,
                                        size_t& bytes_consumed) {
  int __mq_ret = 0;
  do {
      // Check if we have enough data for payload
      if (data_len < header_len + frame.payload_length) {
          __mq_ret = MQ_ERR_WS_INCOMPLETE_FRAME;
          break;
      }
  
      // Read payload
      frame.payload.clear();
      if (frame.payload_length > 0) {
          frame.payload.resize(frame.payload_length);
          std::memcpy(frame.payload.data(), data + header_len, frame.payload_length);
  
          // Unmask if needed
          if (frame.masked) {
              apply_mask(frame.payload.data(), frame.payload.size(), frame.masking_key);
          }
      }
  
      // Validate UTF-8 for text frames
      if (frame.opcode == WebSocketOpcode::TEXT && frame.fin) {
          if (!is_valid_utf8(frame.payload.data(), frame.payload.size())) {
              LOG_ERROR("Text frame contains invalid UTF-8");
              __mq_ret = MQ_ERR_WS_INVALID_UTF8;
              break;
          }
      }
  
      bytes_consumed = header_len + frame.payload_length;
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

int WebSocketFrameParser::serialize_frame(const WebSocketFrame& frame, std::vector<uint8_t>& output) {
  int __mq_ret = 0;
  do {
      output.clear();
  
      // Validate frame first
      int ret = validate_frame(frame);
      if (ret != MQ_SUCCESS) {
          __mq_ret = ret;
          break;
      }
  
      // Byte 0: FIN, RSV, Opcode
      uint8_t byte0 = (frame.fin ? 0x80 : 0x00) |
                      (frame.rsv1 ? 0x40 : 0x00) |
                      (frame.rsv2 ? 0x20 : 0x00) |
                      (frame.rsv3 ? 0x10 : 0x00) |
                      (static_cast<uint8_t>(frame.opcode) & 0x0F);
      output.push_back(byte0);
  
      // Byte 1: MASK, Payload length
      uint8_t byte1 = frame.masked ? 0x80 : 0x00;
  
      if (frame.payload_length < 126) {
          byte1 |= static_cast<uint8_t>(frame.payload_length);
          output.push_back(byte1);
      } else if (frame.payload_length <= 0xFFFF) {
          byte1 |= 126;
          output.push_back(byte1);
          output.push_back(static_cast<uint8_t>(frame.payload_length >> 8));
          output.push_back(static_cast<uint8_t>(frame.payload_length & 0xFF));
      } else {
          byte1 |= 127;
          output.push_back(byte1);
          for (int i = 7; i >= 0; --i) {
              output.push_back(static_cast<uint8_t>((frame.payload_length >> (i * 8)) & 0xFF));
          }
      }
  
      // Masking key (if masked)
      if (frame.masked) {
          output.insert(output.end(), frame.masking_key, frame.masking_key + 4);
      }
  
      // Payload
      if (frame.payload_length > 0) {
          if (frame.masked) {
              // Apply masking
              std::vector<uint8_t> masked_payload(frame.payload.begin(), frame.payload.end());
              apply_mask(masked_payload.data(), masked_payload.size(), frame.masking_key);
              output.insert(output.end(), masked_payload.begin(), masked_payload.end());
          } else {
              output.insert(output.end(), frame.payload.begin(), frame.payload.end());
          }
      }
  
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

void WebSocketFrameParser::apply_mask(uint8_t* data, size_t data_len, const uint8_t* mask) {
    if (!data || !mask || data_len == 0) {
        return;
    }

    for (size_t i = 0; i < data_len; ++i) {
        data[i] ^= mask[i % 4];
    }
}

int WebSocketFrameParser::validate_frame(const WebSocketFrame& frame) {
  int __mq_ret = 0;
  do {
      // Check reserved bits (must be 0 unless extension is negotiated)
      if (frame.rsv1 || frame.rsv2 || frame.rsv3) {
          LOG_ERROR("Reserved bits must be 0");
          __mq_ret = MQ_ERR_WS_RESERVED_BITS;
          break;
      }
  
      // Validate opcode
      if (!is_valid_opcode(static_cast<uint8_t>(frame.opcode))) {
          LOG_ERROR("Invalid opcode: {}", static_cast<int>(frame.opcode));
          __mq_ret = MQ_ERR_WS_INVALID_OPCODE;
          break;
      }
  
      // Control frames validation
      if (frame.is_control_frame()) {
          if (!frame.fin) {
              LOG_ERROR("Control frame must have FIN=1");
              __mq_ret = MQ_ERR_WS_PROTOCOL;
              break;
          }
          if (frame.payload_length > 125) {
              LOG_ERROR("Control frame payload too large: {}", frame.payload_length);
              __mq_ret = MQ_ERR_WS_PROTOCOL;
              break;
          }
      }
  
      // Validate close frame
      if (frame.opcode == WebSocketOpcode::CLOSE && !frame.payload.empty()) {
          if (frame.payload.size() < 2) {
              LOG_ERROR("Close frame payload must be >= 2 bytes if present");
              __mq_ret = MQ_ERR_WS_CLOSE_CODE;
              break;
          }
          // Validate close reason UTF-8
          if (frame.payload.size() > 2) {
              if (!is_valid_utf8(frame.payload.data() + 2, frame.payload.size() - 2)) {
                  LOG_ERROR("Close frame reason must be valid UTF-8");
                  __mq_ret = MQ_ERR_WS_INVALID_UTF8;
                  break;
              }
          }
      }
  
      // Validate payload length matches actual payload
      if (frame.payload_length != frame.payload.size()) {
          LOG_ERROR("Payload length mismatch: expected {}, got {}",
                   frame.payload_length, frame.payload.size());
          __mq_ret = MQ_ERR_WS_PAYLOAD_LENGTH;
          break;
      }
  
      __mq_ret = MQ_SUCCESS;
      break;
  } while (false);

  return __mq_ret;
}

bool WebSocketFrameParser::is_valid_opcode(uint8_t opcode) {
    // Data frames: 0x0-0x2 (continuation, text, binary)
    // Control frames: 0x8-0xA (close, ping, pong)
    // Others are reserved
    return (opcode <= 0x2) || (opcode >= 0x8 && opcode <= 0xA);
}

bool WebSocketFrameParser::is_valid_utf8(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return true;
    }

    size_t i = 0;
    while (i < len) {
        uint8_t byte = data[i];

        // ASCII (0x00-0x7F)
        if (byte <= 0x7F) {
            i += 1;
        }
        // 2-byte sequence (0xC0-0xDF)
        else if ((byte & 0xE0) == 0xC0) {
            if (i + 1 >= len) return false;
            if ((data[i + 1] & 0xC0) != 0x80) return false;
            // Check for overlong encoding
            if (byte < 0xC2) return false;
            i += 2;
        }
        // 3-byte sequence (0xE0-0xEF)
        else if ((byte & 0xF0) == 0xE0) {
            if (i + 2 >= len) return false;
            if ((data[i + 1] & 0xC0) != 0x80) return false;
            if ((data[i + 2] & 0xC0) != 0x80) return false;
            // Check for overlong encoding and surrogate pairs
            if (byte == 0xE0 && data[i + 1] < 0xA0) return false;
            if (byte == 0xED && data[i + 1] >= 0xA0) return false;
            i += 3;
        }
        // 4-byte sequence (0xF0-0xF7)
        else if ((byte & 0xF8) == 0xF0) {
            if (i + 3 >= len) return false;
            if ((data[i + 1] & 0xC0) != 0x80) return false;
            if ((data[i + 2] & 0xC0) != 0x80) return false;
            if ((data[i + 3] & 0xC0) != 0x80) return false;
            // Check for overlong encoding and valid range
            if (byte == 0xF0 && data[i + 1] < 0x90) return false;
            if (byte >= 0xF5) return false;  // > U+10FFFF
            i += 4;
        }
        // Invalid
        else {
            return false;
        }
    }

    return true;
}

// Helper functions

const char* opcode_to_string(WebSocketOpcode opcode) {
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

const char* close_code_to_string(WebSocketCloseCode code) {
    switch (code) {
        case WebSocketCloseCode::NORMAL: return "NORMAL";
        case WebSocketCloseCode::GOING_AWAY: return "GOING_AWAY";
        case WebSocketCloseCode::PROTOCOL_ERROR: return "PROTOCOL_ERROR";
        case WebSocketCloseCode::UNSUPPORTED_DATA: return "UNSUPPORTED_DATA";
        case WebSocketCloseCode::NO_STATUS_RECEIVED: return "NO_STATUS_RECEIVED";
        case WebSocketCloseCode::ABNORMAL_CLOSURE: return "ABNORMAL_CLOSURE";
        case WebSocketCloseCode::INVALID_PAYLOAD: return "INVALID_PAYLOAD";
        case WebSocketCloseCode::POLICY_VIOLATION: return "POLICY_VIOLATION";
        case WebSocketCloseCode::MESSAGE_TOO_BIG: return "MESSAGE_TOO_BIG";
        case WebSocketCloseCode::MANDATORY_EXT: return "MANDATORY_EXT";
        case WebSocketCloseCode::INTERNAL_ERROR: return "INTERNAL_ERROR";
        case WebSocketCloseCode::SERVICE_RESTART: return "SERVICE_RESTART";
        case WebSocketCloseCode::TRY_AGAIN_LATER: return "TRY_AGAIN_LATER";
        case WebSocketCloseCode::TLS_HANDSHAKE: return "TLS_HANDSHAKE";
        default: return "UNKNOWN";
    }
}

}  // namespace websocket
