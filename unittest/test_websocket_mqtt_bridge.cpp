#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_parser.h"
#include "mqtt_buffer.h"
#include "mqtt_session_manager_v2.h"
#include "websocket_mqtt_bridge.h"

namespace {

std::vector<uint8_t> serialize_puback(MQTTAllocator& allocator, uint16_t packet_id) {
    mqtt::MQTTParser parser(&allocator);
    mqtt::PubAckPacket packet(&allocator);
    packet.type = mqtt::PacketType::PUBACK;
    packet.packet_id = packet_id;
    packet.reason_code = mqtt::ReasonCode::Success;

    mqtt::MQTTBuffer buffer(&allocator);
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

    mqtt::MQTTBuffer buffer(&allocator);
    int ret = parser.serialize_pubcomp(&packet, buffer);
    assert(ret == MQ_SUCCESS);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

std::vector<uint8_t> serialize_pingresp(MQTTAllocator& allocator) {
    mqtt::MQTTParser parser(&allocator);
    mqtt::PingRespPacket packet(&allocator);
    packet.type = mqtt::PacketType::PINGRESP;

    mqtt::MQTTBuffer buffer(&allocator);
    int ret = parser.serialize_pingresp(&packet, buffer);
    assert(ret == MQ_SUCCESS);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

std::vector<uint8_t> serialize_pingreq(MQTTAllocator& allocator) {
    mqtt::MQTTParser parser(&allocator);
    mqtt::PingReqPacket packet(&allocator);
    packet.type = mqtt::PacketType::PINGREQ;

    mqtt::MQTTBuffer buffer(&allocator);
    int ret = parser.serialize_pingreq(&packet, buffer);
    assert(ret == MQ_SUCCESS);
    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

void test_reject_puback_before_connect() {
    std::cout << "Testing PUBACK handling in WebSocket MQTT bridge..." << std::endl;

    MQTTAllocator allocator("test_ws_bridge_puback", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    bridge.set_message_format(websocket::MessageFormat::MQTT_PACKET);

    std::vector<uint8_t> puback = serialize_puback(allocator, 42);
    int ret = bridge.handle_websocket_binary("ws_client_puback", puback);

    assert(ret == MQ_ERR_PROTOCOL);
    assert(bridge.get_statistics().translation_errors.load() == 1);
    assert(bridge.get_statistics().ws_messages_received.load() == 1);

    std::cout << "PUBACK handling test passed" << std::endl;
}

void test_reject_fragmented_pingresp_before_connect() {
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
    assert(ret == MQ_ERR_PROTOCOL);
    assert(bridge.get_statistics().translation_errors.load() == 1);
    assert(bridge.get_statistics().ws_messages_received.load() == 2);

    std::cout << "Fragmented packet handling test passed" << std::endl;
}

void test_reject_multiple_packets_before_connect() {
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
    assert(ret == MQ_ERR_PROTOCOL);
    assert(bridge.get_statistics().translation_errors.load() == 1);
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

void test_text_publish_counted_when_forward_fails() {
    std::cout << "\nTesting WebSocket publish receipt is counted before forwarding..." << std::endl;

    mqtt::GlobalSessionManager manager;
    assert(manager.pre_register_threads(1, 4) == 0);
    assert(manager.register_thread_manager(std::this_thread::get_id()) != nullptr);
    assert(manager.finalize_thread_registration() == 0);

    MQTTAllocator allocator("test_ws_bridge_publish_count", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    assert(bridge.init(&manager) == MQ_SUCCESS);
    bridge.set_message_format(websocket::MessageFormat::TEXT_PROTOCOL);

    int ret = bridge.handle_websocket_text("ws_text_pub", "PUBLISH:+/bad:hi");
    assert(ret != MQ_SUCCESS);

    mqtt::ProtocolCounterSnapshot snapshot;
    manager.collect_protocol_counters(snapshot);
    assert(snapshot.publish_received == 1);
    assert(snapshot.publish_received_bytes == 2);

    std::cout << "WebSocket publish receipt counting test passed" << std::endl;
}

void test_parser_incomplete_is_malformed() {
    std::cout << "\nTesting a framed packet the parser calls incomplete is malformed..." << std::endl;

    MQTTAllocator allocator("test_ws_bridge_incomplete", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    bridge.set_message_format(websocket::MessageFormat::MQTT_PACKET);

    // Remaining Length 6 is complete, but the SUBSCRIBE lacks its options byte.
    const std::vector<uint8_t> truncated{0x82, 0x06, 0x00, 0x01, 0x00, 0x00, 0x01, 0x61};
    int ret = bridge.handle_websocket_binary("ws_client_incomplete", truncated);
    assert(ret != MQ_SUCCESS);
    assert(bridge.get_statistics().translation_errors.load() == 1);

    // Nothing is left behind to swallow later bytes.
    const std::vector<uint8_t> tail(4096, 0xC0);
    ret = bridge.handle_websocket_binary("ws_client_incomplete", tail.data(), 1);
    assert(ret == MQ_SUCCESS);
    assert(allocator.get_memory_usage() < 4096);

    std::cout << "Parser-incomplete packet test passed" << std::endl;
}

void test_oversized_packet_rejected_before_buffering() {
    std::cout << "\nTesting an oversized packet is rejected from its fixed header..." << std::endl;

    MQTTAllocator allocator("test_ws_bridge_oversize", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::ProtocolLimits limits;
    limits.max_packet_size = 256;
    websocket::WebSocketMQTTBridge bridge(&allocator);
    bridge.set_protocol_limits(&limits);
    bridge.set_message_format(websocket::MessageFormat::MQTT_PACKET);

    // PUBLISH with Remaining Length 301; only the fixed header has arrived.
    const uint8_t header[] = {0x30, 0xAD, 0x02};
    int ret = bridge.handle_websocket_binary("ws_client_oversize", header, sizeof(header));
    assert(ret == MQ_ERR_PACKET_TOO_LARGE);

    std::cout << "Oversized packet header test passed" << std::endl;
}

void test_packet_streamed_byte_by_byte() {
    std::cout << "\nTesting one packet streamed one byte at a time..." << std::endl;

    MQTTAllocator allocator("test_ws_bridge_stream", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    websocket::WebSocketMQTTBridge bridge(&allocator);
    bridge.set_message_format(websocket::MessageFormat::MQTT_PACKET);

    // A 200-byte PUBLISH before CONNECT: every prefix is buffered, the last byte is rejected.
    std::vector<uint8_t> publish{0x30, 0xC5, 0x01, 0x00, 0x01, 't', 0x00};
    publish.resize(200, 'x');
    for (size_t i = 0; i + 1 < publish.size(); ++i) {
        assert(bridge.handle_websocket_binary("ws_client_stream", &publish[i], 1) == MQ_SUCCESS);
    }
    int ret = bridge.handle_websocket_binary("ws_client_stream", &publish.back(), 1);
    assert(ret == MQ_ERR_PROTOCOL);

    std::cout << "Byte-by-byte packet test passed" << std::endl;
}

}  // namespace

int main() {
    std::cout << "=== WebSocket MQTT Bridge Unit Tests ===" << std::endl;

    test_reject_puback_before_connect();
    test_reject_fragmented_pingresp_before_connect();
    test_reject_multiple_packets_before_connect();
    test_first_packet_must_be_connect();
    test_text_publish_counted_when_forward_fails();
    test_parser_incomplete_is_malformed();
    test_oversized_packet_rejected_before_buffering();
    test_packet_streamed_byte_by_byte();

    std::cout << "\nAll WebSocket MQTT bridge tests passed!" << std::endl;
    return 0;
}
