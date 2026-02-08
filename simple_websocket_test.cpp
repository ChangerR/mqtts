#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <cstring>

// 简化的WebSocket帧结构
struct SimpleWebSocketFrame {
    bool fin;
    uint8_t opcode;
    bool masked;
    uint64_t payload_length;
    uint8_t masking_key[4];
    std::vector<uint8_t> payload;
    
    SimpleWebSocketFrame() : fin(false), opcode(0), masked(false), payload_length(0) {
        masking_key[0] = masking_key[1] = masking_key[2] = masking_key[3] = 0;
    }
    
    void apply_mask() {
        if (masked && !payload.empty()) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] ^= masking_key[i % 4];
            }
        }
    }
};

// 简化的WebSocket帧解析器
class SimpleWebSocketFrameParser {
public:
    int parse_frame(const char* buffer, size_t buffer_size, SimpleWebSocketFrame& frame, size_t& bytes_consumed) {
        if (buffer_size < 2) {
            return -1; // Need at least 2 bytes for header
        }
        
        uint8_t first_byte = buffer[0];
        uint8_t second_byte = buffer[1];
        
        // Parse first byte
        frame.fin = (first_byte & 0x80) != 0;
        frame.opcode = first_byte & 0x0F;
        
        // Parse second byte
        frame.masked = (second_byte & 0x80) != 0;
        uint8_t payload_len = second_byte & 0x7F;
        
        size_t offset = 2;
        
        // Parse extended payload length
        if (payload_len == 126) {
            if (buffer_size < offset + 2) {
                return -1;
            }
            frame.payload_length = (static_cast<uint16_t>(buffer[offset]) << 8) | buffer[offset + 1];
            offset += 2;
        } else if (payload_len == 127) {
            if (buffer_size < offset + 8) {
                return -1;
            }
            frame.payload_length = 0;
            for (int i = 0; i < 8; ++i) {
                frame.payload_length = (frame.payload_length << 8) | buffer[offset + i];
            }
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
        
        // Check if we have enough data for payload
        if (buffer_size < offset + frame.payload_length) {
            return -1;
        }
        
        // Read payload
        if (frame.payload_length > 0) {
            frame.payload.resize(frame.payload_length);
            memcpy(frame.payload.data(), buffer + offset, frame.payload_length);
            
            // Apply masking if needed
            if (frame.masked) {
                frame.apply_mask();
            }
        }
        
        bytes_consumed = offset + frame.payload_length;
        return 0;
    }
    
    int serialize_frame(const SimpleWebSocketFrame& frame, std::vector<uint8_t>& buffer) {
        buffer.clear();
        
        // First byte: FIN + opcode
        uint8_t first_byte = 0;
        if (frame.fin) first_byte |= 0x80;
        first_byte |= frame.opcode & 0x0F;
        buffer.push_back(first_byte);
        
        // Second byte: MASK + payload length
        uint8_t second_byte = 0;
        if (frame.masked) second_byte |= 0x80;
        
        if (frame.payload_length < 126) {
            second_byte |= static_cast<uint8_t>(frame.payload_length);
            buffer.push_back(second_byte);
        } else if (frame.payload_length < 65536) {
            second_byte |= 126;
            buffer.push_back(second_byte);
            buffer.push_back(static_cast<uint8_t>(frame.payload_length >> 8));
            buffer.push_back(static_cast<uint8_t>(frame.payload_length & 0xFF));
        } else {
            second_byte |= 127;
            buffer.push_back(second_byte);
            for (int i = 7; i >= 0; --i) {
                buffer.push_back(static_cast<uint8_t>((frame.payload_length >> (i * 8)) & 0xFF));
            }
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
};

// 测试函数
void test_websocket_frame_creation() {
    std::cout << "Test: WebSocket frame creation..." << std::endl;
    
    SimpleWebSocketFrame frame;
    frame.fin = true;
    frame.opcode = 0x1; // TEXT
    frame.masked = false;
    frame.payload = std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o'};
    frame.payload_length = frame.payload.size();
    
    assert(frame.fin == true);
    assert(frame.opcode == 0x1);
    assert(frame.payload_length == 5);
    
    std::cout << "✓ WebSocket frame creation test passed" << std::endl;
}

void test_websocket_frame_parsing() {
    std::cout << "Test: WebSocket frame parsing..." << std::endl;
    
    // Test simple text frame: FIN=1, opcode=1, no mask, payload="Hello"
    uint8_t frame_data[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
    
    SimpleWebSocketFrameParser parser;
    SimpleWebSocketFrame frame;
    size_t bytes_consumed = 0;
    
    int ret = parser.parse_frame(reinterpret_cast<const char*>(frame_data), 
                                sizeof(frame_data), frame, bytes_consumed);
    
    assert(ret == 0);
    assert(bytes_consumed == sizeof(frame_data));
    assert(frame.fin == true);
    assert(frame.opcode == 0x1);
    assert(frame.masked == false);
    assert(frame.payload_length == 5);
    assert(frame.payload.size() == 5);
    assert(frame.payload[0] == 'H');
    assert(frame.payload[4] == 'o');
    
    std::cout << "✓ WebSocket frame parsing test passed" << std::endl;
}

void test_websocket_frame_serialization() {
    std::cout << "Test: WebSocket frame serialization..." << std::endl;
    
    SimpleWebSocketFrame frame;
    frame.fin = true;
    frame.opcode = 0x1; // TEXT
    frame.masked = false;
    frame.payload = std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o'};
    frame.payload_length = frame.payload.size();
    
    SimpleWebSocketFrameParser parser;
    std::vector<uint8_t> buffer;
    
    int ret = parser.serialize_frame(frame, buffer);
    
    assert(ret == 0);
    assert(buffer.size() == 7); // 2 bytes header + 5 bytes payload
    assert(buffer[0] == 0x81);  // FIN=1, opcode=1
    assert(buffer[1] == 0x05);  // MASK=0, payload length=5
    assert(buffer[2] == 'H');
    assert(buffer[6] == 'o');
    
    std::cout << "✓ WebSocket frame serialization test passed" << std::endl;
}

void test_websocket_frame_masking() {
    std::cout << "Test: WebSocket frame masking..." << std::endl;
    
    SimpleWebSocketFrame frame;
    frame.fin = true;
    frame.opcode = 0x1; // TEXT
    frame.masked = true;
    frame.masking_key[0] = 0x37;
    frame.masking_key[1] = 0xfa;
    frame.masking_key[2] = 0x21;
    frame.masking_key[3] = 0x3d;
    frame.payload = std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o'};
    frame.payload_length = frame.payload.size();
    
    // Save original payload
    std::vector<uint8_t> original_payload = frame.payload;
    
    // Apply masking
    frame.apply_mask();
    
    // Verify payload is masked
    assert(frame.payload[0] == ('H' ^ 0x37));
    assert(frame.payload[1] == ('e' ^ 0xfa));
    assert(frame.payload[2] == ('l' ^ 0x21));
    assert(frame.payload[3] == ('l' ^ 0x3d));
    assert(frame.payload[4] == ('o' ^ 0x37));
    
    // Apply masking again should restore original
    frame.apply_mask();
    assert(frame.payload == original_payload);
    
    std::cout << "✓ WebSocket frame masking test passed" << std::endl;
}

void test_websocket_extended_payload_length() {
    std::cout << "Test: WebSocket extended payload length..." << std::endl;
    
    SimpleWebSocketFrame frame;
    frame.fin = true;
    frame.opcode = 0x1; // TEXT
    frame.masked = false;
    
    // Test 126 bytes payload (extended length)
    std::vector<uint8_t> payload_126(126, 'A');
    frame.payload = payload_126;
    frame.payload_length = frame.payload.size();
    
    SimpleWebSocketFrameParser parser;
    std::vector<uint8_t> buffer;
    
    int ret = parser.serialize_frame(frame, buffer);
    assert(ret == 0);
    assert(buffer.size() == 4 + 126); // 1 + 1 + 2 + 126
    assert(buffer[0] == 0x81);  // FIN=1, opcode=1
    assert(buffer[1] == 126);   // Extended length indicator
    assert(buffer[2] == 0);     // Length high byte
    assert(buffer[3] == 126);   // Length low byte
    
    std::cout << "✓ WebSocket extended payload length test passed" << std::endl;
}

int main() {
    std::cout << "Running WebSocket frame tests..." << std::endl;
    
    test_websocket_frame_creation();
    test_websocket_frame_parsing();
    test_websocket_frame_serialization();
    test_websocket_frame_masking();
    test_websocket_extended_payload_length();
    
    std::cout << "\n🎉 All WebSocket frame tests passed!" << std::endl;
    return 0;
}