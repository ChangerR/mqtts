#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include "src/mqtt_allocator.h"
#include "src/websocket_mqtt_bridge.h"

void test_websocket_mqtt_bridge_creation()
{
    std::cout << "测试WebSocket-MQTT桥接器创建..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    
    // 测试默认配置
    assert(bridge.get_message_format() == websocket::WebSocketMQTTBridge::MessageFormat::JSON);
    
    // 测试统计信息初始化
    auto& stats = bridge.get_statistics();
    assert(stats.websocket_messages_received == 0);
    assert(stats.websocket_messages_sent == 0);
    assert(stats.mqtt_messages_received == 0);
    assert(stats.mqtt_messages_sent == 0);
    assert(stats.active_clients == 0);
    
    std::cout << "WebSocket-MQTT桥接器创建测试通过" << std::endl;
}

void test_json_message_parsing()
{
    std::cout << "\n测试JSON消息解析..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    
    // 测试发布消息解析
    std::string json_publish = R"({
        "type": "publish",
        "topic": "test/topic",
        "payload": "SGVsbG8gV29ybGQ=",
        "qos": 1,
        "retain": false
    })";
    
    std::string topic;
    std::vector<uint8_t> payload;
    uint8_t qos;
    bool retain;
    
    int ret = bridge.parse_json_publish(json_publish, topic, payload, qos, retain);
    
    assert(ret == 0);
    assert(topic == "test/topic");
    assert(qos == 1);
    assert(retain == false);
    // payload应该是base64解码后的 "Hello World"
    
    std::cout << "JSON消息解析测试通过" << std::endl;
}

void test_json_subscribe_parsing()
{
    std::cout << "\n测试JSON订阅解析..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    
    // 测试订阅消息解析
    std::string json_subscribe = R"({
        "type": "subscribe",
        "topic_filter": "test/+",
        "qos": 2
    })";
    
    std::string topic_filter;
    uint8_t qos;
    
    int ret = bridge.parse_json_subscribe(json_subscribe, topic_filter, qos);
    
    assert(ret == 0);
    assert(topic_filter == "test/+");
    assert(qos == 2);
    
    std::cout << "JSON订阅解析测试通过" << std::endl;
}

void test_json_message_generation()
{
    std::cout << "\n测试JSON消息生成..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    
    // 测试发布消息生成
    std::string topic = "test/topic";
    std::vector<uint8_t> payload = {'H', 'e', 'l', 'l', 'o'};
    uint8_t qos = 1;
    bool retain = false;
    
    std::string json_message = bridge.generate_json_publish(topic, payload, qos, retain);
    
    // 验证JSON消息包含必要字段
    assert(json_message.find("\"type\":\"publish\"") != std::string::npos);
    assert(json_message.find("\"topic\":\"test/topic\"") != std::string::npos);
    assert(json_message.find("\"qos\":1") != std::string::npos);
    assert(json_message.find("\"retain\":false") != std::string::npos);
    
    std::cout << "JSON消息生成测试通过" << std::endl;
}

void test_text_protocol_parsing()
{
    std::cout << "\n测试文本协议解析..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    
    bridge.set_message_format(websocket::WebSocketMQTTBridge::MessageFormat::TEXT_PROTOCOL);
    
    // 测试发布消息解析 (格式: PUB topic qos retain payload)
    std::string text_publish = "PUB test/topic 1 0 Hello World";
    
    std::string topic;
    std::vector<uint8_t> payload;
    uint8_t qos;
    bool retain;
    
    int ret = bridge.parse_text_publish(text_publish, topic, payload, qos, retain);
    
    assert(ret == 0);
    assert(topic == "test/topic");
    assert(qos == 1);
    assert(retain == false);
    assert(payload.size() == 11); // "Hello World" length
    
    std::cout << "文本协议解析测试通过" << std::endl;
}

