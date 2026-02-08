#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include "src/mqtt_allocator.h"
#include "src/websocket_frame.h"

using namespace websocket;

void test_parse_text_frame_unmasked() {
    std::cout << "Testing TEXT frame parsing (unmasked)..." << std::endl;

    // TEXT frame: FIN=1, opcode=1, not masked, payload="Hello"
    uint8_t frame_data[] = {
        0x81,  // FIN=1, RSV=000, opcode=0001 (TEXT)
        0x05,  // MASK=0, payload_len=5
        'H', 'e', 'l', 'l', 'o'
    };

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    WebSocketFrame frame(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frame_data, sizeof(frame_data), frame, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(bytes_consumed == sizeof(frame_data));
    assert(frame.fin == true);
    assert(frame.opcode == WebSocketOpcode::TEXT);
    assert(frame.masked == false);
    assert(frame.payload_length == 5);
    assert(frame.payload.size() == 5);
    assert(std::string(frame.payload.begin(), frame.payload.end()) == "Hello");

    std::cout << "TEXT frame parsing test passed" << std::endl;
}

void test_parse_text_frame_masked() {
    std::cout << "\nTesting TEXT frame parsing (masked)..." << std::endl;

    // TEXT frame: FIN=1, opcode=1, masked, payload="Test"
    uint8_t frame_data[] = {
        0x81,  // FIN=1, RSV=000, opcode=0001 (TEXT)
        0x84,  // MASK=1, payload_len=4
        0x12, 0x34, 0x56, 0x78,  // Masking key
        0x46, 0x41, 0x33, 0x0C   // Masked payload "Test" XOR with mask
    };

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    WebSocketFrame frame(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frame_data, sizeof(frame_data), frame, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(bytes_consumed == sizeof(frame_data));
    assert(frame.fin == true);
    assert(frame.opcode == WebSocketOpcode::TEXT);
    assert(frame.masked == true);
    assert(frame.payload_length == 4);
    assert(std::string(frame.payload.begin(), frame.payload.end()) == "Test");

    std::cout << "Masked TEXT frame parsing test passed" << std::endl;
}

void test_parse_binary_frame() {
    std::cout << "\nTesting BINARY frame parsing..." << std::endl;

    // BINARY frame: FIN=1, opcode=2, not masked
    uint8_t frame_data[] = {
        0x82,  // FIN=1, RSV=000, opcode=0010 (BINARY)
        0x04,  // MASK=0, payload_len=4
        0x01, 0x02, 0x03, 0x04
    };

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    WebSocketFrame frame(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frame_data, sizeof(frame_data), frame, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(frame.opcode == WebSocketOpcode::BINARY);
    assert(frame.payload_length == 4);
    assert(frame.payload[0] == 0x01);
    assert(frame.payload[3] == 0x04);

    std::cout << "BINARY frame parsing test passed" << std::endl;
}

void test_parse_ping_frame() {
    std::cout << "\nTesting PING frame parsing..." << std::endl;

    // PING frame: FIN=1, opcode=9, payload="ping"
    uint8_t frame_data[] = {
        0x89,  // FIN=1, RSV=000, opcode=1001 (PING)
        0x04,  // MASK=0, payload_len=4
        'p', 'i', 'n', 'g'
    };

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    WebSocketFrame frame(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frame_data, sizeof(frame_data), frame, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(frame.opcode == WebSocketOpcode::PING);
    assert(frame.is_control_frame() == true);
    assert(frame.fin == true);  // Control frames must have FIN=1
    assert(std::string(frame.payload.begin(), frame.payload.end()) == "ping");

    std::cout << "PING frame parsing test passed" << std::endl;
}

void test_parse_pong_frame() {
    std::cout << "\nTesting PONG frame parsing..." << std::endl;

    // PONG frame: FIN=1, opcode=10
    uint8_t frame_data[] = {
        0x8A,  // FIN=1, RSV=000, opcode=1010 (PONG)
        0x00   // MASK=0, payload_len=0
    };

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    WebSocketFrame frame(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frame_data, sizeof(frame_data), frame, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(frame.opcode == WebSocketOpcode::PONG);
    assert(frame.is_control_frame() == true);
    assert(frame.payload_length == 0);

    std::cout << "PONG frame parsing test passed" << std::endl;
}

void test_parse_close_frame() {
    std::cout << "\nTesting CLOSE frame parsing..." << std::endl;

    // CLOSE frame: FIN=1, opcode=8, code=1000, reason="Normal"
    uint8_t frame_data[] = {
        0x88,  // FIN=1, RSV=000, opcode=1000 (CLOSE)
        0x08,  // MASK=0, payload_len=8
        0x03, 0xE8,  // Status code 1000 (NORMAL)
        'N', 'o', 'r', 'm', 'a', 'l'
    };

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    WebSocketFrame frame(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frame_data, sizeof(frame_data), frame, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(frame.opcode == WebSocketOpcode::CLOSE);

    uint16_t code = 0;
    std::string reason;
    ret = frame.get_close_info(code, reason);
    assert(ret == MQ_SUCCESS);
    assert(code == 1000);
    assert(reason == "Normal");

    std::cout << "CLOSE frame parsing test passed" << std::endl;
}

void test_parse_extended_length_16bit() {
    std::cout << "\nTesting extended payload length (16-bit)..." << std::endl;

    // Create a frame with payload length = 126 (requires 16-bit length)
    std::vector<uint8_t> frame_data;
    frame_data.push_back(0x82);  // FIN=1, opcode=2 (BINARY)
    frame_data.push_back(126);   // MASK=0, payload_len=126 (use next 2 bytes)
    frame_data.push_back(0x00);  // Length high byte
    frame_data.push_back(0x7E);  // Length low byte (126)

    // Add 126 bytes of payload
    for (int i = 0; i < 126; ++i) {
        frame_data.push_back(static_cast<uint8_t>(i));
    }

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    WebSocketFrame frame(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frame_data.data(), frame_data.size(), frame, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(frame.payload_length == 126);
    assert(frame.payload.size() == 126);
    assert(frame.payload[0] == 0);
    assert(frame.payload[125] == 125);

    std::cout << "Extended 16-bit length test passed" << std::endl;
}

void test_parse_extended_length_64bit() {
    std::cout << "\nTesting extended payload length (64-bit)..." << std::endl;

    // Create a frame with payload length = 65536 (requires 64-bit length)
    std::vector<uint8_t> frame_data;
    frame_data.push_back(0x82);  // FIN=1, opcode=2 (BINARY)
    frame_data.push_back(127);   // MASK=0, payload_len=127 (use next 8 bytes)

    // 64-bit length = 65536 (0x00010000)
    frame_data.push_back(0x00);
    frame_data.push_back(0x00);
    frame_data.push_back(0x00);
    frame_data.push_back(0x00);
    frame_data.push_back(0x00);
    frame_data.push_back(0x01);  // High byte
    frame_data.push_back(0x00);
    frame_data.push_back(0x00);  // Low byte

    // Add 65536 bytes of payload
    for (int i = 0; i < 65536; ++i) {
        frame_data.push_back(static_cast<uint8_t>(i % 256));
    }

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    WebSocketFrame frame(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frame_data.data(), frame_data.size(), frame, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(frame.payload_length == 65536);
    assert(frame.payload.size() == 65536);
    assert(frame.payload[0] == 0);
    assert(frame.payload[255] == 255);
    assert(frame.payload[65535] == 255);

    std::cout << "Extended 64-bit length test passed" << std::endl;
}

void test_serialize_text_frame() {
    std::cout << "\nTesting TEXT frame serialization..." << std::endl;

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    // Create a text frame
    WebSocketFrame frame(&allocator);
    frame.fin = true;
    frame.opcode = WebSocketOpcode::TEXT;
    frame.masked = false;
    std::string text = "Hello WebSocket";
    frame.payload.assign(text.begin(), text.end());
    frame.payload_length = frame.payload.size();

    // Serialize
    std::vector<uint8_t> output;
    int ret = parser.serialize_frame(frame, output);

    assert(ret == MQ_SUCCESS);
    assert(output.size() > 0);
    assert(output[0] == 0x81);  // FIN=1, opcode=TEXT
    assert(output[1] == 15);    // payload length

    // Parse it back
    WebSocketFrame parsed_frame(&allocator);
    size_t bytes_consumed = 0;
    ret = parser.parse_frame(output.data(), output.size(), parsed_frame, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(parsed_frame.fin == frame.fin);
    assert(parsed_frame.opcode == frame.opcode);
    assert(parsed_frame.payload == frame.payload);

    std::cout << "TEXT frame serialization test passed" << std::endl;
}

void test_masking() {
    std::cout << "\nTesting masking/unmasking..." << std::endl;

    uint8_t data[] = {'H', 'e', 'l', 'l', 'o'};
    uint8_t mask[] = {0x12, 0x34, 0x56, 0x78};
    uint8_t original[5];
    std::memcpy(original, data, 5);

    // Apply mask
    WebSocketFrameParser::apply_mask(data, 5, mask);

    // Verify it changed
    bool changed = false;
    for (int i = 0; i < 5; ++i) {
        if (data[i] != original[i]) {
            changed = true;
            break;
        }
    }
    assert(changed);

    // Apply mask again (should restore original)
    WebSocketFrameParser::apply_mask(data, 5, mask);

    for (int i = 0; i < 5; ++i) {
        assert(data[i] == original[i]);
    }

    std::cout << "Masking/unmasking test passed" << std::endl;
}

void test_incomplete_frame() {
    std::cout << "\nTesting incomplete frame handling..." << std::endl;

    // Only header, no payload
    uint8_t frame_data[] = {
        0x81,  // FIN=1, opcode=TEXT
        0x05   // payload_len=5, but no payload follows
    };

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    WebSocketFrame frame(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frame_data, sizeof(frame_data), frame, bytes_consumed);

    assert(ret == MQ_ERR_WS_INCOMPLETE_FRAME);

    std::cout << "Incomplete frame handling test passed" << std::endl;
}

void test_invalid_opcode() {
    std::cout << "\nTesting invalid opcode detection..." << std::endl;

    // Invalid opcode = 0x03 (reserved)
    uint8_t frame_data[] = {
        0x83,  // FIN=1, opcode=0011 (reserved)
        0x00   // payload_len=0
    };

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    WebSocketFrame frame(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frame_data, sizeof(frame_data), frame, bytes_consumed);

    assert(ret == MQ_ERR_WS_INVALID_OPCODE);

    std::cout << "Invalid opcode detection test passed" << std::endl;
}

void test_control_frame_too_large() {
    std::cout << "\nTesting control frame size limit..." << std::endl;

    // PING frame with payload > 125 bytes (invalid)
    std::vector<uint8_t> frame_data;
    frame_data.push_back(0x89);  // FIN=1, opcode=PING
    frame_data.push_back(126);   // payload_len=126 (use next 2 bytes)
    frame_data.push_back(0x00);
    frame_data.push_back(0x7E);  // 126 bytes

    for (int i = 0; i < 126; ++i) {
        frame_data.push_back(0x00);
    }

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    WebSocketFrame frame(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frame_data.data(), frame_data.size(), frame, bytes_consumed);

    assert(ret == MQ_ERR_WS_PROTOCOL);

    std::cout << "Control frame size limit test passed" << std::endl;
}

void test_utf8_validation() {
    std::cout << "\nTesting UTF-8 validation..." << std::endl;

    // Valid UTF-8
    const char* valid_utf8 = "Hello 世界 🌍";
    assert(WebSocketFrameParser::is_valid_utf8(
        reinterpret_cast<const uint8_t*>(valid_utf8), strlen(valid_utf8)));

    // Invalid UTF-8 (invalid continuation byte)
    uint8_t invalid_utf8[] = {0xC0, 0x80};  // Overlong encoding
    assert(!WebSocketFrameParser::is_valid_utf8(invalid_utf8, sizeof(invalid_utf8)));

    // Invalid UTF-8 (truncated)
    uint8_t truncated_utf8[] = {0xE4, 0xB8};  // Incomplete 3-byte sequence
    assert(!WebSocketFrameParser::is_valid_utf8(truncated_utf8, sizeof(truncated_utf8)));

    std::cout << "UTF-8 validation test passed" << std::endl;
}

void test_fragmented_message() {
    std::cout << "\nTesting fragmented message..." << std::endl;

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    WebSocketFrameParser parser(&allocator);

    // First fragment: FIN=0, opcode=TEXT
    uint8_t frag1[] = {
        0x01,  // FIN=0, opcode=TEXT
        0x03,  // payload_len=3
        'H', 'e', 'l'
    };

    WebSocketFrame frame1(&allocator);
    size_t bytes_consumed = 0;
    int ret = parser.parse_frame(frag1, sizeof(frag1), frame1, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(frame1.fin == false);
    assert(frame1.opcode == WebSocketOpcode::TEXT);

    // Continuation fragment: FIN=0, opcode=CONTINUATION
    uint8_t frag2[] = {
        0x00,  // FIN=0, opcode=CONTINUATION
        0x02,  // payload_len=2
        'l', 'o'
    };

    WebSocketFrame frame2(&allocator);
    ret = parser.parse_frame(frag2, sizeof(frag2), frame2, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(frame2.fin == false);
    assert(frame2.opcode == WebSocketOpcode::CONTINUATION);

    // Final fragment: FIN=1, opcode=CONTINUATION
    uint8_t frag3[] = {
        0x80,  // FIN=1, opcode=CONTINUATION
        0x01,  // payload_len=1
        '!'
    };

    WebSocketFrame frame3(&allocator);
    ret = parser.parse_frame(frag3, sizeof(frag3), frame3, bytes_consumed);

    assert(ret == MQ_SUCCESS);
    assert(frame3.fin == true);
    assert(frame3.opcode == WebSocketOpcode::CONTINUATION);

    // Reconstruct message
    std::string message;
    message.append(frame1.payload.begin(), frame1.payload.end());
    message.append(frame2.payload.begin(), frame2.payload.end());
    message.append(frame3.payload.begin(), frame3.payload.end());
    assert(message == "Hello!");

    std::cout << "Fragmented message test passed" << std::endl;
}

void test_close_frame_with_code() {
    std::cout << "\nTesting CLOSE frame with status code..." << std::endl;

    MQTTAllocator allocator("test_ws", MQTTMemoryTag::MEM_TAG_CLIENT, 0);

    // Create close frame
    WebSocketFrame frame(&allocator);
    frame.fin = true;
    frame.opcode = WebSocketOpcode::CLOSE;

    int ret = frame.set_close_info(1001, "Going away");
    assert(ret == MQ_SUCCESS);

    // Verify
    uint16_t code = 0;
    std::string reason;
    ret = frame.get_close_info(code, reason);
    assert(ret == MQ_SUCCESS);
    assert(code == 1001);
    assert(reason == "Going away");

    std::cout << "CLOSE frame with status code test passed" << std::endl;
}

int main() {
    std::cout << "Starting WebSocket frame parser tests\n" << std::endl;

    try {
        test_parse_text_frame_unmasked();
        test_parse_text_frame_masked();
        test_parse_binary_frame();
        test_parse_ping_frame();
        test_parse_pong_frame();
        test_parse_close_frame();
        test_parse_extended_length_16bit();
        test_parse_extended_length_64bit();
        test_serialize_text_frame();
        test_masking();
        test_incomplete_frame();
        test_invalid_opcode();
        test_control_frame_too_large();
        test_utf8_validation();
        test_fragmented_message();
        test_close_frame_with_code();

        std::cout << "\n✓ All WebSocket frame tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed: " << e.what() << std::endl;
        return 1;
    }
}
