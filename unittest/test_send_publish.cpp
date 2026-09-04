#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>
#include "mqtt_allocator.h"
#include "mqtt_parser.h"
#include "mqtt_protocol_handler.h"
#include "mqtt_buffer.h"
#include "mqtt_socket.h"
#include "mqtt_define.h"
#include "mqtt_stl_allocator.h"

void test_publish_serialization_qos0()
{
    std::cout << "Testing PUBLISH serialization with QoS 0..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    mqtt::MQTTBuffer serialize_buffer(&allocator);
    
    mqtt::PublishPacket publish_packet(&allocator);
    publish_packet.type = mqtt::PacketType::PUBLISH;
    
    std::string topic = "test/topic";
    std::string payload = "Hello World";
    
    publish_packet.topic_name = mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(&allocator));
    publish_packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end(), mqtt::MQTTSTLAllocator<uint8_t>(&allocator));
    publish_packet.qos = 0;
    publish_packet.retain = false;
    publish_packet.dup = false;
    publish_packet.packet_id = 0;
    
    int ret = parser.serialize_publish(&publish_packet, serialize_buffer);
    assert(ret == 0);
    assert(serialize_buffer.size() > 0);
    
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBLISH);
    
    mqtt::PublishPacket* parsed_publish = static_cast<mqtt::PublishPacket*>(parsed_packet);
    assert(parsed_publish->qos == 0);
    assert(parsed_publish->packet_id == 0);
    assert(parsed_publish->retain == false);
    assert(parsed_publish->dup == false);
    
    std::string parsed_topic(parsed_publish->topic_name.begin(), parsed_publish->topic_name.end());
    assert(parsed_topic == topic);
    
    std::string parsed_payload(parsed_publish->payload.begin(), parsed_publish->payload.end());
    assert(parsed_payload == payload);
    
    std::cout << "QoS 0 serialization test passed - packet size: " << serialize_buffer.size() << " bytes" << std::endl;
}

void test_publish_serialization_qos1()
{
    std::cout << "Testing PUBLISH serialization with QoS 1..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    mqtt::MQTTBuffer serialize_buffer(&allocator);
    
    mqtt::PublishPacket publish_packet(&allocator);
    publish_packet.type = mqtt::PacketType::PUBLISH;
    
    std::string topic = "test/qos1";
    std::string payload = "QoS 1 Message";
    
    publish_packet.topic_name = mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(&allocator));
    publish_packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end(), mqtt::MQTTSTLAllocator<uint8_t>(&allocator));
    publish_packet.qos = 1;
    publish_packet.retain = false;
    publish_packet.dup = false;
    publish_packet.packet_id = 1234;
    
    int ret = parser.serialize_publish(&publish_packet, serialize_buffer);
    assert(ret == 0);
    assert(serialize_buffer.size() > 0);
    
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBLISH);
    
    mqtt::PublishPacket* parsed_publish = static_cast<mqtt::PublishPacket*>(parsed_packet);
    assert(parsed_publish->qos == 1);
    assert(parsed_publish->packet_id == 1234);
    assert(parsed_publish->retain == false);
    assert(parsed_publish->dup == false);
    
    std::string parsed_topic(parsed_publish->topic_name.begin(), parsed_publish->topic_name.end());
    assert(parsed_topic == topic);
    
    std::string parsed_payload(parsed_publish->payload.begin(), parsed_publish->payload.end());
    assert(parsed_payload == payload);
    
    std::cout << "QoS 1 serialization test passed - packet size: " << serialize_buffer.size() << " bytes" << std::endl;
}

