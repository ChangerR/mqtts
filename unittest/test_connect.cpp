#include <iostream>
#include <vector>
#include <iomanip>
#include "src/mqtt_parser.h"
#include "src/mqtt_allocator.h"

void print_hex(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0) {
            std::cout << std::dec << std::setfill('0') << std::setw(6) << i << ": ";
        }
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)data[i] << " ";
        if ((i + 1) % 16 == 0) {
            std::cout << std::endl;
        }
    }
    if (len % 16 != 0) {
        std::cout << std::endl;
    }
}

int main() {
    // CONNECT包的十六进制数据
    uint8_t connect_data[] = {
        0x10, 0x2c, 0x00, 0x04, 0x4d, 0x51, 0x54, 0x54, 0x05, 0xc2, 0x00, 0x3c, 0x05, 0x11, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x0e, 0x6d, 0x71, 0x74, 0x74, 0x78, 0x5f, 0x38, 0x30, 0x36, 0x39, 0x37, 0x31,
        0x30, 0x65, 0x00, 0x04, 0x74, 0x65, 0x73, 0x74, 0x00, 0x04, 0x74, 0x65, 0x73, 0x74
    };
    
    size_t data_len = sizeof(connect_data);
    
    std::cout << "测试CONNECT包解析" << std::endl;
    std::cout << "数据长度: " << data_len << " 字节" << std::endl;
    std::cout << "十六进制数据:" << std::endl;
    print_hex(connect_data, data_len);
    
    // 创建allocator和解析器
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    
    // 解析包
    mqtt::Packet* packet = nullptr;
    int ret = parser.parse_packet(connect_data, data_len, &packet);
    
    if (ret == 0 && packet != nullptr) {
        std::cout << "\n解析成功!" << std::endl;
        
        if (packet->type == mqtt::PacketType::CONNECT) {
            mqtt::ConnectPacket* connect = static_cast<mqtt::ConnectPacket*>(packet);
            
            std::cout << "协议名称: " << connect->protocol_name << std::endl;
            std::cout << "协议版本: " << (int)connect->protocol_version << std::endl;
            std::cout << "Keep Alive: " << connect->keep_alive << " 秒" << std::endl;
            std::cout << "客户端ID: " << connect->client_id << std::endl;
            
            std::cout << "连接标志:" << std::endl;
            std::cout << "  Clean Start: " << (connect->flags.clean_start ? "是" : "否") << std::endl;
            std::cout << "  用户名标志: " << (connect->flags.username_flag ? "是" : "否") << std::endl;
            std::cout << "  密码标志: " << (connect->flags.password_flag ? "是" : "否") << std::endl;
            
            if (connect->flags.username_flag) {
                std::cout << "用户名: " << connect->username << std::endl;
            }
            if (connect->flags.password_flag) {
                std::cout << "密码: " << connect->password << std::endl;
            }
            
            int prop_count = 0;
            if (connect->properties.session_expiry_interval != 0) ++prop_count;
            if (connect->properties.receive_maximum != 0) ++prop_count;
            if (connect->properties.maximum_packet_size != 0) ++prop_count;
            if (connect->properties.topic_alias_maximum != 0) ++prop_count;
            if (connect->properties.topic_alias != 0) ++prop_count;
            if (connect->properties.request_response_information) ++prop_count;
            if (connect->properties.request_problem_information) ++prop_count;
            if (!connect->properties.user_properties.empty()) ++prop_count;
            if (!connect->properties.authentication_method.empty()) ++prop_count;
            if (!connect->properties.authentication_data.empty()) ++prop_count;
            if (!connect->properties.assigned_client_identifier.empty()) ++prop_count;
            if (connect->properties.server_keep_alive != 0) ++prop_count;
            if (!connect->properties.response_information.empty()) ++prop_count;
            if (!connect->properties.server_reference.empty()) ++prop_count;
            if (!connect->properties.reason_string.empty()) ++prop_count;
            if (connect->properties.payload_format_indicator != 0) ++prop_count;
            if (connect->properties.message_expiry_interval != 0) ++prop_count;
            if (!connect->properties.content_type.empty()) ++prop_count;
            if (!connect->properties.response_topic.empty()) ++prop_count;
            if (!connect->properties.correlation_data.empty()) ++prop_count;
            if (connect->properties.subscription_identifier != 0) ++prop_count;
            if (connect->properties.will_delay_interval != 0) ++prop_count;
            if (connect->properties.maximum_qos != 0) ++prop_count;
            if (!connect->properties.retain_available) ++prop_count;
            if (!connect->properties.wildcard_subscription_available) ++prop_count;
            if (!connect->properties.subscription_identifier_available) ++prop_count;
            if (!connect->properties.shared_subscription_available) ++prop_count;
            std::cout << "属性数量: " << prop_count << std::endl;

            // 详细输出每个属性
            if (connect->properties.session_expiry_interval != 0)
                std::cout << "session_expiry_interval: " << connect->properties.session_expiry_interval << std::endl;
            if (connect->properties.receive_maximum != 0)
                std::cout << "receive_maximum: " << connect->properties.receive_maximum << std::endl;
            if (connect->properties.maximum_packet_size != 0)
                std::cout << "maximum_packet_size: " << connect->properties.maximum_packet_size << std::endl;
            if (connect->properties.topic_alias_maximum != 0)
                std::cout << "topic_alias_maximum: " << connect->properties.topic_alias_maximum << std::endl;
            if (connect->properties.topic_alias != 0)
                std::cout << "topic_alias: " << connect->properties.topic_alias << std::endl;
            if (connect->properties.request_response_information)
                std::cout << "request_response_information: true" << std::endl;
            if (connect->properties.request_problem_information)
                std::cout << "request_problem_information: true" << std::endl;
            if (!connect->properties.user_properties.empty()) {
                std::cout << "user_properties: ";
                for (size_t i = 0; i < connect->properties.user_properties.size(); ++i) {
                    std::cout << "[" << connect->properties.user_properties[i].first << ": " << connect->properties.user_properties[i].second << "] ";
                }
                std::cout << std::endl;
            }
            if (!connect->properties.authentication_method.empty())
                std::cout << "authentication_method: " << connect->properties.authentication_method << std::endl;
            if (!connect->properties.authentication_data.empty())
                std::cout << "authentication_data: (size=" << connect->properties.authentication_data.size() << ")" << std::endl;
            if (!connect->properties.assigned_client_identifier.empty())
                std::cout << "assigned_client_identifier: " << connect->properties.assigned_client_identifier << std::endl;
            if (connect->properties.server_keep_alive != 0)
                std::cout << "server_keep_alive: " << connect->properties.server_keep_alive << std::endl;
            if (!connect->properties.response_information.empty())
                std::cout << "response_information: " << connect->properties.response_information << std::endl;
            if (!connect->properties.server_reference.empty())
                std::cout << "server_reference: " << connect->properties.server_reference << std::endl;
            if (!connect->properties.reason_string.empty())
                std::cout << "reason_string: " << connect->properties.reason_string << std::endl;
            if (connect->properties.payload_format_indicator != 0)
                std::cout << "payload_format_indicator: " << (int)connect->properties.payload_format_indicator << std::endl;
            if (connect->properties.message_expiry_interval != 0)
                std::cout << "message_expiry_interval: " << connect->properties.message_expiry_interval << std::endl;
            if (!connect->properties.content_type.empty())
                std::cout << "content_type: " << connect->properties.content_type << std::endl;
            if (!connect->properties.response_topic.empty())
                std::cout << "response_topic: " << connect->properties.response_topic << std::endl;
            if (!connect->properties.correlation_data.empty())
                std::cout << "correlation_data: (size=" << connect->properties.correlation_data.size() << ")" << std::endl;
            if (connect->properties.subscription_identifier != 0)
                std::cout << "subscription_identifier: " << connect->properties.subscription_identifier << std::endl;
            if (connect->properties.will_delay_interval != 0)
                std::cout << "will_delay_interval: " << connect->properties.will_delay_interval << std::endl;
            if (connect->properties.maximum_qos != 0)
                std::cout << "maximum_qos: " << (int)connect->properties.maximum_qos << std::endl;
            if (!connect->properties.retain_available)
                std::cout << "retain_available: false" << std::endl;
            if (!connect->properties.wildcard_subscription_available)
                std::cout << "wildcard_subscription_available: false" << std::endl;
            if (!connect->properties.subscription_identifier_available)
                std::cout << "subscription_identifier_available: false" << std::endl;
            if (!connect->properties.shared_subscription_available)
                std::cout << "shared_subscription_available: false" << std::endl;
        }
    } else {
        std::cout << "\n解析失败! 错误码: " << ret << std::endl;
    }
    
    return 0;
} 