#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

// 简化的JSON解析器
class SimpleJSONParser {
public:
    static std::string extract_string_value(const std::string& json, const std::string& key) {
        std::string search_key = "\"" + key + "\":\"";
        size_t start = json.find(search_key);
        if (start == std::string::npos) {
            return "";
        }
        
        start += search_key.length();
        size_t end = json.find("\"", start);
        if (end == std::string::npos) {
            return "";
        }
        
        return json.substr(start, end - start);
    }
    
    static int extract_int_value(const std::string& json, const std::string& key) {
        std::string search_key = "\"" + key + "\":";
        size_t start = json.find(search_key);
        if (start == std::string::npos) {
            return -1;
        }
        
        start += search_key.length();
        size_t end = json.find_first_of(",}", start);
        if (end == std::string::npos) {
            return -1;
        }
        
        std::string value_str = json.substr(start, end - start);
        return std::stoi(value_str);
    }
    
    static bool extract_bool_value(const std::string& json, const std::string& key) {
        std::string search_key = "\"" + key + "\":";
        size_t start = json.find(search_key);
        if (start == std::string::npos) {
            return false;
        }
        
        start += search_key.length();
        return json.substr(start, 4) == "true";
    }
};

// 简化的Base64编码器
class SimpleBase64 {
public:
    static std::string encode(const std::vector<uint8_t>& data) {
        const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        
        size_t i = 0;
        uint8_t char_array_3[3];
        uint8_t char_array_4[4];
        
        for (uint8_t byte : data) {
            char_array_3[i++] = byte;
            if (i == 3) {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;
                
                for (i = 0; i < 4; i++) {
                    result += chars[char_array_4[i]];
                }
                i = 0;
            }
        }
        
        if (i) {
            for (size_t j = i; j < 3; j++) {
                char_array_3[j] = '\0';
            }
            
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            
            for (size_t j = 0; j < i + 1; j++) {
                result += chars[char_array_4[j]];
            }
            
            while (i++ < 3) {
                result += '=';
            }
        }
        
        return result;
    }
    
    static std::vector<uint8_t> decode(const std::string& encoded) {
        const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<uint8_t> result;
        
        size_t in_len = encoded.length();
        size_t i = 0;
        size_t in = 0;
        uint8_t char_array_4[4], char_array_3[3];
        
        while (in_len-- && (encoded[in] != '=') && 
               (std::isalnum(encoded[in]) || (encoded[in] == '+') || (encoded[in] == '/'))) {
            char_array_4[i++] = encoded[in]; in++;
            if (i == 4) {
                for (i = 0; i < 4; i++) {
                    char_array_4[i] = chars.find(char_array_4[i]);
                }
                
                char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
                char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
                char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
                
                for (i = 0; (i < 3); i++) {
                    result.push_back(char_array_3[i]);
                }
                i = 0;
            }
        }
        
        if (i) {
            for (size_t j = i; j < 4; j++) {
                char_array_4[j] = 0;
            }
            
            for (size_t j = 0; j < 4; j++) {
                char_array_4[j] = chars.find(char_array_4[j]);
            }
            
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            
            for (size_t j = 0; (j < i - 1); j++) {
                result.push_back(char_array_3[j]);
            }
        }
        
        return result;
    }
};

// 简化的MQTT桥接器
class SimpleMQTTBridge {
public:
    struct PublishMessage {
        std::string topic;
        std::vector<uint8_t> payload;
        uint8_t qos;
        bool retain;
    };
    
    struct SubscribeMessage {
        std::string topic_filter;
        uint8_t qos;
    };
    
    int parse_json_publish(const std::string& json, PublishMessage& msg) {
        if (json.find("\"type\":\"publish\"") == std::string::npos) {
            return -1;
        }
        
        msg.topic = SimpleJSONParser::extract_string_value(json, "topic");
        if (msg.topic.empty()) {
            return -1;
        }
        
        std::string payload_b64 = SimpleJSONParser::extract_string_value(json, "payload");
        if (!payload_b64.empty()) {
            msg.payload = SimpleBase64::decode(payload_b64);
        }
        
        msg.qos = static_cast<uint8_t>(SimpleJSONParser::extract_int_value(json, "qos"));
        msg.retain = SimpleJSONParser::extract_bool_value(json, "retain");
        
        return 0;
    }
    
