#include "mqtt_session_manager_v2.h"
#include "mqtt_packet.h"
#include "logger.h"
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

using namespace mqtt;

/**
 * @brief 演示优化后的MQTT会话管理器的使用方法
 * 
 * 主要改进：
 * 1. 内存共享：多个session共享同一个消息内容
 * 2. 延迟packet_id生成：每个session根据自己的状态生成packet_id
 * 3. 批量转发：一次性向多个客户端转发消息
 * 4. 缓存管理：自动清理过期的消息内容
 */

// 模拟的MQTT协议处理器
class MockMQTTProtocolHandler {
public:
    bool is_connected() const { return connected_; }
    void set_connected(bool connected) { connected_ = connected; }
    
    // 模拟订阅列表
    const std::vector<MQTTString>& get_subscriptions() const { return subscriptions_; }
    void add_subscription(const MQTTString& topic) { subscriptions_.push_back(topic); }
    
private:
    bool connected_ = true;
    std::vector<MQTTString> subscriptions_;
};

void demonstrate_optimized_message_forwarding()
{
    std::cout << "\n=== MQTT会话管理器优化演示 ===\n" << std::endl;
    
    // 获取全局会话管理器实例
    GlobalSessionManager& global_manager = GlobalSessionManagerInstance::get_instance();
    
    // 1. 初始化阶段 - 预分配资源
    std::cout << "1. 初始化阶段 - 预分配资源" << std::endl;
    global_manager.pre_register_threads(4, 10000);  // 4个线程，10000个客户端
    
    // 2. 注册线程管理器
    std::cout << "2. 注册线程管理器" << std::endl;
    ThreadLocalSessionManager* thread_manager = global_manager.register_thread_manager(std::this_thread::get_id());
    global_manager.finalize_thread_registration();
    
    // 3. 创建模拟的客户端
    std::cout << "3. 创建模拟的客户端" << std::endl;
    std::vector<std::unique_ptr<MockMQTTProtocolHandler>> clients;
    std::vector<MQTTString> client_ids;
    
    for (int i = 0; i < 5; i++) {
        auto client = std::make_unique<MockMQTTProtocolHandler>();
        MQTTString client_id = to_mqtt_string("client_" + std::to_string(i), nullptr);
        
        // 添加订阅
        client->add_subscription(to_mqtt_string("sensor/temperature", nullptr));
        client->add_subscription(to_mqtt_string("sensor/humidity", nullptr));
        
        // 注册到会话管理器
        global_manager.register_session(client_id, reinterpret_cast<MQTTProtocolHandler*>(client.get()));
        
        clients.push_back(std::move(client));
        client_ids.push_back(client_id);
    }
    
    // 4. 创建测试消息
    std::cout << "\n4. 创建测试消息" << std::endl;
    PublishPacket original_packet;
    original_packet.type = PacketType::PUBLISH;
    original_packet.topic_name = to_mqtt_string("sensor/temperature", nullptr);
    original_packet.payload = MQTTByteVector{'2', '5', '.', '5', 'C'};
    original_packet.qos = 1;
    original_packet.retain = false;
    original_packet.dup = false;
    
    MQTTString sender_id = to_mqtt_string("sensor_device_001", nullptr);
    
    std::cout << "原始消息：" << std::endl;
    std::cout << "  - 主题: " << from_mqtt_string(original_packet.topic_name) << std::endl;
    std::cout << "  - 负载大小: " << original_packet.payload.size() << " bytes" << std::endl;
    std::cout << "  - QoS: " << static_cast<int>(original_packet.qos) << std::endl;
    
    // 5. 演示传统方式 vs 优化方式
    std::cout << "\n5. 消息转发方式对比" << std::endl;
    
    // 传统方式 - 每个客户端都复制完整消息
    std::cout << "\n传统方式 - 每个客户端都复制完整消息：" << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();
    
    for (const MQTTString& client_id : client_ids) {
        global_manager.forward_publish(client_id, original_packet, sender_id);
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    std::cout << "  - 转发时间: " << duration.count() << " 微秒" << std::endl;
    std::cout << "  - 内存使用: " << client_ids.size() << " 个完整副本" << std::endl;
    
    // 优化方式 - 使用共享内容
    std::cout << "\n优化方式 - 使用共享内容：" << std::endl;
    start_time = std::chrono::high_resolution_clock::now();
    
    // 创建或获取共享内容
    SharedPublishContentPtr shared_content = global_manager.get_or_create_shared_content(original_packet, sender_id);
    
    // 批量转发
    std::vector<MQTTString> target_clients = client_ids;
    int forwarded_count = global_manager.batch_forward_publish(target_clients, shared_content);
    
    end_time = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    std::cout << "  - 转发时间: " << duration.count() << " 微秒" << std::endl;
    std::cout << "  - 内存使用: 1 个共享副本 + " << client_ids.size() << " 个轻量级引用" << std::endl;
    std::cout << "  - 成功转发: " << forwarded_count << " 个客户端" << std::endl;
    
    // 6. 演示基于主题的优化转发
    std::cout << "\n6. 基于主题的优化转发：" << std::endl;
    
    MQTTString topic = to_mqtt_string("sensor/temperature", nullptr);
    int topic_forwarded = global_manager.forward_publish_by_topic_shared(topic, shared_content);
    
    std::cout << "  - 主题: " << from_mqtt_string(topic) << std::endl;
    std::cout << "  - 订阅者数量: " << topic_forwarded << std::endl;
    
    // 7. 演示packet_id的自动生成
    std::cout << "\n7. Session特定的packet_id生成：" << std::endl;
    
    for (size_t i = 0; i < 3; i++) {
        uint16_t packet_id = thread_manager->generate_packet_id(client_ids[i]);
        std::cout << "  - 客户端 " << from_mqtt_string(client_ids[i]) 
                  << " 的packet_id: " << packet_id << std::endl;
    }
    
    // 8. 缓存管理
    std::cout << "\n8. 消息缓存管理：" << std::endl;
    size_t cache_size = global_manager.get_message_cache_size();
    std::cout << "  - 当前缓存大小: " << cache_size << " 个消息内容" << std::endl;
    
    // 清理过期缓存
    global_manager.cleanup_message_cache(300);  // 清理5分钟以前的缓存
    
    // 9. 性能统计
    std::cout << "\n9. 性能统计：" << std::endl;
    std::cout << "  - 总会话数: " << global_manager.get_total_session_count() << std::endl;
    std::cout << "  - 待发送消息数: " << thread_manager->get_pending_message_count() << std::endl;
    
    // 10. 清理资源
    std::cout << "\n10. 清理资源" << std::endl;
    for (const MQTTString& client_id : client_ids) {
        global_manager.unregister_session(client_id);
    }
    
    std::cout << "\n=== 演示完成 ===\n" << std::endl;
}

void demonstrate_memory_sharing_benefits()
{
    std::cout << "\n=== 内存共享优化效果演示 ===\n" << std::endl;
    
    // 创建大负载消息
    PublishPacket large_packet;
    large_packet.type = PacketType::PUBLISH;
    large_packet.topic_name = to_mqtt_string("data/large_payload", nullptr);
    large_packet.qos = 1;
    large_packet.retain = false;
    
    // 创建10KB的负载
    large_packet.payload.resize(10240);
    std::fill(large_packet.payload.begin(), large_packet.payload.end(), 'A');
    
    MQTTString sender_id = to_mqtt_string("data_producer", nullptr);
    
    std::cout << "大负载消息：" << std::endl;
    std::cout << "  - 负载大小: " << large_packet.payload.size() << " bytes" << std::endl;
    
    // 模拟1000个客户端
    int client_count = 1000;
    
    // 传统方式的内存估算
    size_t traditional_memory = client_count * (large_packet.payload.size() + 
                                               large_packet.topic_name.size() + 
                                               sizeof(PublishPacket));
    
    // 优化方式的内存估算
    size_t optimized_memory = large_packet.payload.size() + 
                              large_packet.topic_name.size() + 
                              sizeof(SharedPublishContent) + 
                              client_count * sizeof(SessionPublishInfo);
    
    std::cout << "\n内存使用对比（" << client_count << " 个客户端）：" << std::endl;
    std::cout << "  - 传统方式: " << traditional_memory / 1024 / 1024 << " MB" << std::endl;
    std::cout << "  - 优化方式: " << optimized_memory / 1024 / 1024 << " MB" << std::endl;
    std::cout << "  - 节省内存: " << (traditional_memory - optimized_memory) / 1024 / 1024 << " MB" << std::endl;
    std::cout << "  - 节省比例: " << ((traditional_memory - optimized_memory) * 100 / traditional_memory) << "%" << std::endl;
    
    std::cout << "\n=== 内存优化演示完成 ===\n" << std::endl;
}

int main()
{
    std::cout << "MQTT会话管理器优化演示程序" << std::endl;
    std::cout << "============================" << std::endl;
    
    try {
        // 演示优化后的消息转发
        demonstrate_optimized_message_forwarding();
        
        // 演示内存共享的优势
        demonstrate_memory_sharing_benefits();
        
        std::cout << "\n所有演示完成！" << std::endl;
        std::cout << "\n主要优化点总结：" << std::endl;
        std::cout << "1. 内存共享：避免消息内容的重复存储" << std::endl;
        std::cout << "2. 延迟packet_id生成：每个session根据自己的状态生成" << std::endl;
        std::cout << "3. 批量转发：提高转发效率" << std::endl;
        std::cout << "4. 缓存管理：自动清理过期内容" << std::endl;
        std::cout << "5. 引用计数：自动内存管理" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "演示过程中发生错误: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}