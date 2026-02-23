#include <cassert>
#include <iostream>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_parser.h"
#include "mqtt_serialize_buffer.h"
#include "websocket_mqtt_bridge.h"

namespace {

std::vector<uint8_t> serialize_puback(MQTTAllocator& allocator, uint16_t packet_id) {
    mqtt::MQTTParser parser(&allocator);
    mqtt::PubAckPacket packet(&allocator);
    packet.type = mqtt::PacketType::PUBACK;
    packet.packet_id = packet_id;
    packet.reason_code = mqtt::ReasonCode::Success;

    mqtt::MQTTSerializeBuffer buffer(&allocator);
    int ret = parser.serialize_puback(&packet, buffer);
    assert(ret == MQ_SUCCESS);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

std::vector<uint8_t> serialize_pubcomp(MQTTAllocator& allocator, uint16_t packet_id) {
    mqtt::MQTTParser parser(&allocator);
    mqtt::PubCompPacket packet(&allocator);
    packet.type = mqtt::PacketType::PUBCOMP;
    packet.packet_id = packet_id;
    packet.reason_code = mqtt::ReasonCode::Success;

    mqtt::MQTTSerializeBuffer buffer(&allocator);
    int ret = parser.serialize_pubcomp(&packet, buffer);
    assert(ret == MQ_SUCCESS);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

std::vector<uint8_t> serialize_pingresp(MQTTAllocator& allocator) {
    mqtt::MQTTParser parser(&allocator);
    mqtt::PingRespPacket packet(&allocator);
    packet.type = mqtt::PacketType::PINGRESP;

    mqtt::MQTTSerializeBuffer buffer(&allocator);
    int ret = parser.serialize_pingresp(&packet, buffer);
    assert(ret == MQ_SUCCESS);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

std::vector<uint8_t> serialize_pingreq(MQTTAllocator& allocator) {
    mqtt::MQTTParser parser(&allocator);
    mqtt::PingReqPacket packet(&allocator);
    packet.type = mqtt::PacketType::PINGREQ;

    mqtt::MQTTSerializeBuffer buffer(&allocator);
    int ret = parser.serialize_pingreq(&packet, buffer);
    assert(ret == MQ_SUCCESS);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

void test_handle_puback_packet_success() {
    std::cout << "Testing PUBACK handling in WebSocket MQTT bridge..." << std::endl;

    MQTTAllocator allocator("test_ws_bridge_puback", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    bridge.set_message_format(websocket::MessageFormat::MQTT_PACKET);

    std::vector<uint8_t> puback = serialize_puback(allocator, 42);
    int ret = bridge.handle_websocket_binary("ws_client_puback", puback);

    assert(ret == MQ_SUCCESS);
    assert(bridge.get_statistics().translation_errors.load() == 0);
    assert(bridge.get_statistics().ws_messages_received.load() == 1);

    std::cout << "PUBACK handling test passed" << std::endl;
}

void test_handle_fragmented_pingresp_packet_success() {
    std::cout << "\nTesting fragmented MQTT packet handling..." << std::endl;

    MQTTAllocator allocator("test_ws_bridge_fragment", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    bridge.set_message_format(websocket::MessageFormat::MQTT_PACKET);

    std::vector<uint8_t> pingresp = serialize_pingresp(allocator);
    assert(pingresp.size() == 2);

    std::vector<uint8_t> first_half{pingresp[0]};
    std::vector<uint8_t> second_half{pingresp[1]};

    int ret = bridge.handle_websocket_binary("ws_client_fragment", first_half);
    assert(ret == MQ_SUCCESS);

    ret = bridge.handle_websocket_binary("ws_client_fragment", second_half);
    assert(ret == MQ_SUCCESS);
    assert(bridge.get_statistics().translation_errors.load() == 0);
    assert(bridge.get_statistics().ws_messages_received.load() == 2);

    std::cout << "Fragmented packet handling test passed" << std::endl;
}

void test_handle_multiple_packets_in_one_frame_success() {
    std::cout << "\nTesting multi-packet handling in one binary frame..." << std::endl;

    MQTTAllocator allocator("test_ws_bridge_multi", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    bridge.set_message_format(websocket::MessageFormat::MQTT_PACKET);

    std::vector<uint8_t> puback = serialize_puback(allocator, 100);
    std::vector<uint8_t> pubcomp = serialize_pubcomp(allocator, 100);

    std::vector<uint8_t> combined;
    combined.reserve(puback.size() + pubcomp.size());
    combined.insert(combined.end(), puback.begin(), puback.end());
    combined.insert(combined.end(), pubcomp.begin(), pubcomp.end());

    int ret = bridge.handle_websocket_binary("ws_client_multi", combined);
    assert(ret == MQ_SUCCESS);
    assert(bridge.get_statistics().translation_errors.load() == 0);
    assert(bridge.get_statistics().ws_messages_received.load() == 1);

    std::cout << "Multi-packet frame handling test passed" << std::endl;
}

void test_first_packet_must_be_connect() {
    std::cout << "\nTesting first MQTT packet must be CONNECT in WebSocket binary mode..." << std::endl;

    MQTTAllocator allocator("test_ws_bridge_first_packet", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    bridge.set_message_format(websocket::MessageFormat::MQTT_PACKET);

    std::vector<uint8_t> pingreq = serialize_pingreq(allocator);
    int ret = bridge.handle_websocket_binary("ws_client_no_connect", pingreq);
    assert(ret == MQ_ERR_PROTOCOL);
    assert(bridge.get_statistics().translation_errors.load() >= 1);

    std::cout << "First-packet CONNECT guard test passed" << std::endl;
}

}  // namespace

int main() {
    std::cout << "=== WebSocket MQTT Bridge Unit Tests ===" << std::endl;

    test_handle_puback_packet_success();
    test_handle_fragmented_pingresp_packet_success();
    test_handle_multiple_packets_in_one_frame_success();
    test_first_packet_must_be_connect();

    std::cout << "\nAll WebSocket MQTT bridge tests passed!" << std::endl;
    return 0;
}
