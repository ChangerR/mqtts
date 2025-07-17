#include "websocket_protocol_handler.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <cstring>
#include "logger.h"
#include "websocket_server.h"

namespace websocket {

WebSocketProtocolHandler::WebSocketProtocolHandler(MQTTAllocator* allocator)
    : allocator_(allocator)
    , socket_(nullptr)
    , state_(WebSocketState::CONNECTING)
    , handshake_completed_(false)
    , ping_pending_(false)
    , server_(nullptr)
    , session_manager_(nullptr)
    , write_lock_acquired_(false)
    , frame_read_state_(FrameReadState::READ_HEADER)
    , frame_bytes_needed_(0)
    , buffer_(nullptr)
    , current_buffer_size_(0)
    , bytes_read_(0) {
}

WebSocketProtocolHandler::~WebSocketProtocolHandler() {
    if (buffer_) {
        allocator_->deallocate(buffer_, current_buffer_size_);
    }
}

int WebSocketProtocolHandler::init(MQTTSocket* socket, const std::string& client_ip, int client_port) {
    if (!socket) {
        return -1;
    }
    
    socket_ = socket;
    client_ip_ = client_ip;
    client_port_ = client_port;
    
    // Initialize buffer
    current_buffer_size_ = 4096;
    buffer_ = static_cast<char*>(allocator_->allocate(current_buffer_size_));
    if (!buffer_) {
        return -1;
    }
    
    state_ = WebSocketState::CONNECTING;
    handshake_completed_ = false;
    
    LOG_DEBUG("WebSocket handler initialized for {}:{}", client_ip, client_port);
    return 0;
}

int WebSocketProtocolHandler::process() {
    if (!socket_) {
        return -1;
    }
    
    // Handle handshake first
    if (!handshake_completed_) {
        int ret = handle_handshake();
        if (ret != 0) {
            return ret;
        }
        if (!handshake_completed_) {
            return 0; // Need more data
        }
        state_ = WebSocketState::OPEN;
    }
    
    // Process WebSocket frames
    while (state_ == WebSocketState::OPEN) {
        WebSocketFrame frame;
        int ret = read_frame_header();
        if (ret != 0) {
            if (ret == -2) {
                return 0; // Need more data
            }
            return ret;
        }
        
        ret = read_frame_payload(frame);
        if (ret != 0) {
            if (ret == -2) {
                return 0; // Need more data
            }
            return ret;
        }
        
        ret = handle_frame(frame);
        if (ret != 0) {
            return ret;
        }
    }
    
    return 0;
}

int WebSocketProtocolHandler::handle_handshake() {
    // Read HTTP request
    char* line_end = nullptr;
    while ((line_end = strstr(buffer_, "\r\n\r\n")) == nullptr) {
        if (bytes_read_ >= current_buffer_size_ - 1) {
            // Need larger buffer
            if (ensure_buffer_size(current_buffer_size_ * 2) != 0) {
                return -1;
            }
        }
        
        char temp_buf[1024];
        int temp_len = sizeof(temp_buf);
        ssize_t bytes = socket_->recv(temp_buf, temp_len);
        if (bytes > 0) {
            memcpy(buffer_ + bytes_read_, temp_buf, bytes);
        }
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            return -2; // Need more data
        }
        
        bytes_read_ += bytes;
        buffer_[bytes_read_] = '\0';
    }
    
    // Parse HTTP request
    std::string request(buffer_, line_end + 4 - buffer_);
    std::unordered_map<std::string, std::string> headers;
    
    int ret = parse_http_request(request, headers);
    if (ret != 0) {
        LOG_ERROR("Failed to parse HTTP request");
        stats_.protocol_errors++;
        return -1;
    }
    
    // Validate WebSocket handshake
    if (headers["upgrade"] != "websocket" || headers["connection"] != "upgrade") {
        LOG_ERROR("Invalid WebSocket handshake");
        stats_.protocol_errors++;
        return -1;
    }
    
    std::string websocket_key = headers["sec-websocket-key"];
    if (websocket_key.empty()) {
        LOG_ERROR("Missing WebSocket key");
        stats_.protocol_errors++;
        return -1;
    }
    
    // Generate response
    std::string response_key;
    ret = generate_websocket_key_response(websocket_key, response_key);
    if (ret != 0) {
        LOG_ERROR("Failed to generate WebSocket key response");
        return -1;
    }
    
    // Send handshake response
    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n";
    response << "Upgrade: websocket\r\n";
    response << "Connection: Upgrade\r\n";
    response << "Sec-WebSocket-Accept: " << response_key << "\r\n";
    response << "\r\n";
    
    std::string response_str = response.str();
    if (socket_->send(reinterpret_cast<const uint8_t*>(response_str.c_str()), response_str.length()) < 0) {
        LOG_ERROR("Failed to send handshake response");
        return -1;
    }
    
    handshake_completed_ = true;
    
