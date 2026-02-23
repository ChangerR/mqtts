#include "websocket_protocol_handler.h"
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include "logger.h"
#include "websocket_mqtt_bridge.h"

namespace websocket {

// WebSocket GUID for handshake (RFC 6455)
static const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const size_t READ_BUFFER_SIZE = 64 * 1024;  // 64KB
static const int DEFAULT_PING_INTERVAL_MS = 30000;  // 30 seconds
static const int DEFAULT_PONG_TIMEOUT_MS = 10000;   // 10 seconds
static const size_t DEFAULT_MAX_MESSAGE_SIZE = 10 * 1024 * 1024;  // 10MB

static std::string trim_ascii(const std::string& input) {
    size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }

    size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return input.substr(begin, end - begin);
}

static std::string to_lower_ascii(const std::string& input) {
    std::string out = input;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

static bool header_value(const std::string& request, const std::string& header_name, std::string& value) {
    std::istringstream lines(request);
    std::string line;
    const std::string wanted = to_lower_ascii(header_name);
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }

        std::string name = to_lower_ascii(trim_ascii(line.substr(0, colon_pos)));
        if (name != wanted) {
            continue;
        }

        value = trim_ascii(line.substr(colon_pos + 1));
        return true;
    }
    return false;
}

static bool is_supported_mqtt_subprotocol_token(const std::string& token) {
    std::string normalized = trim_ascii(token);
    if (normalized.size() >= 2) {
        const char first = normalized.front();
        const char last = normalized.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            normalized = normalized.substr(1, normalized.size() - 2);
            normalized = trim_ascii(normalized);
        }
    }

    const std::string lower = to_lower_ascii(normalized);
    if (lower == "mqtt") {
        return true;
    }

    // Accept versioned MQTT subprotocol tokens such as mqttv3.1/mqttv3.1.1/mqttv5.
    return lower.rfind("mqttv", 0) == 0;
}