    int parse_json_subscribe(const std::string& json, SubscribeMessage& msg) {
        if (json.find("\"type\":\"subscribe\"") == std::string::npos) {
            return -1;
        }
        
        msg.topic_filter = SimpleJSONParser::extract_string_value(json, "topic_filter");
        if (msg.topic_filter.empty()) {
            return -1;
        }
        
        msg.qos = static_cast<uint8_t>(SimpleJSONParser::extract_int_value(json, "qos"));
        
        return 0;
    }
    
    std::string generate_json_publish(const PublishMessage& msg) {
        std::ostringstream json;
        json << "{";
        json << "\"type\":\"publish\",";
        json << "\"topic\":\"" << msg.topic << "\",";
        json << "\"payload\":\"" << SimpleBase64::encode(msg.payload) << "\",";
        json << "\"qos\":" << static_cast<int>(msg.qos) << ",";
        json << "\"retain\":" << (msg.retain ? "true" : "false");
        json << "}";
        
        return json.str();
    }
    
    std::string generate_json_response(const std::string& type, bool success, const std::string& message = "") {
        std::ostringstream json;
        json << "{";
        json << "\"type\":\"" << type << "\",";
        json << "\"success\":" << (success ? "true" : "false");
        if (!message.empty()) {
            json << ",\"message\":\"" << message << "\"";
        }
        json << "}";
        
        return json.str();
    }
    
    int parse_text_publish(const std::string& text, PublishMessage& msg) {
        std::istringstream iss(text);
        std::string command;
        std::string qos_str, retain_str;
        
        if (!(iss >> command >> msg.topic >> qos_str >> retain_str)) {
            return -1;
        }
        
        if (command != "PUB") {
            return -1;
        }
        
        msg.qos = static_cast<uint8_t>(std::stoi(qos_str));
        msg.retain = (std::stoi(retain_str) != 0);
        
        // Read the rest as payload
        std::string payload_str;
        std::getline(iss, payload_str);
        if (!payload_str.empty() && payload_str[0] == ' ') {
            payload_str = payload_str.substr(1); // Remove leading space
        }
        
        msg.payload = std::vector<uint8_t>(payload_str.begin(), payload_str.end());
        
        return 0;
    }
    
    int parse_text_subscribe(const std::string& text, SubscribeMessage& msg) {
        std::istringstream iss(text);
        std::string command;
        std::string qos_str;
        
        if (!(iss >> command >> msg.topic_filter >> qos_str)) {
            return -1;
        }
        
        if (command != "SUB") {
            return -1;
        }
        
        msg.qos = static_cast<uint8_t>(std::stoi(qos_str));
        
        return 0;
    }
};

// 测试函数
void test_json_publish_parsing() {
    std::cout << "Test: JSON publish message parsing..." << std::endl;
    
    std::string json_publish = "{\"type\":\"publish\",\"topic\":\"test/topic\",\"payload\":\"SGVsbG8gV29ybGQ=\",\"qos\":1,\"retain\":false}";
    
    SimpleMQTTBridge bridge;
    SimpleMQTTBridge::PublishMessage msg;
    
    int ret = bridge.parse_json_publish(json_publish, msg);
    
    assert(ret == 0);
    assert(msg.topic == "test/topic");
    assert(msg.qos == 1);
    assert(msg.retain == false);
    
    // payload应该是base64解码后的 "Hello World"
    std::string payload_str(msg.payload.begin(), msg.payload.end());
    assert(payload_str == "Hello World");
    
    std::cout << "✓ JSON publish message parsing test passed" << std::endl;
}

