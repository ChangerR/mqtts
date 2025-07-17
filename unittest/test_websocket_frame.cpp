#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include "src/websocket_frame.h"

void test_websocket_frame_creation()
{
    std::cout << "测试WebSocket帧创建..." << std::endl;
    
    websocket::WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = websocket::WebSocketOpcode::TEXT;
    frame.masked = false;
    frame.payload = std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o'};
    frame.payload_length = frame.payload.size();
    
    assert(frame.fin == true);
    assert(frame.opcode == websocket::WebSocketOpcode::TEXT);
    assert(frame.is_data_frame() == true);
    assert(frame.is_control_frame() == false);
    assert(frame.payload_length == 5);
    assert(frame.get_opcode_name() == "TEXT");
    
    std::cout << "WebSocket帧创建测试通过" << std::endl;
}

void test_websocket_frame_control()
{
    std::cout << "\n测试WebSocket控制帧..." << std::endl;
    
    websocket::WebSocketFrame ping_frame;
    ping_frame.fin = true;
    ping_frame.opcode = websocket::WebSocketOpcode::PING;
    ping_frame.masked = false;
    ping_frame.payload = std::vector<uint8_t>{'p', 'i', 'n', 'g'};
    ping_frame.payload_length = ping_frame.payload.size();
    
    assert(ping_frame.is_control_frame() == true);
    assert(ping_frame.is_data_frame() == false);
    assert(ping_frame.get_opcode_name() == "PING");
    
    websocket::WebSocketFrame close_frame;
    close_frame.fin = true;
    close_frame.opcode = websocket::WebSocketOpcode::CLOSE;
    close_frame.masked = false;
    close_frame.payload = std::vector<uint8_t>{0x03, 0xe8}; // 1000 = normal closure
    close_frame.payload_length = close_frame.payload.size();
    
    assert(close_frame.is_control_frame() == true);
    assert(close_frame.get_opcode_name() == "CLOSE");
    
    std::cout << "WebSocket控制帧测试通过" << std::endl;
}

void test_websocket_frame_parser()
{
    std::cout << "\n测试WebSocket帧解析器..." << std::endl;
    
    websocket::WebSocketFrameParser parser;
    
    // 测试简单的文本帧解析
    // FIN=1, RSV=000, opcode=0001 (TEXT), MASK=0, payload length=5
    uint8_t frame_data[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
    
    websocket::WebSocketFrame frame;
    size_t bytes_consumed = 0;
    
    int ret = parser.parse_frame(reinterpret_cast<const char*>(frame_data), 
                                sizeof(frame_data), frame, bytes_consumed);
    
    assert(ret == 0);
    assert(bytes_consumed == sizeof(frame_data));
    assert(frame.fin == true);
    assert(frame.opcode == websocket::WebSocketOpcode::TEXT);
    assert(frame.masked == false);
    assert(frame.payload_length == 5);
    assert(frame.payload.size() == 5);
    assert(frame.payload[0] == 'H');
    assert(frame.payload[4] == 'o');
    
    std::cout << "WebSocket帧解析器测试通过" << std::endl;
}

void test_websocket_frame_serialization()
{
    std::cout << "\n测试WebSocket帧序列化..." << std::endl;
    
    websocket::WebSocketFrameParser parser;
    
    // 创建一个文本帧
    websocket::WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = websocket::WebSocketOpcode::TEXT;
    frame.masked = false;
    frame.payload = std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o'};
    frame.payload_length = frame.payload.size();
    
    // 序列化帧
    std::vector<uint8_t> buffer;
    int ret = parser.serialize_frame(frame, buffer);
    
    assert(ret == 0);
    assert(buffer.size() == 7); // 2 bytes header + 5 bytes payload
    assert(buffer[0] == 0x81);  // FIN=1, opcode=TEXT
    assert(buffer[1] == 0x05);  // MASK=0, payload length=5
    assert(buffer[2] == 'H');
    assert(buffer[6] == 'o');
    
    std::cout << "WebSocket帧序列化测试通过" << std::endl;
}

void test_websocket_frame_masking()
{
    std::cout << "\n测试WebSocket帧掩码..." << std::endl;
    
    websocket::WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = websocket::WebSocketOpcode::TEXT;
    frame.masked = true;
    frame.masking_key[0] = 0x37;
    frame.masking_key[1] = 0xfa;
    frame.masking_key[2] = 0x21;
    frame.masking_key[3] = 0x3d;
    frame.payload = std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o'};
    frame.payload_length = frame.payload.size();
    
    // 保存原始载荷
    std::vector<uint8_t> original_payload = frame.payload;
    
    // 应用掩码
    frame.apply_mask();
    
    // 验证载荷已被掩码
    assert(frame.payload[0] == ('H' ^ 0x37));
    assert(frame.payload[1] == ('e' ^ 0xfa));
    assert(frame.payload[2] == ('l' ^ 0x21));
    assert(frame.payload[3] == ('l' ^ 0x3d));
    assert(frame.payload[4] == ('o' ^ 0x37));
    
    // 再次应用掩码应该恢复原始载荷
    frame.apply_mask();
    
    assert(frame.payload == original_payload);
    
    std::cout << "WebSocket帧掩码测试通过" << std::endl;
}

void test_websocket_frame_validation()
{
    std::cout << "\n测试WebSocket帧验证..." << std::endl;
    
    websocket::WebSocketFrameParser parser;
    
    // 测试有效帧
    websocket::WebSocketFrame valid_frame;
    valid_frame.fin = true;
    valid_frame.opcode = websocket::WebSocketOpcode::TEXT;
    valid_frame.masked = false;
    valid_frame.payload = std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o'};
    valid_frame.payload_length = valid_frame.payload.size();
    
    assert(parser.is_valid_frame(valid_frame) == true);
    
    // 测试无效控制帧（载荷太长）
    websocket::WebSocketFrame invalid_control_frame;
    invalid_control_frame.fin = true;
    invalid_control_frame.opcode = websocket::WebSocketOpcode::PING;
    invalid_control_frame.masked = false;
    invalid_control_frame.payload = std::vector<uint8_t>(130, 'x'); // 超过125字节
    invalid_control_frame.payload_length = invalid_control_frame.payload.size();
    
    assert(parser.is_valid_frame(invalid_control_frame) == false);
    
    std::cout << "WebSocket帧验证测试通过" << std::endl;
}

void test_websocket_close_codes()
{
    std::cout << "\n测试WebSocket关闭代码..." << std::endl;
    
    // 测试关闭代码转换
    websocket::WebSocketCloseCode code = websocket::websocket_close_code_from_uint16(1000);
    assert(code == websocket::WebSocketCloseCode::NORMAL_CLOSURE);
    
    std::string code_str = websocket::websocket_close_code_to_string(websocket::WebSocketCloseCode::PROTOCOL_ERROR);
    assert(code_str == "Protocol Error");
    
    code_str = websocket::websocket_close_code_to_string(websocket::WebSocketCloseCode::MESSAGE_TOO_BIG);
    assert(code_str == "Message Too Big");
    
    std::cout << "WebSocket关闭代码测试通过" << std::endl;
}

int main()
{
    std::cout << "开始WebSocket帧测试..." << std::endl;
    
    test_websocket_frame_creation();
    test_websocket_frame_control();
    test_websocket_frame_parser();
    test_websocket_frame_serialization();
    test_websocket_frame_masking();
    test_websocket_frame_validation();
    test_websocket_close_codes();
    
    std::cout << "\n所有WebSocket帧测试通过！" << std::endl;
    return 0;
}