static bool select_supported_subprotocol(const std::string& offered, std::string& selected) {
    size_t start = 0;
    while (start < offered.size()) {
        size_t comma = offered.find(',', start);
        std::string token = trim_ascii(offered.substr(start, comma == std::string::npos
                                                                ? std::string::npos
                                                                : comma - start));
        if (is_supported_mqtt_subprotocol_token(token)) {
            selected = token;
            return true;
        }

        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

// Base64 encode helper
static std::string base64_encode(const unsigned char* input, size_t length) {
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    BIO_write(bio, input, length);
    BIO_flush(bio);

    BUF_MEM* buffer_ptr;
    BIO_get_mem_ptr(bio, &buffer_ptr);

    std::string result(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio);

    return result;
}

// WebSocketProtocolHandler implementation

WebSocketProtocolHandler::WebSocketProtocolHandler(MQTTAllocator* allocator)
    : allocator_(allocator),
      socket_(nullptr),
      state_(WebSocketState::CONNECTING),
      selected_subprotocol_(),
      client_port_(0),
      parser_(allocator),
      bridge_(nullptr),
      server_(nullptr),
      read_buffer_(mqtt::mqtt_stl_allocator<uint8_t>(allocator)),
      read_buffer_offset_(0),
      waiting_for_pong_(false),
      ping_interval_ms_(DEFAULT_PING_INTERVAL_MS),
      pong_timeout_ms_(DEFAULT_PONG_TIMEOUT_MS),
      max_message_size_(DEFAULT_MAX_MESSAGE_SIZE),
      fragmented_opcode_(WebSocketOpcode::TEXT),
      fragmented_payload_(mqtt::mqtt_stl_allocator<uint8_t>(allocator)),
      is_fragmenting_(false) {

    read_buffer_.reserve(READ_BUFFER_SIZE);
    last_ping_sent_ = std::chrono::steady_clock::now();
    last_pong_received_ = std::chrono::steady_clock::now();
}

WebSocketProtocolHandler::~WebSocketProtocolHandler() {
    close_connection();
}

int WebSocketProtocolHandler::init(MQTTSocket* socket, const std::string& client_ip, int client_port) {
    if (!socket) {
        LOG_ERROR("Socket is null");
        return MQ_ERR_INVALID_ARGS;
    }

    socket_ = socket;
    client_ip_ = client_ip;
    client_port_ = client_port;

    // Generate default client ID if not set
    if (client_id_.empty()) {
        std::ostringstream oss;
        oss << "ws_" << client_ip << "_" << client_port;
        client_id_ = oss.str();
    }

    LOG_INFO("WebSocket handler initialized for {}:{}, client_id={}",
             client_ip_, client_port_, client_id_);

    return MQ_SUCCESS;
}

int WebSocketProtocolHandler::process() {
    if (!socket_) {
        return MQ_ERR_INVALID_STATE;
    }

    // Handle handshake first
    if (state_ == WebSocketState::CONNECTING) {
        int ret = handle_handshake();
        if (ret != MQ_SUCCESS) {
            LOG_ERROR("Handshake failed: {}", ret);
            state_ = WebSocketState::CLOSED;
            return ret;
        }
        state_ = WebSocketState::OPEN;
        LOG_INFO("WebSocket connection established for client {}", client_id_);
    }

    // Main processing loop
    while (state_ == WebSocketState::OPEN) {
        // Check for ping timeout
        if (is_pong_timeout()) {
            LOG_WARN("Pong timeout for client {}", client_id_);
            send_close(static_cast<uint16_t>(WebSocketCloseCode::ABNORMAL_CLOSURE), "Ping timeout");
            state_ = WebSocketState::CLOSING;
            break;
        }

        // Send periodic ping
        if (should_send_ping()) {
            send_periodic_ping();
        }

        // Read and process frames
        WebSocketFrame frame(allocator_);
        int ret = read_frame(frame);

        if (ret == MQ_ERR_SOCKET_RECV || ret == MQ_ERR_SOCKET) {
            // Treat both recv errors and generic socket disconnect as peer close.
            LOG_INFO("Connection closed by peer for client {} (ret={})", client_id_, ret);
            state_ = WebSocketState::CLOSED;
            break;
        } else if (ret == MQ_ERR_WS_INCOMPLETE_FRAME) {
            // Need more data, continue
            continue;
        } else if (ret != MQ_SUCCESS) {
            LOG_ERROR("Failed to read frame: {}", ret);
            stats_.protocol_errors++;
            if (socket_ && socket_->is_connected()) {
                send_close(static_cast<uint16_t>(WebSocketCloseCode::PROTOCOL_ERROR), "Frame read error");
            }
            state_ = WebSocketState::CLOSING;
            break;
        }

        // Handle the frame
        ret = handle_frame(frame);
        if (ret != MQ_SUCCESS) {
            LOG_ERROR("Failed to handle frame: {}", ret);
            stats_.protocol_errors++;
            if (state_ != WebSocketState::CLOSING && state_ != WebSocketState::CLOSED) {
                send_close(static_cast<uint16_t>(WebSocketCloseCode::PROTOCOL_ERROR), "Frame handling error");
                state_ = WebSocketState::CLOSING;
            }
            break;
        }

        // If closing, break the loop
        if (state_ == WebSocketState::CLOSING || state_ == WebSocketState::CLOSED) {
            break;
        }
    }

    return MQ_SUCCESS;
}

int WebSocketProtocolHandler::handle_handshake() {
    // Read handshake request
    std::string request;
    int ret = read_handshake_request(request);
    if (ret != MQ_SUCCESS) {
        LOG_ERROR("Failed to read handshake request: {}", ret);
        return ret;
    }

    LOG_DEBUG("Received handshake request:\n{}", request);

    // Parse handshake request
    std::string ws_key;
    ret = parse_handshake_request(request, ws_key);
    if (ret != MQ_SUCCESS) {
        LOG_ERROR("Failed to parse handshake request: {}", ret);
        return ret;
    }

    if (bridge_) {
        if (is_supported_mqtt_subprotocol_token(selected_subprotocol_)) {
            bridge_->set_message_format(MessageFormat::MQTT_PACKET);
            LOG_INFO("WebSocket subprotocol '{}' selected, bridge switched to MQTT_PACKET mode",
                     selected_subprotocol_);
        } else {
            bridge_->set_message_format(MessageFormat::JSON);
            if (!selected_subprotocol_.empty()) {
                LOG_WARN("Unsupported WebSocket subprotocol '{}', fallback to JSON mode",
                         selected_subprotocol_);
            }
        }
    }

    // Send handshake response
    ret = send_handshake_response(ws_key);
    if (ret != MQ_SUCCESS) {
        LOG_ERROR("Failed to send handshake response: {}", ret);
        return ret;
    }

    LOG_INFO("WebSocket handshake completed for client {}", client_id_);
    return MQ_SUCCESS;
}

int WebSocketProtocolHandler::read_handshake_request(std::string& request) {
    request.clear();
    read_buffer_.clear();
    read_buffer_offset_ = 0;
    char buffer[4096];

    // Read until we find "\r\n\r\n" (end of HTTP headers)
    while (true) {
        int len = sizeof(buffer) - 1;
        int ret = socket_->recv(buffer, len);

        if (ret < 0) {
            LOG_ERROR("Failed to read handshake: {}", ret);
            return ret;
        }

        if (len == 0) {
            LOG_ERROR("Connection closed during handshake");
            return MQ_ERR_SOCKET_RECV;
        }

        buffer[len] = '\0';
        request.append(buffer, len);

        // Check for end of headers
        if (request.find("\r\n\r\n") != std::string::npos) {
            break;
        }

        // Prevent infinite loop
        if (request.size() > 8192) {
            LOG_ERROR("Handshake request too large");
            return MQ_ERR_WS_HANDSHAKE;
        }
    }

    // Keep any bytes that arrived after the HTTP headers.
    // These bytes are usually the first WebSocket frame and must not be dropped.
    size_t header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        LOG_ERROR("Handshake terminator not found");
        return MQ_ERR_WS_HANDSHAKE;
    }

    size_t payload_begin = header_end + 4;
    if (payload_begin < request.size()) {
        read_buffer_.insert(read_buffer_.end(),
                            request.begin() + payload_begin,
                            request.end());
        LOG_DEBUG("Buffered {} bytes of post-handshake data for client {}",
                  request.size() - payload_begin,
                  client_id_);
        request.erase(payload_begin);
    }

    return MQ_SUCCESS;
}

int WebSocketProtocolHandler::parse_handshake_request(const std::string& request, std::string& ws_key) {
    selected_subprotocol_.clear();

    if (!header_value(request, "Sec-WebSocket-Key", ws_key)) {
        LOG_ERROR("Sec-WebSocket-Key header not found");
        return MQ_ERR_WS_HANDSHAKE;
    }

    if (ws_key.empty()) {
        LOG_ERROR("Empty Sec-WebSocket-Key");
        return MQ_ERR_WS_HANDSHAKE;
    }

    std::string offered_subprotocols;
    if (header_value(request, "Sec-WebSocket-Protocol", offered_subprotocols)) {
        (void)select_supported_subprotocol(offered_subprotocols, selected_subprotocol_);
        LOG_DEBUG("Client offered subprotocols: '{}', selected: '{}'",
                  offered_subprotocols,
                  selected_subprotocol_.empty() ? std::string("<none>") : selected_subprotocol_);
    }

    LOG_DEBUG("Extracted WebSocket key: {}", ws_key);
    return MQ_SUCCESS;
}

std::string WebSocketProtocolHandler::compute_accept_key(const std::string& ws_key) {
    // Concatenate key with GUID
    std::string combined = ws_key + WS_GUID;

    // Compute SHA-1 hash
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()), combined.length(), hash);

    // Base64 encode
    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