void test_publish_serialization_qos2()
{
    std::cout << "Testing PUBLISH serialization with QoS 2..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    mqtt::MQTTBuffer serialize_buffer(&allocator);
    
    mqtt::PublishPacket publish_packet(&allocator);
    publish_packet.type = mqtt::PacketType::PUBLISH;
    
    std::string topic = "test/qos2";
    std::string payload = "QoS 2 Message";
    
    publish_packet.topic_name = mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(&allocator));
    publish_packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end(), mqtt::MQTTSTLAllocator<uint8_t>(&allocator));
    publish_packet.qos = 2;
    publish_packet.retain = false;
    publish_packet.dup = false;
    publish_packet.packet_id = 5678;
    
    int ret = parser.serialize_publish(&publish_packet, serialize_buffer);
    assert(ret == 0);
    assert(serialize_buffer.size() > 0);
    
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBLISH);
    
    mqtt::PublishPacket* parsed_publish = static_cast<mqtt::PublishPacket*>(parsed_packet);
    assert(parsed_publish->qos == 2);
    assert(parsed_publish->packet_id == 5678);
    assert(parsed_publish->retain == false);
    assert(parsed_publish->dup == false);
    
    std::string parsed_topic(parsed_publish->topic_name.begin(), parsed_publish->topic_name.end());
    assert(parsed_topic == topic);
    
    std::string parsed_payload(parsed_publish->payload.begin(), parsed_publish->payload.end());
    assert(parsed_payload == payload);
    
    std::cout << "QoS 2 serialization test passed - packet size: " << serialize_buffer.size() << " bytes" << std::endl;
}

void test_publish_serialization_with_retain()
{
    std::cout << "Testing PUBLISH serialization with retain flag..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    mqtt::MQTTBuffer serialize_buffer(&allocator);
    
    mqtt::PublishPacket publish_packet(&allocator);
    publish_packet.type = mqtt::PacketType::PUBLISH;
    
    std::string topic = "test/retain";
    std::string payload = "Retained Message";
    
    publish_packet.topic_name = mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(&allocator));
    publish_packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end(), mqtt::MQTTSTLAllocator<uint8_t>(&allocator));
    publish_packet.qos = 0;
    publish_packet.retain = true;
    publish_packet.dup = false;
    publish_packet.packet_id = 0;
    
    int ret = parser.serialize_publish(&publish_packet, serialize_buffer);
    assert(ret == 0);
    assert(serialize_buffer.size() > 0);
    
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBLISH);
    
    mqtt::PublishPacket* parsed_publish = static_cast<mqtt::PublishPacket*>(parsed_packet);
    assert(parsed_publish->qos == 0);
    assert(parsed_publish->retain == true);
    assert(parsed_publish->dup == false);
    
    std::string parsed_topic(parsed_publish->topic_name.begin(), parsed_publish->topic_name.end());
    assert(parsed_topic == topic);
    
    std::string parsed_payload(parsed_publish->payload.begin(), parsed_publish->payload.end());
    assert(parsed_payload == payload);
    
    std::cout << "Retain flag serialization test passed - packet size: " << serialize_buffer.size() << " bytes" << std::endl;
}

void test_publish_serialization_with_dup()
{
    std::cout << "Testing PUBLISH serialization with dup flag..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    mqtt::MQTTBuffer serialize_buffer(&allocator);
    
    mqtt::PublishPacket publish_packet(&allocator);
    publish_packet.type = mqtt::PacketType::PUBLISH;
    
    std::string topic = "test/dup";
    std::string payload = "Duplicate Message";
    
    publish_packet.topic_name = mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(&allocator));
    publish_packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end(), mqtt::MQTTSTLAllocator<uint8_t>(&allocator));
    publish_packet.qos = 1;
    publish_packet.retain = false;
    publish_packet.dup = true;
    publish_packet.packet_id = 9999;
    
    int ret = parser.serialize_publish(&publish_packet, serialize_buffer);
    assert(ret == 0);
    assert(serialize_buffer.size() > 0);
    
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBLISH);
    
    mqtt::PublishPacket* parsed_publish = static_cast<mqtt::PublishPacket*>(parsed_packet);
    assert(parsed_publish->qos == 1);
    assert(parsed_publish->packet_id == 9999);
    assert(parsed_publish->retain == false);
    assert(parsed_publish->dup == true);
    
    std::string parsed_topic(parsed_publish->topic_name.begin(), parsed_publish->topic_name.end());
    assert(parsed_topic == topic);
    
    std::string parsed_payload(parsed_publish->payload.begin(), parsed_publish->payload.end());
    assert(parsed_payload == payload);
    
    std::cout << "Dup flag serialization test passed - packet size: " << serialize_buffer.size() << " bytes" << std::endl;
}