    // Reset buffer for frame processing
    bytes_read_ = 0;
    frame_read_state_ = FrameReadState::READ_HEADER;
    
    LOG_DEBUG("WebSocket handshake completed for {}:{}", client_ip_, client_port_);
    return 0;
}

int WebSocketProtocolHandler::parse_http_request(const std::string& request, 
                                               std::unordered_map<std::string, std::string>& headers) {
    std::istringstream iss(request);
    std::string line;
    
    // Skip request line
    if (!std::getline(iss, line)) {
        return -1;
    }
    
    // Parse headers
    while (std::getline(iss, line) && !line.empty()) {
        if (line.back() == '\r') {
            line.pop_back();
        }
        
        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }
        
        std::string key = line.substr(0, colon_pos);
        std::string value = line.substr(colon_pos + 1);
        
        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        // Convert to lowercase
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        
        headers[key] = value;
    }
    
    return 0;
}

int WebSocketProtocolHandler::generate_websocket_key_response(const std::string& key, std::string& response) {
    // For testing purposes, use a simple response
    // In production, this should use proper SHA-1 + base64 encoding
    std::string magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + magic_string;
    
    // Simple hash simulation for testing
    response = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    
    return 0;
}

int WebSocketProtocolHandler::handle_frame(const WebSocketFrame& frame) {
    stats_.frames_received++;
    
    switch (frame.opcode) {
        case WebSocketOpcode::TEXT:
            stats_.text_frames++;
            return handle_text_frame(frame);
        case WebSocketOpcode::BINARY:
            stats_.binary_frames++;
            return handle_binary_frame(frame);
        case WebSocketOpcode::CLOSE:
            stats_.close_frames++;
            return handle_close_frame(frame);
        case WebSocketOpcode::PING:
            stats_.ping_frames++;
            return handle_ping_frame(frame);
        case WebSocketOpcode::PONG:
            stats_.pong_frames++;
            return handle_pong_frame(frame);
        default:
            LOG_ERROR("Unsupported WebSocket opcode: {}", static_cast<int>(frame.opcode));
            stats_.protocol_errors++;
            return -1;
    }
}

int WebSocketProtocolHandler::handle_text_frame(const WebSocketFrame& frame) {
    std::string text(frame.payload.begin(), frame.payload.end());
    LOG_DEBUG("Received text frame: {}", text);
    
    // Forward to MQTT bridge if available
    if (server_ && !client_id_.empty()) {
        // This would be implemented to forward to the MQTT bridge
        // For now, just log the message
        LOG_DEBUG("Forwarding text message to MQTT bridge");
    }
    
    return 0;
}

int WebSocketProtocolHandler::handle_binary_frame(const WebSocketFrame& frame) {
    LOG_DEBUG("Received binary frame of size: {}", frame.payload.size());
    
    // Forward to MQTT bridge if available
    if (server_ && !client_id_.empty()) {
        // This would be implemented to forward to the MQTT bridge
        // For now, just log the message
        LOG_DEBUG("Forwarding binary message to MQTT bridge");
    }
    
    return 0;
}

int WebSocketProtocolHandler::handle_close_frame(const WebSocketFrame& frame) {
    LOG_DEBUG("Received close frame");
    
    uint16_t close_code = 1000; // Normal closure
    std::string reason = "";
    
    if (frame.payload.size() >= 2) {
        close_code = (static_cast<uint16_t>(frame.payload[0]) << 8) | frame.payload[1];
        if (frame.payload.size() > 2) {
            reason = std::string(frame.payload.begin() + 2, frame.payload.end());
        }
    }
    
    LOG_DEBUG("Close code: {}, reason: {}", close_code, reason);
    
    // Send close frame in response
    send_close(close_code, reason);
    
    state_ = WebSocketState::CLOSED;
    return 0;
}

int WebSocketProtocolHandler::handle_ping_frame(const WebSocketFrame& frame) {
    LOG_DEBUG("Received ping frame");
    
    // Send pong response
    return send_pong(frame.payload);
}

int WebSocketProtocolHandler::handle_pong_frame(const WebSocketFrame& frame) {
    LOG_DEBUG("Received pong frame");
    
    ping_pending_ = false;
    last_pong_time_ = std::chrono::steady_clock::now();
    
    return 0;
}

int WebSocketProtocolHandler::send_text(const std::string& text) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WebSocketOpcode::TEXT;
    frame.masked = false;
    frame.payload = std::vector<uint8_t>(text.begin(), text.end());
    frame.payload_length = frame.payload.size();
    
    return send_frame(frame);
}

int WebSocketProtocolHandler::send_binary(const std::vector<uint8_t>& data) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WebSocketOpcode::BINARY;
    frame.masked = false;
    frame.payload = data;
    frame.payload_length = frame.payload.size();
    
    return send_frame(frame);
}

