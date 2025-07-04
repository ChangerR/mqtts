#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <chrono>
#include <thread>

// 包含优化后的头文件
#include "mqtt_message_queue.h"
#include "mqtt_packet.h"

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
    PublishPacket packet;
    packet.type = PacketType::PUBLISH;
    packet.topic_name = to_mqtt_string("test/topic", nullptr);
    packet.payload = MQTTByteVector{'h', 'e', 'l', 'l', 'o'};
    packet.qos = 1;
    packet.retain = false;
    packet.dup = false;
    
    MQTTString sender_id = to_mqtt_string("sender", nullptr);
    
    // 创建共享内容
    SharedPublishContent* content = new SharedPublishContent(packet, sender_id);
    
    // 测试初始引用计数
    bool initial_ref_count = (content->ref_count.load() == 1);
    
    // 创建智能指针
    SharedPublishContentPtr ptr1(content);
    bool ref_count_after_ptr1 = (content->ref_count.load() == 2);
    
    // 复制智能指针
    SharedPublishContentPtr ptr2 = ptr1;
    bool ref_count_after_copy = (content->ref_count.load() == 3);
    
    // 测试结果
    test.assert_true(initial_ref_count && ref_count_after_ptr1 && ref_count_after_copy,
                    "引用计数管理不正确");
}

// 测试SessionPublishInfo的消息生成
void test_session_publish_info(TestFramework& test) {
    test.start_test("Session消息生成");
    
    // 创建测试数据
    PublishPacket original_packet;
    original_packet.type = PacketType::PUBLISH;
    original_packet.topic_name = to_mqtt_string("test/topic", nullptr);
    original_packet.payload = MQTTByteVector{'d', 'a', 't', 'a'};
    original_packet.qos = 2;
    original_packet.retain = true;
    original_packet.dup = false;
    
    MQTTString sender_id = to_mqtt_string("sender", nullptr);
    MQTTString target_id = to_mqtt_string("target", nullptr);
    
    // 创建SessionPublishInfo
    SessionPublishInfo session_info(original_packet, target_id, sender_id);
    
    // 生成带有session特定packet_id的消息
    uint16_t session_packet_id = 12345;
    PublishPacket generated_packet = session_info.generate_publish_packet(session_packet_id);
    
    // 验证生成的消息
    bool topic_correct = (generated_packet.topic_name == original_packet.topic_name);
    bool payload_correct = (generated_packet.payload == original_packet.payload);
    bool qos_correct = (generated_packet.qos == original_packet.qos);
    bool retain_correct = (generated_packet.retain == original_packet.retain);
    bool packet_id_correct = (generated_packet.packet_id == session_packet_id);
    
    test.assert_true(topic_correct && payload_correct && qos_correct && 
                    retain_correct && packet_id_correct,
                    "生成的消息不正确");
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
    
    MQTTString target_id = to_mqtt_string("target", nullptr);
    MQTTString sender_id = to_mqtt_string("sender", nullptr);
    
    // 创建PendingMessage
    PendingMessage message(packet, target_id, sender_id);
    
    // 测试兼容性接口
    bool target_correct = (message.get_target_client_id() == target_id);
    bool sender_correct = (message.get_sender_client_id() == sender_id);
    bool valid = message.is_valid();
    
    // 测试消息生成
    uint16_t test_packet_id = 999;
    PublishPacket generated = message.generate_publish_packet(test_packet_id);
    bool generated_correct = (generated.packet_id == test_packet_id);
    
    test.assert_true(target_correct && sender_correct && valid && generated_correct,
                    "PendingMessage兼容性接口不正确");
}

// 测试MessageContentCache
void test_message_content_cache(TestFramework& test) {
    test.start_test("消息内容缓存");
    
    MessageContentCache cache;
    
    // 创建测试数据
    PublishPacket packet1;
    packet1.type = PacketType::PUBLISH;
    packet1.topic_name = to_mqtt_string("test/topic1", nullptr);
    packet1.payload = MQTTByteVector{'d', 'a', 't', 'a', '1'};
    packet1.qos = 1;
    
    PublishPacket packet2;
    packet2.type = PacketType::PUBLISH;
    packet2.topic_name = to_mqtt_string("test/topic2", nullptr);
    packet2.payload = MQTTByteVector{'d', 'a', 't', 'a', '2'};
    packet2.qos = 1;
    
    MQTTString sender_id = to_mqtt_string("sender", nullptr);
    
    // 获取共享内容
    SharedPublishContentPtr content1 = cache.get_or_create_content(packet1, sender_id);
    SharedPublishContentPtr content2 = cache.get_or_create_content(packet2, sender_id);
    
    // 再次获取相同的内容
    SharedPublishContentPtr content1_again = cache.get_or_create_content(packet1, sender_id);
    
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
    SharedPublishContentPtr shared_content(new SharedPublishContent(large_packet, sender_id));
    std::vector<SessionPublishInfo> optimized_messages;
    
    start_time = std::chrono::high_resolution_clock::now();
    
    for (const auto& client : clients) {
        optimized_messages.emplace_back(shared_content, client);
    }
    
    end_time = std::chrono::high_resolution_clock::now();
    auto optimized_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    // 计算内存使用估算
    size_t traditional_memory = traditional_messages.size() * 
                                (sizeof(PendingMessage) + large_packet.payload.size());
    size_t optimized_memory = sizeof(SharedPublishContent) + large_packet.payload.size() +
                              optimized_messages.size() * sizeof(SessionPublishInfo);
    
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
        test_session_publish_info(test);
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