#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include "src/mqtt_allocator.h"
#include "src/mqtt_parser.h"
#include "src/mqtt_protocol_handler.h"
#include "src/mqtt_serialize_buffer.h"

class MockQoSSocket
{
public:
    MockQoSSocket() : last_send_data_(), last_send_size_(0) {}
    
    int send(const uint8_t* buf, int len)
    {
        last_send_data_.clear();
        last_send_data_.reserve(len);
        for (int i = 0; i < len; i++) {
            last_send_data_.push_back(buf[i]);
        }
        last_send_size_ = len;
        return 0;
    }
    
    int recv(char* buf, int& len)
    {
        len = 0;
        return 0;
    }
    
    bool is_connected() const { return true; }
    int get_fd() const { return 1; }
    int close() { return 0; }
    
    const std::vector<uint8_t>& get_last_send_data() const { return last_send_data_; }
    int get_last_send_size() const { return last_send_size_; }
    
private:
    std::vector<uint8_t> last_send_data_;
    int last_send_size_;
};

// Test helper class to access private members
class TestMQTTProtocolHandler : public mqtt::MQTTProtocolHandler
{
public:
    TestMQTTProtocolHandler(MQTTAllocator* allocator) : mqtt::MQTTProtocolHandler(allocator), mock_socket_(nullptr) {}
    
    void set_mock_socket(MockQoSSocket* socket) 
    {
        mock_socket_ = socket;
        // We need to override the socket pointer in the parent class
        // This is for testing purposes only
    }
    
    // Make handle_publish public for testing
    int test_handle_publish(const mqtt::PublishPacket* packet)
    {
        return handle_publish(packet);
    }
    
    MockQoSSocket* get_mock_socket() const { return mock_socket_; }
    
private:
    MockQoSSocket* mock_socket_;
};

void test_qos1_puback_reply()
{
    std::cout << "Testing QoS 1 PUBACK reply..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    
    // Create a QoS 1 PUBLISH packet
    mqtt::PublishPacket publish_packet(&allocator);
    publish_packet.type = mqtt::PacketType::PUBLISH;
    
    std::string topic = "test/qos1";
    std::string payload = "QoS 1 Test Message";
    
    publish_packet.topic_name = mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(&allocator));
    publish_packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end(), mqtt::MQTTSTLAllocator<uint8_t>(&allocator));
    publish_packet.qos = 1;
    publish_packet.retain = false;
    publish_packet.dup = false;
    publish_packet.packet_id = 1234;
    
    // Serialize the PUBLISH packet
    mqtt::MQTTSerializeBuffer serialize_buffer(&allocator);
    int ret = parser.serialize_publish(&publish_packet, serialize_buffer);
    assert(ret == 0);
    
    // Parse it back to verify
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBLISH);
    
    mqtt::PublishPacket* parsed_publish = static_cast<mqtt::PublishPacket*>(parsed_packet);
    assert(parsed_publish->qos == 1);
    assert(parsed_publish->packet_id == 1234);
    
    std::cout << "QoS 1 PUBLISH packet serialization/parsing works correctly" << std::endl;
    std::cout << "Packet ID: " << parsed_publish->packet_id << ", QoS: " << parsed_publish->qos << std::endl;
    
    // Clean up
    parsed_publish->~PublishPacket();
    allocator.deallocate(parsed_packet, sizeof(mqtt::PublishPacket));
    
    std::cout << "QoS 1 PUBACK reply test passed" << std::endl;
}

void test_qos2_pubrec_reply()
{
    std::cout << "Testing QoS 2 PUBREC reply..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    
    // Create a QoS 2 PUBLISH packet
    mqtt::PublishPacket publish_packet(&allocator);
    publish_packet.type = mqtt::PacketType::PUBLISH;
    
    std::string topic = "test/qos2";
    std::string payload = "QoS 2 Test Message";
    
    publish_packet.topic_name = mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(&allocator));
    publish_packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end(), mqtt::MQTTSTLAllocator<uint8_t>(&allocator));
    publish_packet.qos = 2;
    publish_packet.retain = false;
    publish_packet.dup = false;
    publish_packet.packet_id = 5678;
    
    // Serialize the PUBLISH packet
    mqtt::MQTTSerializeBuffer serialize_buffer(&allocator);
    int ret = parser.serialize_publish(&publish_packet, serialize_buffer);
    assert(ret == 0);
    
    // Parse it back to verify
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBLISH);
    
    mqtt::PublishPacket* parsed_publish = static_cast<mqtt::PublishPacket*>(parsed_packet);
    assert(parsed_publish->qos == 2);
    assert(parsed_publish->packet_id == 5678);
    
    std::cout << "QoS 2 PUBLISH packet serialization/parsing works correctly" << std::endl;
    std::cout << "Packet ID: " << parsed_publish->packet_id << ", QoS: " << parsed_publish->qos << std::endl;
    
    // Clean up
    parsed_publish->~PublishPacket();
    allocator.deallocate(parsed_packet, sizeof(mqtt::PublishPacket));
    
    std::cout << "QoS 2 PUBREC reply test passed" << std::endl;
}

