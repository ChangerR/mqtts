#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_parser.h"
#include "mqtt_topic_alias.h"

namespace {

mqtt::MQTTString topic_of(const char* text, MQTTAllocator* allocator)
{
  return mqtt::to_mqtt_string(text, allocator);
}

}  // namespace

TEST(InboundTopicAlias, EstablishResolveAndReplace)
{
  MQTTAllocator allocator("alias", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::InboundTopicAliases table(&allocator);
  mqtt::MQTTString topic = topic_of("sensors/temp", &allocator);

  EXPECT_EQ(table.apply(false, 0, 16, topic), mqtt::InboundTopicAliases::Result::Unused);
  EXPECT_EQ(mqtt::from_mqtt_string(topic), "sensors/temp");

  EXPECT_EQ(table.apply(true, 1, 16, topic), mqtt::InboundTopicAliases::Result::Resolved);
  EXPECT_EQ(mqtt::from_mqtt_string(topic), "sensors/temp");

  topic.clear();
  EXPECT_EQ(table.apply(true, 1, 16, topic), mqtt::InboundTopicAliases::Result::Resolved);
  EXPECT_EQ(mqtt::from_mqtt_string(topic), "sensors/temp");

  topic = topic_of("sensors/other", &allocator);
  EXPECT_EQ(table.apply(true, 1, 16, topic), mqtt::InboundTopicAliases::Result::Resolved);
  topic.clear();
  EXPECT_EQ(table.apply(true, 1, 16, topic), mqtt::InboundTopicAliases::Result::Resolved);
  EXPECT_EQ(mqtt::from_mqtt_string(topic), "sensors/other");

  mqtt::InboundTopicAliases other(&allocator);
  mqtt::MQTTString fresh{mqtt::MQTTStrAllocator(&allocator)};
  EXPECT_EQ(other.apply(true, 1, 16, fresh), mqtt::InboundTopicAliases::Result::Unknown);
}

TEST(InboundTopicAlias, ZeroAndOutOfRangeAndUnknown)
{
  MQTTAllocator allocator("alias-range", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::InboundTopicAliases table(&allocator);
  mqtt::MQTTString topic = topic_of("a/b", &allocator);

  EXPECT_EQ(table.apply(true, 0, 16, topic), mqtt::InboundTopicAliases::Result::Invalid);
  EXPECT_EQ(mqtt::from_mqtt_string(topic), "a/b");
  EXPECT_EQ(table.apply(true, 17, 16, topic), mqtt::InboundTopicAliases::Result::Invalid);
  EXPECT_EQ(table.apply(true, 16, 16, topic), mqtt::InboundTopicAliases::Result::Resolved);

  topic.clear();
  EXPECT_EQ(table.apply(true, 2, 16, topic), mqtt::InboundTopicAliases::Result::Unknown);
  EXPECT_TRUE(topic.empty());

  mqtt::InboundTopicAliases closed(&allocator);
  topic = topic_of("a/b", &allocator);
  EXPECT_EQ(closed.apply(true, 1, 0, topic), mqtt::InboundTopicAliases::Result::Invalid);
}

TEST(InboundTopicAlias, AllocationFailureDoesNotStore)
{
  MQTTAllocator source("alias-source", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  MQTTAllocator limited("alias-limited", MQTTMemoryTag::MEM_TAG_CLIENT, 1);
  mqtt::InboundTopicAliases table(&limited);
  mqtt::MQTTString topic = topic_of("sensors/temp", &source);

  EXPECT_EQ(table.apply(true, 1, 16, topic), mqtt::InboundTopicAliases::Result::Quota);
  EXPECT_EQ(mqtt::from_mqtt_string(topic), "sensors/temp");

  mqtt::MQTTString again{mqtt::MQTTStrAllocator(&source)};
  EXPECT_EQ(table.apply(true, 1, 16, again), mqtt::InboundTopicAliases::Result::Unknown);
}

TEST(InboundTopicAlias, ParserRecordsAliasZero)
{
  const uint8_t raw[] = {0x30, 0x08, 0x00, 0x00, 0x03, 0x23, 0x00, 0x00, 'h', 'i'};
  MQTTAllocator allocator("alias-parse", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::MQTTParser parser(&allocator);
  parser.set_protocol_version_hint(5);
  mqtt::Packet* packet = nullptr;
  ASSERT_EQ(parser.parse_packet(raw, sizeof(raw), &packet), 0);
  ASSERT_NE(packet, nullptr);
  auto* publish = static_cast<mqtt::PublishPacket*>(packet);
  EXPECT_TRUE(publish->topic_name.empty());
  EXPECT_TRUE(publish->properties.has_topic_alias);
  EXPECT_EQ(publish->properties.topic_alias, 0);
  EXPECT_EQ(mqtt::from_mqtt_bytes(publish->payload), std::vector<uint8_t>({'h', 'i'}));
  publish->~PublishPacket();
  allocator.deallocate(packet, sizeof(mqtt::PublishPacket));

  mqtt::PublishPacket named(&allocator);
  named.type = mqtt::PacketType::PUBLISH;
  named.dup = false;
  named.qos = 0;
  named.retain = false;
  named.topic_name = topic_of("sensors/temp", &allocator);
  named.properties.topic_alias = 4;
  named.payload = mqtt::to_mqtt_bytes(std::vector<uint8_t>{'z'}, &allocator);
  mqtt::MQTTBuffer buffer(&allocator);
  ASSERT_EQ(parser.serialize_publish(&named, buffer), 0);
  mqtt::Packet* parsed = nullptr;
  ASSERT_EQ(parser.parse_packet(buffer.data(), buffer.size(), &parsed), 0);
  auto* round = static_cast<mqtt::PublishPacket*>(parsed);
  EXPECT_TRUE(round->properties.has_topic_alias);
  EXPECT_EQ(round->properties.topic_alias, 4);
  EXPECT_EQ(mqtt::from_mqtt_string(round->topic_name), "sensors/temp");
  round->~PublishPacket();
  allocator.deallocate(parsed, sizeof(mqtt::PublishPacket));

  const uint8_t duplicate[] = {0x30, 0x0B, 0x00, 0x00, 0x06, 0x23, 0x00, 0x01,
                               0x23, 0x00, 0x02, 'h', 'i'};
  mqtt::Packet* rejected = nullptr;
  EXPECT_EQ(parser.parse_packet(duplicate, sizeof(duplicate), &rejected), MQ_ERR_PROTOCOL);
  EXPECT_EQ(rejected, nullptr);
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
