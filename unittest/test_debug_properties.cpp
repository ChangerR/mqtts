#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include "src/mqtt_allocator.h"
#include "src/mqtt_parser.h"
#include "src/mqtt_serialize_buffer.h"

int main()
{
    std::cout << "Testing empty properties serialization..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    
    // Create a PUBLISH packet with empty properties to test serialization
    mqtt::PublishPacket* packet = new (allocator.allocate(sizeof(mqtt::PublishPacket))) mqtt::PublishPacket(&allocator);
    packet->type = mqtt::PacketType::PUBLISH;
    packet->topic_name = mqtt::to_mqtt_string("test/topic", &allocator);
    packet->qos = 1;
    packet->retain = false;
    packet->dup = false;
    packet->packet_id = 123;
    
    // The payload can be empty
    packet->payload.clear();
    
    // Properties should be empty (default constructed)
    
    // Try to serialize the packet which will include properties serialization
    mqtt::MQTTSerializeBuffer buffer(&allocator);
    int ret = parser.serialize_publish(packet, buffer);
    
    std::cout << "serialize_publish returned: " << ret << std::endl;
    std::cout << "Packet buffer size: " << buffer.size() << std::endl;
    
    if (ret == 0 && buffer.size() > 0) {
        std::cout << "Packet buffer contents (first 20 bytes):";
        size_t display_size = std::min(buffer.size(), static_cast<size_t>(20));
        for (size_t i = 0; i < display_size; ++i) {
            std::cout << " 0x" << std::hex << static_cast<int>(buffer.data()[i]);
        }
        std::cout << std::endl;
    }
    
    // Clean up
    packet->~PublishPacket();
    allocator.deallocate(packet, sizeof(mqtt::PublishPacket));
    
    std::cout << "Empty properties serialization test completed" << std::endl;
    return 0;
}