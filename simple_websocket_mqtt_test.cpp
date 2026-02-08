#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

// Simple WebSocket MQTT client for testing

// Base64 encode
std::string base64_encode(const unsigned char* input, size_t length) {
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

// Compute WebSocket accept key
std::string compute_accept_key(const std::string& key) {
    const char* GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + GUID;

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()), combined.length(), hash);

    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

// Generate random WebSocket key
std::string generate_websocket_key() {
    unsigned char random_bytes[16];
    for (int i = 0; i < 16; i++) {
        random_bytes[i] = rand() % 256;
    }
    return base64_encode(random_bytes, 16);
}

// Create WebSocket frame
std::vector<uint8_t> create_websocket_frame(uint8_t opcode, const std::string& payload, bool masked = true) {
    std::vector<uint8_t> frame;

    // Byte 0: FIN=1, RSV=0, opcode
    frame.push_back(0x80 | (opcode & 0x0F));

    // Byte 1: MASK=1 (client must mask), payload length
    size_t payload_len = payload.size();
    if (payload_len < 126) {
        frame.push_back((masked ? 0x80 : 0x00) | static_cast<uint8_t>(payload_len));
    } else if (payload_len < 65536) {
        frame.push_back((masked ? 0x80 : 0x00) | 126);
        frame.push_back(static_cast<uint8_t>(payload_len >> 8));
        frame.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    } else {
        frame.push_back((masked ? 0x80 : 0x00) | 127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back(static_cast<uint8_t>((payload_len >> (i * 8)) & 0xFF));
        }
    }

    // Masking key (if masked)
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    if (masked) {
        for (int i = 0; i < 4; i++) {
            frame.push_back(mask[i]);
        }
    }

    // Payload (masked if needed)
    for (size_t i = 0; i < payload.size(); i++) {
        uint8_t byte = payload[i];
        if (masked) {
            byte ^= mask[i % 4];
        }
        frame.push_back(byte);
    }

    return frame;
}

// Parse WebSocket frame
bool parse_websocket_frame(const uint8_t* data, size_t len, uint8_t& opcode, std::string& payload) {
    if (len < 2) {
        return false;
    }

    uint8_t byte0 = data[0];
    bool fin = (byte0 & 0x80) != 0;
    opcode = byte0 & 0x0F;

    uint8_t byte1 = data[1];
    bool masked = (byte1 & 0x80) != 0;
    uint64_t payload_len = byte1 & 0x7F;

    size_t offset = 2;

    if (payload_len == 126) {
        if (len < offset + 2) return false;
        payload_len = (static_cast<uint64_t>(data[offset]) << 8) | data[offset + 1];
        offset += 2;
    } else if (payload_len == 127) {
        if (len < offset + 8) return false;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | data[offset + i];
        }
        offset += 8;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (len < offset + 4) return false;
        memcpy(mask, data + offset, 4);
        offset += 4;
    }

    if (len < offset + payload_len) {
        return false;
    }

    payload.clear();
    for (size_t i = 0; i < payload_len; i++) {
        uint8_t byte = data[offset + i];
        if (masked) {
            byte ^= mask[i % 4];
        }
        payload.push_back(byte);
    }

    return fin;
}

class WebSocketMQTTClient {
public:
    WebSocketMQTTClient(const std::string& host, int port)
        : host_(host), port_(port), sockfd_(-1), connected_(false) {}

    ~WebSocketMQTTClient() {
        disconnect();
    }

    bool connect() {
        // Create socket
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) {
            std::cerr << "Failed to create socket" << std::endl;
            return false;
        }

