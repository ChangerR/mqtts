#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_buffer.h"
#include "mqtt_define.h"
#include "mqtt_memory_tags.h"
#include "mqtt_packet.h"
#include "mqtt_parser.h"

namespace {

void append_remaining(std::vector<uint8_t>& out, size_t size)
{
  do {
    uint8_t byte = size % 128;
    size /= 128;
    if (size) {
      byte |= 0x80;
    }
    out.push_back(byte);
  } while (size);
}

void append_utf(std::vector<uint8_t>& out, const std::string& value)
{
  out.push_back(static_cast<uint8_t>(value.size() >> 8));
  out.push_back(static_cast<uint8_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

std::vector<uint8_t> frame(uint8_t header, const std::vector<uint8_t>& body)
{
  std::vector<uint8_t> out(1, header);
  append_remaining(out, body.size());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

std::vector<uint8_t> connect_with_properties(const std::vector<uint8_t>& properties)
{
  std::vector<uint8_t> body;
  append_utf(body, "MQTT");
  body.push_back(5);
  body.push_back(0x02);
  body.push_back(0);
  body.push_back(60);
  append_remaining(body, properties.size());
  body.insert(body.end(), properties.begin(), properties.end());
  append_utf(body, "flow-zero");
  return frame(0x10, body);
}

std::vector<uint8_t> publish_bytes(size_t payload_size)
{
  std::vector<uint8_t> body;
  append_utf(body, "big/t");
  body.push_back(0);
  body.push_back(1);
  body.push_back(0);
  body.insert(body.end(), payload_size, 'x');
  return frame(0x32, body);
}

int parse(MQTTAllocator& allocator, const std::vector<uint8_t>& bytes,
          mqtt::Packet** packet)
{
  mqtt::MQTTParser parser(&allocator);
  parser.set_protocol_version_hint(5);
  return parser.parse_packet(bytes.data(), bytes.size(), packet);
}

}  // namespace

void release_connect(MQTTAllocator& allocator, mqtt::Packet* packet)
{
  mqtt::ConnectPacket* connect = static_cast<mqtt::ConnectPacket*>(packet);
  connect->~ConnectPacket();
  allocator.deallocate(connect, sizeof(mqtt::ConnectPacket));
}

TEST(Phase0Parser, ZeroFlowLimitsStayVisibleForConnackRejection)
{
  MQTTAllocator allocator("flow-zero", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::Packet* packet = nullptr;

  ASSERT_EQ(parse(allocator, connect_with_properties({0x21, 0x00, 0x00}), &packet), MQ_SUCCESS);
  EXPECT_TRUE(static_cast<mqtt::ConnectPacket*>(packet)->properties.has_receive_maximum);
  EXPECT_EQ(static_cast<mqtt::ConnectPacket*>(packet)->properties.receive_maximum, 0u);
  release_connect(allocator, packet);

  ASSERT_EQ(parse(allocator, connect_with_properties({0x27, 0x00, 0x00, 0x00, 0x00}), &packet),
            MQ_SUCCESS);
  EXPECT_TRUE(static_cast<mqtt::ConnectPacket*>(packet)->properties.has_maximum_packet_size);
  EXPECT_EQ(static_cast<mqtt::ConnectPacket*>(packet)->properties.maximum_packet_size, 0u);
  release_connect(allocator, packet);

  ASSERT_EQ(parse(allocator, connect_with_properties({0x21, 0x00, 0x01, 0x27, 0x00, 0x00, 0x00,
                                                      0x01}),
                  &packet),
            MQ_SUCCESS);
  EXPECT_EQ(static_cast<mqtt::ConnectPacket*>(packet)->properties.receive_maximum, 1u);
  EXPECT_EQ(static_cast<mqtt::ConnectPacket*>(packet)->properties.maximum_packet_size, 1u);
  release_connect(allocator, packet);
  EXPECT_EQ(allocator.get_memory_usage(), 0u);
}

TEST(Phase0Parser, BudgetExhaustionReturnsErrorAndReleasesPacket)
{
  MQTTAllocator allocator("tiny", MQTTMemoryTag::MEM_TAG_CLIENT, 64 * 1024);
  const std::vector<uint8_t> bytes = publish_bytes(100 * 1024);
  mqtt::Packet* packet = reinterpret_cast<mqtt::Packet*>(&allocator);

  EXPECT_EQ(parse(allocator, bytes, &packet), MQ_ERR_MEMORY_ALLOC);
  EXPECT_EQ(packet, nullptr);
  EXPECT_EQ(allocator.get_memory_usage(), 0u);

  const std::vector<uint8_t> fits = publish_bytes(16 * 1024);
  ASSERT_EQ(parse(allocator, fits, &packet), MQ_SUCCESS);
  mqtt::PublishPacket* publish = static_cast<mqtt::PublishPacket*>(packet);
  EXPECT_EQ(publish->payload.size(), 16u * 1024);
  publish->~PublishPacket();
  allocator.deallocate(publish, sizeof(mqtt::PublishPacket));
  EXPECT_EQ(allocator.get_memory_usage(), 0u);
}

TEST(Phase0Parser, PublishHeaderPlusPayloadMatchesFullSerialization)
{
  MQTTAllocator allocator("serialize", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::MQTTParser parser(&allocator);
  parser.set_protocol_version_hint(5);

  mqtt::PublishPacket packet(&allocator);
  packet.type = mqtt::PacketType::PUBLISH;
  packet.topic_name = mqtt::to_mqtt_string("a/b", &allocator);
  packet.qos = 1;
  packet.retain = true;
  packet.dup = false;
  packet.packet_id = 0x1234;
  packet.properties.has_message_expiry_interval = true;
  packet.properties.message_expiry_interval = 30;
  packet.payload.assign(300, 'p');

  mqtt::MQTTBuffer full(&allocator);
  ASSERT_EQ(parser.serialize_publish(&packet, full), MQ_SUCCESS);

  mqtt::MQTTBuffer header(&allocator);
  ASSERT_EQ(parser.serialize_publish_header(packet.topic_name, packet.qos, packet.retain,
                                            packet.dup, packet.packet_id, packet.properties,
                                            packet.payload.size(), header),
            MQ_SUCCESS);
  std::vector<uint8_t> joined(header.data(), header.data() + header.size());
  joined.insert(joined.end(), packet.payload.begin(), packet.payload.end());
  EXPECT_EQ(joined, std::vector<uint8_t>(full.data(), full.data() + full.size()));

  mqtt::Packet* parsed = nullptr;
  ASSERT_EQ(parser.parse_packet(joined.data(), joined.size(), &parsed), MQ_SUCCESS);
  mqtt::PublishPacket* round_trip = static_cast<mqtt::PublishPacket*>(parsed);
  EXPECT_EQ(round_trip->payload.size(), 300u);
  EXPECT_EQ(round_trip->packet_id, 0x1234);
  round_trip->~PublishPacket();
  allocator.deallocate(round_trip, sizeof(mqtt::PublishPacket));
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