int WebSocketProtocolHandler::send_handshake_response(const std::string& ws_key) {
    std::string accept_key = compute_accept_key(ws_key);

    // Build HTTP response
    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n"
             << "Upgrade: websocket\r\n"
             << "Connection: Upgrade\r\n"
             << "Sec-WebSocket-Accept: " << accept_key << "\r\n";

    if (!selected_subprotocol_.empty()) {
        response << "Sec-WebSocket-Protocol: " << selected_subprotocol_ << "\r\n";
    }

    response << "\r\n";

    std::string response_str = response.str();

    int ret = socket_->send(reinterpret_cast<const uint8_t*>(response_str.c_str()),
                           response_str.length());
    if (ret < 0) {
        LOG_ERROR("Failed to send handshake response: {}", ret);
        return ret;
    }

    LOG_DEBUG("Sent handshake response:\n{}", response_str);
    return MQ_SUCCESS;
}

int WebSocketProtocolHandler::read_frame(WebSocketFrame& frame) {
    while (true) {
        size_t available = read_buffer_.size() - read_buffer_offset_;

        // Parse buffered data first to avoid blocking recv when a full frame is already in memory.
        if (available > 0) {
            size_t bytes_consumed = 0;
            int ret = parser_.parse_frame(read_buffer_.data() + read_buffer_offset_,
                                          available,
                                          frame, bytes_consumed);

            if (ret == MQ_SUCCESS) {
                read_buffer_offset_ += bytes_consumed;
                stats_.frames_received++;

                // Compact buffer if needed
                if (read_buffer_offset_ > READ_BUFFER_SIZE / 2) {
                    read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + read_buffer_offset_);
                    read_buffer_offset_ = 0;
                }

                return MQ_SUCCESS;
            }

            if (ret != MQ_ERR_WS_INCOMPLETE_FRAME) {
                return ret;
            }
        }

        // Need more bytes to finish a frame.
        char temp_buffer[4096];
        int len = sizeof(temp_buffer);
        int ret = socket_->recv(temp_buffer, len);

        if (ret < 0) {
            return ret;
        }

        if (len == 0) {
            return MQ_ERR_SOCKET_RECV;  // Connection closed
        }

        read_buffer_.insert(read_buffer_.end(), temp_buffer, temp_buffer + len);
        stats_.bytes_received += len;
    }
}