void test_client_management()
{
    std::cout << "\n测试客户端管理..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    
    std::string client_id = "test_client_001";
    
    // 测试客户端连接
    int ret = bridge.handle_client_connect(client_id, "test_user");
    assert(ret == 0);
    
    // 验证统计信息更新
    auto& stats = bridge.get_statistics();
    assert(stats.active_clients == 1);
    
    // 测试客户端断开
    ret = bridge.handle_client_disconnect(client_id, "normal_closure");
    assert(ret == 0);
    
    std::cout << "客户端管理测试通过" << std::endl;
}

void test_subscription_management()
{
    std::cout << "\n测试订阅管理..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    
    std::string client_id = "test_client_002";
    std::string topic_filter = "test/+";
    uint8_t qos = 1;
    
    // 测试订阅
    int ret = bridge.subscribe_topic(client_id, topic_filter, qos);
    assert(ret == 0);
    
    // 验证统计信息更新
    auto& stats = bridge.get_statistics();
    assert(stats.active_subscriptions == 1);
    
    // 测试取消订阅
    ret = bridge.unsubscribe_topic(client_id, topic_filter);
    assert(ret == 0);
    
    // 测试取消所有订阅
    bridge.subscribe_topic(client_id, "topic1", 0);
    bridge.subscribe_topic(client_id, "topic2", 1);
    
    ret = bridge.unsubscribe_all_topics(client_id);
    assert(ret == 0);
    
    std::cout << "订阅管理测试通过" << std::endl;
}

void test_message_format_switching()
{
    std::cout << "\n测试消息格式切换..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    
    // 测试默认格式
    assert(bridge.get_message_format() == websocket::WebSocketMQTTBridge::MessageFormat::JSON);
    
    // 切换到文本协议
    bridge.set_message_format(websocket::WebSocketMQTTBridge::MessageFormat::TEXT_PROTOCOL);
    assert(bridge.get_message_format() == websocket::WebSocketMQTTBridge::MessageFormat::TEXT_PROTOCOL);
    
    // 切换到MQTT数据包格式
    bridge.set_message_format(websocket::WebSocketMQTTBridge::MessageFormat::MQTT_PACKET);
    assert(bridge.get_message_format() == websocket::WebSocketMQTTBridge::MessageFormat::MQTT_PACKET);
    
    std::cout << "消息格式切换测试通过" << std::endl;
}

void test_base64_encoding()
{
    std::cout << "\n测试Base64编码..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    
    // 测试Base64编码
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    std::string encoded = bridge.base64_encode(data);
    
    assert(encoded == "SGVsbG8=");
    
    // 测试Base64解码
    std::vector<uint8_t> decoded = bridge.base64_decode(encoded);
    
    assert(decoded.size() == 5);
    assert(decoded[0] == 'H');
    assert(decoded[4] == 'o');
    
    std::cout << "Base64编码测试通过" << std::endl;
}

void test_error_handling()
{
    std::cout << "\n测试错误处理..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    
    // 测试无效JSON解析
    std::string invalid_json = "{ invalid json }";
    std::string topic;
    std::vector<uint8_t> payload;
    uint8_t qos;
    bool retain;
    
    int ret = bridge.parse_json_publish(invalid_json, topic, payload, qos, retain);
    assert(ret != 0); // 应该返回错误
    
    // 验证错误统计
    auto& stats = bridge.get_statistics();
    assert(stats.translation_errors > 0);
    
    std::cout << "错误处理测试通过" << std::endl;
}

int main()
{
    std::cout << "开始WebSocket-MQTT桥接器测试..." << std::endl;
    
    test_websocket_mqtt_bridge_creation();
    test_json_message_parsing();
    test_json_subscribe_parsing();
    test_json_message_generation();
    test_text_protocol_parsing();
    test_client_management();
    test_subscription_management();
    test_message_format_switching();
    test_base64_encoding();
    test_error_handling();
    
    std::cout << "\n所有WebSocket-MQTT桥接器测试通过！" << std::endl;
    return 0;
}