void test_publish_serialization_empty_payload()
{
    std::cout << "Testing PUBLISH serialization with empty payload..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    mqtt::MQTTBuffer serialize_buffer(&allocator);
    
    mqtt::PublishPacket publish_packet(&allocator);
    publish_packet.type = mqtt::PacketType::PUBLISH;
    
    std::string topic = "test/empty";
    std::string payload = "";
    
    publish_packet.topic_name = mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(&allocator));
    publish_packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end(), mqtt::MQTTSTLAllocator<uint8_t>(&allocator));
    publish_packet.qos = 0;
    publish_packet.retain = false;
    publish_packet.dup = false;
    publish_packet.packet_id = 0;
    
    int ret = parser.serialize_publish(&publish_packet, serialize_buffer);
    assert(ret == 0);
    assert(serialize_buffer.size() > 0);
    
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBLISH);
    
    mqtt::PublishPacket* parsed_publish = static_cast<mqtt::PublishPacket*>(parsed_packet);
    assert(parsed_publish->qos == 0);
    assert(parsed_publish->payload.empty());
    
    std::string parsed_topic(parsed_publish->topic_name.begin(), parsed_publish->topic_name.end());
    assert(parsed_topic == topic);
    
    std::cout << "Empty payload serialization test passed - packet size: " << serialize_buffer.size() << " bytes" << std::endl;
}

void test_publish_serialization_large_payload()
{
    std::cout << "Testing PUBLISH serialization with large payload..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    mqtt::MQTTBuffer serialize_buffer(&allocator);
    
    mqtt::PublishPacket publish_packet(&allocator);
    publish_packet.type = mqtt::PacketType::PUBLISH;
    
    std::string topic = "test/large";
    
    std::string payload;
    payload.reserve(1024);
    for (int i = 0; i < 1024; i++) {
        payload += static_cast<char>('A' + (i % 26));
    }
    
    publish_packet.topic_name = mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(&allocator));
    publish_packet.payload = mqtt::MQTTByteVector(payload.begin(), payload.end(), mqtt::MQTTSTLAllocator<uint8_t>(&allocator));
    publish_packet.qos = 1;
    publish_packet.retain = false;
    publish_packet.dup = false;
    publish_packet.packet_id = 12345;
    
    int ret = parser.serialize_publish(&publish_packet, serialize_buffer);
    assert(ret == 0);
    assert(serialize_buffer.size() > 0);
    
    mqtt::Packet* parsed_packet = nullptr;
    ret = parser.parse_packet(serialize_buffer.data(), serialize_buffer.size(), &parsed_packet);
    assert(ret == 0);
    assert(parsed_packet != nullptr);
    assert(parsed_packet->type == mqtt::PacketType::PUBLISH);
    
    mqtt::PublishPacket* parsed_publish = static_cast<mqtt::PublishPacket*>(parsed_packet);
    assert(parsed_publish->qos == 1);
    assert(parsed_publish->packet_id == 12345);
    assert(parsed_publish->payload.size() == 1024);
    
    std::string parsed_topic(parsed_publish->topic_name.begin(), parsed_publish->topic_name.end());
    assert(parsed_topic == topic);
    
    std::string parsed_payload(parsed_publish->payload.begin(), parsed_publish->payload.end());
    assert(parsed_payload == payload);
    assert(parsed_payload.size() == 1024);
    
    std::cout << "Large payload serialization test passed - packet size: " << serialize_buffer.size() << " bytes, payload size: " << parsed_payload.size() << " bytes" << std::endl;
}