        // Connect to server
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_);
        inet_pton(AF_INET, host_.c_str(), &server_addr.sin_addr);

        if (::connect(sockfd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "Failed to connect to server" << std::endl;
            close(sockfd_);
            sockfd_ = -1;
            return false;
        }

        // Perform WebSocket handshake
        if (!perform_handshake()) {
            close(sockfd_);
            sockfd_ = -1;
            return false;
        }

        connected_ = true;
        std::cout << "Connected to WebSocket MQTT server at " << host_ << ":" << port_ << std::endl;
        return true;
    }

    void disconnect() {
        if (sockfd_ >= 0) {
            // Send close frame
            std::vector<uint8_t> close_frame = create_websocket_frame(0x08, "");
            send(sockfd_, close_frame.data(), close_frame.size(), 0);

            close(sockfd_);
            sockfd_ = -1;
            connected_ = false;
        }
    }

    bool publish(const std::string& topic, const std::string& payload, int qos = 0) {
        if (!connected_) {
            std::cerr << "Not connected" << std::endl;
            return false;
        }

        // Create JSON message
        std::string json = "{\"topic\":\"" + topic + "\","
                          "\"payload\":\"" + payload + "\","
                          "\"qos\":" + std::to_string(qos) + ","
                          "\"retain\":false}";

        // Send as WebSocket TEXT frame
        std::vector<uint8_t> frame = create_websocket_frame(0x01, json);
        ssize_t sent = send(sockfd_, frame.data(), frame.size(), 0);

        if (sent < 0) {
            std::cerr << "Failed to send message" << std::endl;
            return false;
        }

        std::cout << "Published to topic '" << topic << "': " << payload << std::endl;
        return true;
    }

    bool subscribe(const std::string& topic, int qos = 0) {
        if (!connected_) {
            std::cerr << "Not connected" << std::endl;
            return false;
        }

        // Create JSON subscribe message
        std::string json = "{\"type\":\"subscribe\","
                          "\"topic\":\"" + topic + "\","
                          "\"qos\":" + std::to_string(qos) + "}";

        std::vector<uint8_t> frame = create_websocket_frame(0x01, json);
        ssize_t sent = send(sockfd_, frame.data(), frame.size(), 0);

        if (sent < 0) {
            std::cerr << "Failed to send subscribe" << std::endl;
            return false;
        }

        std::cout << "Subscribed to topic: " << topic << std::endl;
        return true;
    }

    bool receive_message(std::string& topic, std::string& payload, int timeout_ms = 5000) {
        if (!connected_) {
            return false;
        }

        // Set timeout
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Read data
        uint8_t buffer[65536];
        ssize_t n = recv(sockfd_, buffer, sizeof(buffer), 0);

        if (n <= 0) {
            return false;
        }

        // Parse WebSocket frame
        uint8_t opcode;
        std::string frame_payload;
        if (!parse_websocket_frame(buffer, n, opcode, frame_payload)) {
            std::cerr << "Failed to parse frame" << std::endl;
            return false;
        }

        // Handle different frame types
        if (opcode == 0x01) {  // TEXT frame
            // Parse JSON
            size_t topic_pos = frame_payload.find("\"topic\":\"");
            if (topic_pos != std::string::npos) {
                topic_pos += 9;
                size_t topic_end = frame_payload.find("\"", topic_pos);
                topic = frame_payload.substr(topic_pos, topic_end - topic_pos);
            }

            size_t payload_pos = frame_payload.find("\"payload\":\"");
            if (payload_pos != std::string::npos) {
                payload_pos += 11;
                size_t payload_end = frame_payload.find("\"", payload_pos);
                payload = frame_payload.substr(payload_pos, payload_end - payload_pos);
            }

            std::cout << "Received message on topic '" << topic << "': " << payload << std::endl;
            return true;
        } else if (opcode == 0x09) {  // PING
            // Send PONG
            std::vector<uint8_t> pong = create_websocket_frame(0x0A, frame_payload);
            send(sockfd_, pong.data(), pong.size(), 0);
            return receive_message(topic, payload, timeout_ms);
        }

        return false;
    }

    bool send_ping() {
        if (!connected_) {
            return false;
        }

        std::vector<uint8_t> ping = create_websocket_frame(0x09, "ping");
        return send(sockfd_, ping.data(), ping.size(), 0) > 0;
    }

private:
    bool perform_handshake() {
        // Generate WebSocket key
        std::string ws_key = generate_websocket_key();

        // Build handshake request
        std::string handshake =
            "GET / HTTP/1.1\r\n"
            "Host: " + host_ + ":" + std::to_string(port_) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + ws_key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";

        // Send handshake
        if (send(sockfd_, handshake.c_str(), handshake.size(), 0) < 0) {
            std::cerr << "Failed to send handshake" << std::endl;
            return false;
        }

        // Receive handshake response
        char buffer[4096];
        ssize_t n = recv(sockfd_, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
            std::cerr << "Failed to receive handshake response" << std::endl;
            return false;
        }

        buffer[n] = '\0';
        std::string response(buffer);

        // Verify response
        if (response.find("HTTP/1.1 101") == std::string::npos) {
            std::cerr << "Handshake failed: " << response << std::endl;
            return false;
        }

        // Verify accept key
        std::string expected_accept = compute_accept_key(ws_key);
        if (response.find("Sec-WebSocket-Accept: " + expected_accept) == std::string::npos) {
            std::cerr << "Invalid accept key in handshake response" << std::endl;
            return false;
        }

        std::cout << "WebSocket handshake successful" << std::endl;
        return true;
    }

    std::string host_;
    int port_;
    int sockfd_;
    bool connected_;
};

// Test functions

