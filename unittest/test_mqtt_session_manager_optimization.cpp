#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <chrono>
#include <thread>

// 包含优化后的头文件
#include "../src/mqtt_message_queue.h"
#include "../src/mqtt_packet.h"

using namespace mqtt;

// 简单的测试框架
class TestFramework {
private:
    int passed_ = 0;
    int failed_ = 0;
    std::string current_test_;

public:
    void start_test(const std::string& test_name) {
        current_test_ = test_name;
        std::cout << "测试: " << test_name << " ... ";
    }

    void assert_true(bool condition, const std::string& message = "") {
        if (condition) {
            std::cout << "通过" << std::endl;
            passed_++;
        } else {
            std::cout << "失败: " << message << std::endl;
            failed_++;
        }
    }

    void print_summary() {
        std::cout << "\n=== 测试总结 ===" << std::endl;
        std::cout << "通过: " << passed_ << std::endl;
        std::cout << "失败: " << failed_ << std::endl;
        std::cout << "成功率: " << (passed_ * 100 / (passed_ + failed_)) << "%" << std::endl;
    }

    bool all_passed() const {
        return failed_ == 0;
    }
};

// 测试共享内容的引用计数
void test_shared_content_ref_count(TestFramework& test) {
    test.start_test("共享内容引用计数");
    
    // 创建测试数据
    MQTTString topic = to_mqtt_string("test/topic", nullptr);
    MQTTByteVector payload{'h', 'e', 'l', 'l', 'o'};
    uint8_t qos = 1;
    bool retain = false;
    bool dup = false;
    Properties properties;
    MQTTString sender_id = to_mqtt_string("sender", nullptr);
    
    // 创建共享内容
    SharedMessageContent* content = new SharedMessageContent(topic, payload, qos, retain, dup, properties, sender_id);
    
    // 测试初始引用计数
    bool initial_ref_count = (content->ref_count.load() == 1);
    
    // 创建智能指针
    SharedMessageContentPtr ptr1(content);
    bool ref_count_after_ptr1 = (content->ref_count.load() == 2);
    
    // 复制智能指针
    SharedMessageContentPtr ptr2 = ptr1;
    bool ref_count_after_copy = (content->ref_count.load() == 3);
    
    // 测试结果
    test.assert_true(initial_ref_count && ref_count_after_ptr1 && ref_count_after_copy,
                    "引用计数管理不正确");
}

// 测试PendingMessageInfo的消息信息访问
void test_pending_message_info(TestFramework& test) {
    test.start_test("PendingMessageInfo消息信息");
    
    // 创建测试数据
    MQTTString topic = to_mqtt_string("test/topic", nullptr);
    MQTTByteVector payload{'d', 'a', 't', 'a'};
    uint8_t qos = 2;
    bool retain = true;
    bool dup = false;
    Properties properties;
    MQTTString sender_id = to_mqtt_string("sender", nullptr);
    MQTTString target_id = to_mqtt_string("target", nullptr);
    
    // 创建PendingMessageInfo
    PendingMessageInfo message_info(topic, payload, qos, retain, dup, properties, sender_id, target_id);
    
    // 验证消息信息访问
    bool topic_correct = (message_info.get_topic() == topic);
    bool payload_correct = (message_info.get_payload() == payload);
    bool qos_correct = (message_info.get_qos() == qos);
    bool retain_correct = (message_info.is_retain() == retain);
    bool dup_correct = (message_info.is_dup() == dup);
    bool sender_correct = (message_info.get_sender_client_id() == sender_id);
    bool target_correct = (message_info.get_target_client_id() == target_id);
    
    test.assert_true(topic_correct && payload_correct && qos_correct && 
                    retain_correct && dup_correct && sender_correct && target_correct,
                    "消息信息访问不正确");
}

// 测试PendingMessage的兼容性接口
void test_pending_message_compatibility(TestFramework& test) {
    test.start_test("PendingMessage兼容性");
    
    // 创建测试数据
    PublishPacket packet;
    packet.type = PacketType::PUBLISH;
    packet.topic_name = to_mqtt_string("test/topic", nullptr);
    packet.payload = MQTTByteVector{'t', 'e', 's', 't'};
    packet.qos = 1;
    packet.retain = false;
    packet.dup = false;
    
    MQTTString target_id = to_mqtt_string("target", nullptr);
    MQTTString sender_id = to_mqtt_string("sender", nullptr);
    
    // 创建PendingMessage（使用兼容性构造函数）
    PendingMessage message(packet, target_id, sender_id);
    
    // 测试兼容性接口
    bool target_correct = (message.get_target_client_id() == target_id);
    bool sender_correct = (message.get_sender_client_id() == sender_id);
    bool valid = message.is_valid();
    bool topic_correct = (message.get_topic() == packet.topic_name);
    bool payload_correct = (message.get_payload() == packet.payload);
    bool qos_correct = (message.get_qos() == packet.qos);
    
    test.assert_true(target_correct && sender_correct && valid && 
                    topic_correct && payload_correct && qos_correct,
                    "PendingMessage兼容性接口不正确");
}