static void drain_socket(int fd)
{
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0) {
            break;
        }
    }
}

void test_allocated_packet_nested_publish_returns_to_baseline()
{
    std::cout << "Testing AllocatedPacket nested PublishPacket returns allocator to baseline..."
              << std::endl;

    MQTTAllocator allocator("allocated_packet_nested", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    const size_t baseline = allocator.get_memory_usage();

    {
        mqtt::AllocatedPacket<mqtt::PublishPacket> packet(&allocator);
        assert(packet);

        const std::string topic = "sensors/temperature/living-room/zone-alpha";
        const std::string payload(256, 'P');
        packet->topic_name =
            mqtt::MQTTString(topic.begin(), topic.end(), mqtt::MQTTStrAllocator(&allocator));
        packet->payload = mqtt::MQTTByteVector(payload.begin(), payload.end(),
                                               mqtt::MQTTSTLAllocator<uint8_t>(&allocator));
        assert(allocator.get_memory_usage() > baseline);
    }

    assert(allocator.get_memory_usage() == baseline);
    std::cout << "AllocatedPacket nested PublishPacket baseline test passed" << std::endl;
}

void test_send_publish_socketpair_allocator_unchanged()
{
    std::cout << "Testing two send_publish calls over socketpair leave allocator usage unchanged..."
              << std::endl;

    int fds[2] = {-1, -1};
    int pair_ret = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(pair_ret == 0);

    MQTTAllocator allocator("send_publish_socketpair", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    MQTTSocket sock(fds[0]);
    mqtt::MQTTProtocolHandler handler(&allocator);
    int init_ret = handler.init(&sock, "127.0.0.1", 1883);
    assert(init_ret == MQ_SUCCESS);

    const std::string topic_str = "test/allocated-packet/send-publish";
    const std::string payload_str(128, 'x');
    mqtt::MQTTString topic(topic_str.begin(), topic_str.end(), mqtt::MQTTStrAllocator(&allocator));
    mqtt::MQTTByteVector payload(payload_str.begin(), payload_str.end(),
                                 mqtt::MQTTSTLAllocator<uint8_t>(&allocator));

    int send_ret = handler.send_publish(topic, payload, 0, false, false);
    assert(send_ret == MQ_SUCCESS);
    drain_socket(fds[1]);

    const size_t baseline = allocator.get_memory_usage();

    send_ret = handler.send_publish(topic, payload, 0, false, false);
    assert(send_ret == MQ_SUCCESS);
    drain_socket(fds[1]);
    send_ret = handler.send_publish(topic, payload, 0, false, false);
    assert(send_ret == MQ_SUCCESS);
    drain_socket(fds[1]);

    assert(allocator.get_memory_usage() == baseline);

    ::close(fds[1]);
    fds[1] = -1;
    std::cout << "Two send_publish socketpair allocator test passed (baseline: " << baseline
              << " bytes)" << std::endl;
}

int main()
{
    std::cout << "Starting MQTT PUBLISH packet serialization tests\n" << std::endl;
    
    try {
        test_publish_serialization_qos0();
        test_publish_serialization_qos1();
        test_publish_serialization_qos2();
        test_publish_serialization_with_retain();
        test_publish_serialization_with_dup();
        test_publish_serialization_empty_payload();
        test_publish_serialization_large_payload();
        test_allocated_packet_nested_publish_returns_to_baseline();
        test_send_publish_socketpair_allocator_unchanged();
        
        std::cout << "\nAll PUBLISH serialization tests passed!" << std::endl;
        std::cout << "This verifies that MQTTProtocolHandler::send_publish() correctly serializes PUBLISH packets." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed: " << e.what() << std::endl;
        return 1;
    }
}