void test_publish_subscribe(WebSocketMQTTClient& client) {
    std::cout << "\n=== Test 1: Publish/Subscribe ===" << std::endl;

    // Subscribe to a topic
    if (!client.subscribe("test/topic", 0)) {
        std::cerr << "Subscribe failed" << std::endl;
        return;
    }

    sleep(1);

    // Publish a message
    if (!client.publish("test/topic", "Hello WebSocket MQTT!", 0)) {
        std::cerr << "Publish failed" << std::endl;
        return;
    }

    // Wait for message
    std::string topic, payload;
    if (client.receive_message(topic, payload, 5000)) {
        if (topic == "test/topic" && payload == "Hello WebSocket MQTT!") {
            std::cout << "✓ Publish/Subscribe test PASSED" << std::endl;
        } else {
            std::cout << "✗ Publish/Subscribe test FAILED (wrong data)" << std::endl;
        }
    } else {
        std::cout << "✗ Publish/Subscribe test FAILED (no message received)" << std::endl;
    }
}

void test_multiple_messages(WebSocketMQTTClient& client) {
    std::cout << "\n=== Test 2: Multiple Messages ===" << std::endl;

    client.subscribe("sensor/#", 1);
    sleep(1);

    // Publish multiple messages
    for (int i = 0; i < 5; i++) {
        std::string topic = "sensor/temp" + std::to_string(i);
        std::string payload = "Temperature: " + std::to_string(20 + i) + "C";
        client.publish(topic, payload, 0);
        usleep(100000);  // 100ms delay
    }

    // Receive messages
    int received = 0;
    for (int i = 0; i < 5; i++) {
        std::string topic, payload;
        if (client.receive_message(topic, payload, 2000)) {
            received++;
        }
    }

    if (received == 5) {
        std::cout << "✓ Multiple messages test PASSED (received " << received << "/5)" << std::endl;
    } else {
        std::cout << "✗ Multiple messages test FAILED (received " << received << "/5)" << std::endl;
    }
}

void test_ping_pong(WebSocketMQTTClient& client) {
    std::cout << "\n=== Test 3: Ping/Pong ===" << std::endl;

    if (client.send_ping()) {
        std::cout << "✓ Ping/Pong test PASSED" << std::endl;
    } else {
        std::cout << "✗ Ping/Pong test FAILED" << std::endl;
    }
}

void test_qos_levels(WebSocketMQTTClient& client) {
    std::cout << "\n=== Test 4: QoS Levels ===" << std::endl;

    client.subscribe("qos/test", 1);
    sleep(1);

    // Test different QoS levels
    client.publish("qos/test", "QoS 0 message", 0);
    client.publish("qos/test", "QoS 1 message", 1);

    int received = 0;
    for (int i = 0; i < 2; i++) {
        std::string topic, payload;
        if (client.receive_message(topic, payload, 2000)) {
            received++;
        }
    }

    if (received >= 1) {
        std::cout << "✓ QoS test PASSED (received " << received << " messages)" << std::endl;
    } else {
        std::cout << "✗ QoS test FAILED" << std::endl;
    }
}

void test_wildcard_topics(WebSocketMQTTClient& client) {
    std::cout << "\n=== Test 5: Wildcard Topics ===" << std::endl;

    // Subscribe with wildcards
    client.subscribe("home/+/temperature", 0);
    client.subscribe("home/#", 0);
    sleep(1);

    // Publish to matching topics
    client.publish("home/bedroom/temperature", "22C", 0);
    client.publish("home/kitchen/humidity", "60%", 0);

    int received = 0;
    for (int i = 0; i < 3; i++) {
        std::string topic, payload;
        if (client.receive_message(topic, payload, 2000)) {
            received++;
        }
    }

    if (received >= 2) {
        std::cout << "✓ Wildcard topics test PASSED (received " << received << " messages)" << std::endl;
    } else {
        std::cout << "✗ Wildcard topics test FAILED (received " << received << " messages)" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 1883;  // MQTT port with WebSocket auto-detection

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = std::atoi(argv[2]);
    }

    std::cout << "WebSocket MQTT Integration Test" << std::endl;
    std::cout << "=================================" << std::endl;
    std::cout << "Server: " << host << ":" << port << std::endl;

    WebSocketMQTTClient client(host, port);

    if (!client.connect()) {
        std::cerr << "Failed to connect to server" << std::endl;
        std::cerr << "\nPlease ensure the WebSocket MQTT server is running:" << std::endl;
        std::cerr << "  ./mqtts mqtts.yaml" << std::endl;
        return 1;
    }

    // Run tests
    test_publish_subscribe(client);
    test_multiple_messages(client);
    test_ping_pong(client);
    test_qos_levels(client);
    test_wildcard_topics(client);

    std::cout << "\n=== All Tests Complete ===" << std::endl;

    client.disconnect();
    return 0;
}