int WebSocketProtocolHandler::handle_frame(const WebSocketFrame& frame) {
    LOG_TRACE("Handling frame: opcode={}, fin={}, length={}",
              static_cast<int>(frame.opcode), frame.fin, frame.payload_length);

    // Control frames
    if (frame.is_control_frame()) {
        switch (frame.opcode) {
            case WebSocketOpcode::CLOSE:
                return handle_close_frame(frame);
            case WebSocketOpcode::PING:
                return handle_ping_frame(frame);
            case WebSocketOpcode::PONG:
                return handle_pong_frame(frame);
            default:
                LOG_ERROR("Unknown control frame opcode: {}", static_cast<int>(frame.opcode));
                return MQ_ERR_WS_INVALID_OPCODE;
        }
    }

    // Data frames
    switch (frame.opcode) {
        case WebSocketOpcode::TEXT:
            return handle_text_frame(frame);
        case WebSocketOpcode::BINARY:
            return handle_binary_frame(frame);
        case WebSocketOpcode::CONTINUATION:
            // Handle continuation frame for fragmented messages
            if (!is_fragmenting_) {
                LOG_ERROR("Received continuation frame without initial frame");
                return MQ_ERR_WS_FRAGMENTATION;
            }
            // Append to fragmented payload
            fragmented_payload_.insert(fragmented_payload_.end(),
                                      frame.payload.begin(), frame.payload.end());
            if (frame.fin) {
                // Complete message
                WebSocketFrame complete_frame(allocator_);
                complete_frame.fin = true;
                complete_frame.opcode = fragmented_opcode_;
                complete_frame.payload = fragmented_payload_;
                complete_frame.payload_length = fragmented_payload_.size();

                is_fragmenting_ = false;
                fragmented_payload_.clear();

                // Handle complete frame
                if (fragmented_opcode_ == WebSocketOpcode::TEXT) {
                    return handle_text_frame(complete_frame);
                } else {
                    return handle_binary_frame(complete_frame);
                }
            }
            return MQ_SUCCESS;
        default:
            LOG_ERROR("Unknown data frame opcode: {}", static_cast<int>(frame.opcode));
            return MQ_ERR_WS_INVALID_OPCODE;
    }
}