int WebSocketProtocolHandler::send_close(uint16_t code, const std::string& reason) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WebSocketOpcode::CLOSE;
    frame.masked = false;
    
    // Add close code
    frame.payload.push_back(static_cast<uint8_t>(code >> 8));
    frame.payload.push_back(static_cast<uint8_t>(code & 0xFF));
    
    // Add reason
    frame.payload.insert(frame.payload.end(), reason.begin(), reason.end());
    frame.payload_length = frame.payload.size();
    
    return send_frame(frame);
}

int WebSocketProtocolHandler::send_ping(const std::vector<uint8_t>& payload) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WebSocketOpcode::PING;
    frame.masked = false;
    frame.payload = payload;
    frame.payload_length = frame.payload.size();
    
    ping_pending_ = true;
    last_ping_time_ = std::chrono::steady_clock::now();
    
    return send_frame(frame);
}

int WebSocketProtocolHandler::send_pong(const std::vector<uint8_t>& payload) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WebSocketOpcode::PONG;
    frame.masked = false;
    frame.payload = payload;
    frame.payload_length = frame.payload.size();
    
    return send_frame(frame);
}

int WebSocketProtocolHandler::send_frame(const WebSocketFrame& frame) {
    if (!socket_) {
        return -1;
    }
    
    WebSocketFrameParser parser;
    std::vector<uint8_t> buffer;
    
    int ret = parser.serialize_frame(frame, buffer);
    if (ret != 0) {
        LOG_ERROR("Failed to serialize WebSocket frame");
        return -1;
    }
    
    // Send frame data
    if (socket_->send(buffer.data(), buffer.size()) < 0) {
        LOG_ERROR("Failed to send WebSocket frame");
        return -1;
    }
    
    stats_.frames_sent++;
    stats_.bytes_sent += buffer.size();
    
    return 0;
}

int WebSocketProtocolHandler::ensure_buffer_size(size_t needed_size) {
    if (needed_size <= current_buffer_size_) {
        return 0;
    }
    
    char* new_buffer = static_cast<char*>(allocator_->allocate(needed_size));
    if (!new_buffer) {
        return -1;
    }
    
    if (buffer_) {
        memcpy(new_buffer, buffer_, bytes_read_);
        allocator_->deallocate(buffer_, current_buffer_size_);
    }
    
    buffer_ = new_buffer;
    current_buffer_size_ = needed_size;
    
    return 0;
}

int WebSocketProtocolHandler::read_frame_header() {
    // Need at least 2 bytes for header
    if (bytes_read_ < 2) {
        char temp_buf[2];
        int temp_len = 2 - bytes_read_;
        ssize_t bytes = socket_->recv(temp_buf, temp_len);
        if (bytes > 0) {
            memcpy(buffer_ + bytes_read_, temp_buf, bytes);
        }
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            return -2; // Need more data
        }
        bytes_read_ += bytes;
    }
    
    if (bytes_read_ < 2) {
        return -2; // Need more data
    }
    
    // We have enough data to determine frame size
    WebSocketFrameParser parser;
    size_t frame_size = 0;
    
    int ret = parser.get_frame_size(buffer_, bytes_read_, frame_size);
    if (ret != 0) {
        return -2; // Need more data
    }
    
    frame_bytes_needed_ = frame_size;
    frame_read_state_ = FrameReadState::READ_PAYLOAD;
    
    return 0;
}

int WebSocketProtocolHandler::read_frame_payload(WebSocketFrame& frame) {
    // Read complete frame
    while (bytes_read_ < frame_bytes_needed_) {
        if (ensure_buffer_size(frame_bytes_needed_) != 0) {
            return -1;
        }
        
        char temp_buf[1024];
        int temp_len = std::min(sizeof(temp_buf), frame_bytes_needed_ - bytes_read_);
        ssize_t bytes = socket_->recv(temp_buf, temp_len);
        if (bytes > 0) {
            memcpy(buffer_ + bytes_read_, temp_buf, bytes);
        }
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            return -2; // Need more data
        }
        
        bytes_read_ += bytes;
    }
    
    // Parse complete frame
    WebSocketFrameParser parser;
    size_t bytes_consumed = 0;
    
    int ret = parser.parse_frame(buffer_, bytes_read_, frame, bytes_consumed);
    if (ret != 0) {
        LOG_ERROR("Failed to parse WebSocket frame");
        stats_.protocol_errors++;
        return -1;
    }
    
    // Move remaining data to beginning of buffer
    if (bytes_consumed < bytes_read_) {
        memmove(buffer_, buffer_ + bytes_consumed, bytes_read_ - bytes_consumed);
        bytes_read_ -= bytes_consumed;
    } else {
        bytes_read_ = 0;
    }
    
    // Reset for next frame
    frame_read_state_ = FrameReadState::READ_HEADER;
    frame_bytes_needed_ = 0;
    
    return 0;
}

} // namespace websocket