// 测试MessageContentCache
void test_message_content_cache(TestFramework& test) {
    test.start_test("消息内容缓存");
    
    MessageContentCache cache;
    
    // 创建测试数据
    MQTTString topic1 = to_mqtt_string("test/topic1", nullptr);
    MQTTByteVector payload1{'d', 'a', 't', 'a', '1'};
    MQTTString topic2 = to_mqtt_string("test/topic2", nullptr);
    MQTTByteVector payload2{'d', 'a', 't', 'a', '2'};
    uint8_t qos = 1;
    bool retain = false;
    bool dup = false;
    Properties properties;
    MQTTString sender_id = to_mqtt_string("sender", nullptr);
    
    // 获取共享内容
    SharedMessageContentPtr content1 = cache.get_or_create_content(topic1, payload1, qos, retain, dup, properties, sender_id);
    SharedMessageContentPtr content2 = cache.get_or_create_content(topic2, payload2, qos, retain, dup, properties, sender_id);
    
    // 再次获取相同的内容
    SharedMessageContentPtr content1_again = cache.get_or_create_content(topic1, payload1, qos, retain, dup, properties, sender_id);
    
    // 验证缓存行为
    bool cache_working = (content1.get() == content1_again.get());
    bool different_content = (content1.get() != content2.get());
    bool cache_size_correct = (cache.get_cache_size() == 2);
    
    test.assert_true(cache_working && different_content && cache_size_correct,
                    "消息内容缓存不正确");
}

// 性能测试
void test_performance_comparison(TestFramework& test) {
    test.start_test("性能对比");
    
    // 创建大负载消息
    PublishPacket large_packet;
    large_packet.type = PacketType::PUBLISH;
    large_packet.topic_name = to_mqtt_string("performance/test", nullptr);
    large_packet.payload.resize(1024 * 10);  // 10KB负载
    std::fill(large_packet.payload.begin(), large_packet.payload.end(), 'A');
    large_packet.qos = 1;
    
    MQTTString sender_id = to_mqtt_string("perf_sender", nullptr);
    
    const int client_count = 1000;
    std::vector<MQTTString> clients;
    for (int i = 0; i < client_count; i++) {
        clients.push_back(to_mqtt_string("client_" + std::to_string(i), nullptr));
    }
    
    // 测试传统方式的内存使用
    std::vector<PendingMessage> traditional_messages;
    auto start_time = std::chrono::high_resolution_clock::now();
    
    for (const auto& client : clients) {
        traditional_messages.emplace_back(large_packet, client, sender_id);
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto traditional_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    // 测试优化方式的内存使用
    SharedMessageContentPtr shared_content(new SharedMessageContent(large_packet.topic_name, large_packet.payload, 
                                                                     large_packet.qos, large_packet.retain, 
                                                                     large_packet.dup, large_packet.properties, sender_id));
    std::vector<PendingMessageInfo> optimized_messages;
    
    start_time = std::chrono::high_resolution_clock::now();
    
    for (const auto& client : clients) {
        optimized_messages.emplace_back(shared_content, client);
    }
    
    end_time = std::chrono::high_resolution_clock::now();
    auto optimized_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    // 计算内存使用估算
    size_t traditional_memory = traditional_messages.size() * 
                                (sizeof(PendingMessage) + large_packet.payload.size());
    size_t optimized_memory = sizeof(SharedMessageContent) + large_packet.payload.size() +
                              optimized_messages.size() * sizeof(PendingMessageInfo);
    
    double memory_savings = (double)(traditional_memory - optimized_memory) / traditional_memory * 100;
    
    std::cout << std::endl;
    std::cout << "  传统方式时间: " << traditional_time.count() << " 微秒" << std::endl;
    std::cout << "  优化方式时间: " << optimized_time.count() << " 微秒" << std::endl;
    std::cout << "  传统方式内存: " << traditional_memory / 1024 / 1024 << " MB" << std::endl;
    std::cout << "  优化方式内存: " << optimized_memory / 1024 / 1024 << " MB" << std::endl;
    std::cout << "  内存节省: " << memory_savings << "%" << std::endl;
    
    test.assert_true(memory_savings > 50.0, "内存节省不足50%");
}

int main() {
    std::cout << "MQTT会话管理器优化测试" << std::endl;
    std::cout << "======================" << std::endl;
    
    TestFramework test;
    
    try {
        // 运行各项测试
        test_shared_content_ref_count(test);
        test_pending_message_info(test);
        test_pending_message_compatibility(test);
        test_message_content_cache(test);
        test_performance_comparison(test);
        
        // 打印测试总结
        test.print_summary();
        
        if (test.all_passed()) {
            std::cout << "\n所有测试通过！MQTT会话管理器优化实现正确。" << std::endl;
            return 0;
        } else {
            std::cout << "\n部分测试失败，请检查实现。" << std::endl;
            return 1;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "测试过程中发生异常: " << e.what() << std::endl;
        return 1;
    }
}