int WebSocketProtocolHandler::handle_text_frame(const WebSocketFrame& frame) {
    stats_.text_frames++;

    // Check for fragmentation
    if (!frame.fin) {
        if (is_fragmenting_) {
            LOG_ERROR("Already fragmenting a message");
            return MQ_ERR_WS_FRAGMENTATION;
        }
        is_fragmenting_ = true;
        fragmented_opcode_ = frame.opcode;
        fragmented_payload_.assign(frame.payload.begin(), frame.payload.end());
        return MQ_SUCCESS;
    }

    // Convert to string
    std::string text(frame.payload.begin(), frame.payload.end());
    LOG_DEBUG("Received text message from {}: {}", client_id_, text);

    // Forward to bridge if available
    if (bridge_) {
        return bridge_->handle_websocket_text(client_id_, text);
    }

    return MQ_SUCCESS;
}

int WebSocketProtocolHandler::handle_binary_frame(const WebSocketFrame& frame) {
    stats_.binary_frames++;

    // Check for fragmentation
    if (!frame.fin) {
        if (is_fragmenting_) {
            LOG_ERROR("Already fragmenting a message");
            return MQ_ERR_WS_FRAGMENTATION;
        }
        is_fragmenting_ = true;
        fragmented_opcode_ = frame.opcode;
        fragmented_payload_.assign(frame.payload.begin(), frame.payload.end());
        return MQ_SUCCESS;
    }

    LOG_DEBUG("Received binary message from {}: {} bytes", client_id_, frame.payload.size());

    // Forward to bridge if available
    if (bridge_) {
        std::vector<uint8_t> data(frame.payload.begin(), frame.payload.end());
        return bridge_->handle_websocket_binary(client_id_, data);
    }

    return MQ_SUCCESS;
}

int WebSocketProtocolHandler::handle_close_frame(const WebSocketFrame& frame) {
    uint16_t code = 1000;
    std::string reason;

    if (!frame.payload.empty()) {
        frame.get_close_info(code, reason);
    }

    LOG_INFO("Received close frame from {}: code={}, reason='{}'",
             client_id_, code, reason);

    // Send close response if not already closing
    if (state_ != WebSocketState::CLOSING) {
        send_close(code, reason);
    }

    state_ = WebSocketState::CLOSED;
    return MQ_SUCCESS;
}

int WebSocketProtocolHandler::handle_ping_frame(const WebSocketFrame& frame) {
    stats_.ping_frames++;
    LOG_TRACE("Received ping from {}", client_id_);

    // Send pong with same payload
    std::vector<uint8_t> payload(frame.payload.begin(), frame.payload.end());
    return send_pong(payload);
}

int WebSocketProtocolHandler::handle_pong_frame(const WebSocketFrame& frame) {
    stats_.pong_frames++;
    LOG_TRACE("Received pong from {}", client_id_);

    last_pong_received_ = std::chrono::steady_clock::now();
    waiting_for_pong_ = false;

    return MQ_SUCCESS;
}

int WebSocketProtocolHandler::send_frame(const WebSocketFrame& frame) {
    // Serialize frame
    std::vector<uint8_t> data;
    int ret = parser_.serialize_frame(frame, data);
    if (ret != MQ_SUCCESS) {
        LOG_ERROR("Failed to serialize frame: {}", ret);
        return ret;
    }

    // Send data
    ret = write_frame_data(data);
    if (ret != MQ_SUCCESS) {
        return ret;
    }

    stats_.frames_sent++;
    stats_.bytes_sent += data.size();

    return MQ_SUCCESS;
}