void test_json_subscribe_parsing() {
    std::cout << "Test: JSON subscribe message parsing..." << std::endl;
    
    std::string json_subscribe = "{\"type\":\"subscribe\",\"topic_filter\":\"test/+\",\"qos\":2}";
    
    SimpleMQTTBridge bridge;
    SimpleMQTTBridge::SubscribeMessage msg;
    
    int ret = bridge.parse_json_subscribe(json_subscribe, msg);
    
    assert(ret == 0);
    assert(msg.topic_filter == "test/+");
    assert(msg.qos == 2);
    
    std::cout << "✓ JSON subscribe message parsing test passed" << std::endl;
}

void test_json_message_generation() {
    std::cout << "Test: JSON message generation..." << std::endl;
    
    SimpleMQTTBridge bridge;
    SimpleMQTTBridge::PublishMessage msg;
    msg.topic = "test/topic";
    msg.payload = std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o'};
    msg.qos = 1;
    msg.retain = false;
    
    std::string json_message = bridge.generate_json_publish(msg);
    
    // 验证JSON消息包含必要字段
    assert(json_message.find("\"type\":\"publish\"") != std::string::npos);
    assert(json_message.find("\"topic\":\"test/topic\"") != std::string::npos);
    assert(json_message.find("\"qos\":1") != std::string::npos);
    assert(json_message.find("\"retain\":false") != std::string::npos);
    
    std::cout << "✓ JSON message generation test passed" << std::endl;
}

void test_text_protocol_parsing() {
    std::cout << "Test: Text protocol parsing..." << std::endl;
    
    std::string text_publish = "PUB test/topic 1 0 Hello World";
    
    SimpleMQTTBridge bridge;
    SimpleMQTTBridge::PublishMessage msg;
    
    int ret = bridge.parse_text_publish(text_publish, msg);
    
    assert(ret == 0);
    assert(msg.topic == "test/topic");
    assert(msg.qos == 1);
    assert(msg.retain == false);
    assert(msg.payload.size() == 11); // "Hello World" length
    
    std::string payload_str(msg.payload.begin(), msg.payload.end());
    assert(payload_str == "Hello World");
    
    std::cout << "✓ Text protocol parsing test passed" << std::endl;
}

void test_text_subscribe_parsing() {
    std::cout << "Test: Text subscribe parsing..." << std::endl;
    
    std::string text_subscribe = "SUB test/+ 2";
    
    SimpleMQTTBridge bridge;
    SimpleMQTTBridge::SubscribeMessage msg;
    
    int ret = bridge.parse_text_subscribe(text_subscribe, msg);
    
    assert(ret == 0);
    assert(msg.topic_filter == "test/+");
    assert(msg.qos == 2);
    
    std::cout << "✓ Text subscribe parsing test passed" << std::endl;
}

void test_base64_encoding() {
    std::cout << "Test: Base64 encoding..." << std::endl;
    
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    std::string encoded = SimpleBase64::encode(data);
    
    assert(encoded == "SGVsbG8=");
    
    std::vector<uint8_t> decoded = SimpleBase64::decode(encoded);
    
    assert(decoded.size() == 5);
    assert(decoded[0] == 'H');
    assert(decoded[4] == 'o');
    
    std::cout << "✓ Base64 encoding test passed" << std::endl;
}

void test_error_handling() {
    std::cout << "Test: Error handling..." << std::endl;
    
    SimpleMQTTBridge bridge;
    
    // Test invalid JSON
    std::string invalid_json = "{ invalid json }";
    SimpleMQTTBridge::PublishMessage msg;
    
    int ret = bridge.parse_json_publish(invalid_json, msg);
    assert(ret != 0); // Should return error
    
    // Test missing required fields
    std::string incomplete_json = "{\"type\":\"publish\",\"qos\":1}";
    ret = bridge.parse_json_publish(incomplete_json, msg);
    assert(ret != 0); // Should return error due to missing topic
    
    std::cout << "✓ Error handling test passed" << std::endl;
}

int main() {
    std::cout << "Running MQTT Bridge tests..." << std::endl;
    
    test_json_publish_parsing();
    test_json_subscribe_parsing();
    test_json_message_generation();
    test_text_protocol_parsing();
    test_text_subscribe_parsing();
    test_base64_encoding();
    test_error_handling();
    
    std::cout << "\n🎉 All MQTT Bridge tests passed!" << std::endl;
    return 0;
}