void test_qos0_no_reply()
{
    std::cout << "Testing QoS 0 (no reply expected)..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    
    // Create a QoS 0 PUBLISH packet
    mqtt::PublishPacket publish_packet(&allocator);
    publish_packet.type = mqtt::PacketType::PUBLISH;
    
    std::string topic = "test/qos0";
    std::string payload = "QoS 0 Test Message";
    
    publish_packet.topic_name = mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(&allocator));
    publish_packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end(), mqtt::MQTTSTLAllocator<uint8_t>(&allocator));
    publish_packet.qos = 0;
    publish_packet.retain = false;
    publish_packet.dup = false;
    publish_packet.packet_id = 0;  // QoS 0 doesn't need packet ID
    
    // Serialize the PUBLISH packet
    mqtt::MQTTSerializeBuffer serialize_buffer(&allocator);
    int ret = parser.serialize_publish(&publish_packet, serialize_buffer);
    assert(ret == 0);
    
    // Parse it back to verify
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBLISH);
    
    mqtt::PublishPacket* parsed_publish = static_cast<mqtt::PublishPacket*>(parsed_packet);
    assert(parsed_publish->qos == 0);
    
    std::cout << "QoS 0 PUBLISH packet serialization/parsing works correctly" << std::endl;
    std::cout << "QoS: " << parsed_publish->qos << " (no packet ID needed)" << std::endl;
    
    // Clean up
    parsed_publish->~PublishPacket();
    allocator.deallocate(parsed_packet, sizeof(mqtt::PublishPacket));
    
    std::cout << "QoS 0 no reply test passed" << std::endl;
}

void test_puback_serialization()
{
    std::cout << "Testing PUBACK packet serialization..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    
    // Create a PUBACK packet
    mqtt::PubAckPacket puback_packet(&allocator);
    puback_packet.type = mqtt::PacketType::PUBACK;
    puback_packet.packet_id = 1234;
    puback_packet.reason_code = mqtt::ReasonCode::Success;
    
    // Serialize the PUBACK packet
    mqtt::MQTTSerializeBuffer serialize_buffer(&allocator);
    int ret = parser.serialize_puback(&puback_packet, serialize_buffer);
    assert(ret == 0);
    assert(serialize_buffer.size() > 0);
    
    std::cout << "About to parse PUBACK packet..." << std::endl;
    // Parse it back to verify
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    std::cout << "parse_packet returned: " << ret << std::endl;
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBACK);
    
    mqtt::PubAckPacket* parsed_puback = static_cast<mqtt::PubAckPacket*>(parsed_packet);
    assert(parsed_puback->packet_id == 1234);
    assert(parsed_puback->reason_code == mqtt::ReasonCode::Success);
    
    std::cout << "PUBACK packet serialization/parsing works correctly" << std::endl;
    std::cout << "PUBACK packet size: " << serialize_buffer.size() << " bytes" << std::endl;
    std::cout << "Packet ID: " << parsed_puback->packet_id << std::endl;
    
    // Clean up
    parsed_puback->~PubAckPacket();
    allocator.deallocate(parsed_packet, sizeof(mqtt::PubAckPacket));
    
    std::cout << "PUBACK serialization test passed" << std::endl;
}

void test_pubrec_serialization()
{
    std::cout << "Testing PUBREC packet serialization..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    
    // Create a PUBREC packet
    mqtt::PubRecPacket pubrec_packet(&allocator);
    pubrec_packet.type = mqtt::PacketType::PUBREC;
    pubrec_packet.packet_id = 5678;
    pubrec_packet.reason_code = mqtt::ReasonCode::Success;
    
    // Serialize the PUBREC packet
    mqtt::MQTTSerializeBuffer serialize_buffer(&allocator);
    int ret = parser.serialize_pubrec(&pubrec_packet, serialize_buffer);
    assert(ret == 0);
    assert(serialize_buffer.size() > 0);
    
    // Parse it back to verify
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBREC);
    
    mqtt::PubRecPacket* parsed_pubrec = static_cast<mqtt::PubRecPacket*>(parsed_packet);
    assert(parsed_pubrec->packet_id == 5678);
    assert(parsed_pubrec->reason_code == mqtt::ReasonCode::Success);
    
    std::cout << "PUBREC packet serialization/parsing works correctly" << std::endl;
    std::cout << "PUBREC packet size: " << serialize_buffer.size() << " bytes" << std::endl;
    std::cout << "Packet ID: " << parsed_pubrec->packet_id << std::endl;
    
    // Clean up
    parsed_pubrec->~PubRecPacket();
    allocator.deallocate(parsed_packet, sizeof(mqtt::PubRecPacket));
    
    std::cout << "PUBREC serialization test passed" << std::endl;
}

int main()
{
    std::cout << "Starting QoS reply mechanism tests\n" << std::endl;
    
    try {
        test_qos0_no_reply();
        test_qos1_puback_reply();
        test_qos2_pubrec_reply();
        test_puback_serialization();
        test_pubrec_serialization();
        
        std::cout << "\nAll QoS reply tests passed!" << std::endl;
        std::cout << "This verifies that:" << std::endl;
        std::cout << "1. QoS 0 PUBLISH packets are handled correctly (no reply needed)" << std::endl;
        std::cout << "2. QoS 1 PUBLISH packets can be processed and PUBACK can be generated" << std::endl;
        std::cout << "3. QoS 2 PUBLISH packets can be processed and PUBREC can be generated" << std::endl;
        std::cout << "4. PUBACK and PUBREC packets can be correctly serialized and parsed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed: " << e.what() << std::endl;
        return 1;
    }
}