int WebSocketProtocolHandler::send_text(const std::string& text) {
    WebSocketFrame frame(allocator_);
    frame.fin = true;
    frame.opcode = WebSocketOpcode::TEXT;
    frame.payload.assign(text.begin(), text.end());
    frame.payload_length = frame.payload.size();

    LOG_TRACE("Sending text message to {}: {} bytes", client_id_, text.size());
    return send_frame(frame);
}

int WebSocketProtocolHandler::send_binary(const std::vector<uint8_t>& data) {
    WebSocketFrame frame(allocator_);
    frame.fin = true;
    frame.opcode = WebSocketOpcode::BINARY;
    frame.payload.assign(data.begin(), data.end());
    frame.payload_length = frame.payload.size();

    LOG_TRACE("Sending binary message to {}: {} bytes", client_id_, data.size());
    return send_frame(frame);
}

int WebSocketProtocolHandler::send_close(uint16_t code, const std::string& reason) {
    WebSocketFrame frame(allocator_);
    frame.fin = true;
    frame.opcode = WebSocketOpcode::CLOSE;
    frame.set_close_info(code, reason);

    LOG_INFO("Sending close frame to {}: code={}, reason='{}'", client_id_, code, reason);

    int ret = send_frame(frame);
    if (ret == MQ_SUCCESS) {
        state_ = WebSocketState::CLOSING;
    }

    return ret;
}

int WebSocketProtocolHandler::send_ping(const std::vector<uint8_t>& payload) {
    WebSocketFrame frame(allocator_);
    frame.fin = true;
    frame.opcode = WebSocketOpcode::PING;
    frame.payload.assign(payload.begin(), payload.end());
    frame.payload_length = frame.payload.size();

    LOG_TRACE("Sending ping to {}", client_id_);
    return send_frame(frame);
}

int WebSocketProtocolHandler::send_pong(const std::vector<uint8_t>& payload) {
    WebSocketFrame frame(allocator_);
    frame.fin = true;
    frame.opcode = WebSocketOpcode::PONG;
    frame.payload.assign(payload.begin(), payload.end());
    frame.payload_length = frame.payload.size();

    LOG_TRACE("Sending pong to {}", client_id_);
    return send_frame(frame);
}

int WebSocketProtocolHandler::write_frame_data(const std::vector<uint8_t>& data) {
    int ret = socket_->send(data.data(), data.size());
    if (ret < 0) {
        LOG_ERROR("Failed to send frame data: {}", ret);
        return ret;
    }
    return MQ_SUCCESS;
}

int WebSocketProtocolHandler::send_periodic_ping() {
    if (waiting_for_pong_) {
        LOG_WARN("Still waiting for pong from {}", client_id_);
        return MQ_SUCCESS;  // Don't send another ping
    }

    int ret = send_ping();
    if (ret == MQ_SUCCESS) {
        last_ping_sent_ = std::chrono::steady_clock::now();
        waiting_for_pong_ = true;
    }

    return ret;
}

bool WebSocketProtocolHandler::should_send_ping() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ping_sent_);
    return elapsed.count() >= ping_interval_ms_;
}

bool WebSocketProtocolHandler::is_pong_timeout() const {
    if (!waiting_for_pong_) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ping_sent_);
    return elapsed.count() >= pong_timeout_ms_;
}

void WebSocketProtocolHandler::close_connection() {
    if (state_ != WebSocketState::CLOSED) {
        if (state_ == WebSocketState::OPEN) {
            send_close(static_cast<uint16_t>(WebSocketCloseCode::GOING_AWAY), "Server closing");
        }
        state_ = WebSocketState::CLOSED;
    }

    if (socket_) {
        socket_->close();
        socket_ = nullptr;
    }

    LOG_INFO("WebSocket connection closed for client {}", client_id_);
}

}  // namespace websocket
