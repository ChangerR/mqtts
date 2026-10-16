#include <gtest/gtest.h>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_buffer.h"
#include "mqtt_config.h"
#include "mqtt_connack_limits.h"
#include "mqtt_parser.h"
#include "mqtt_process_monitor.h"
#include "mqtt_session_manager_v2.h"

namespace {

std::string write_yaml(const std::string& body)
{
  char path[] = "/tmp/mqtts-phase0-XXXXXX";
  int fd = mkstemp(path);
  EXPECT_GE(fd, 0);
  std::string file(path);
  if (fd >= 0) {
    const ssize_t wrote = ::write(fd, body.data(), body.size());
    EXPECT_EQ(wrote, static_cast<ssize_t>(body.size()));
    ::close(fd);
  }
  return file;
}

bool warnings_contain(const mqtt::ConfigManager& manager, const std::string& fragment)
{
  for (const auto& warning : manager.warnings()) {
    if (warning.find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

struct ParsedConnAck {
  bool session_present = false;
  mqtt::Properties properties;
  std::vector<uint8_t> bytes;
};

ParsedConnAck announce(MQTTAllocator& allocator, const mqtt::ProtocolLimits& limits, uint8_t version,
                       uint16_t keep_alive, bool requested_expiry, bool persistence,
                       bool session_present)
{
  mqtt::ConnAckPacket packet(&allocator);
  packet.type = mqtt::PacketType::CONNACK;
  packet.reason_code = mqtt::ReasonCode::Success;
  packet.session_present = session_present;
  mqtt::apply_connack_capabilities(packet, limits, version, keep_alive, requested_expiry, persistence);

  mqtt::MQTTParser parser(&allocator);
  parser.set_protocol_version_hint(version);
  mqtt::MQTTBuffer buffer(&allocator);
  EXPECT_EQ(parser.serialize_connack(&packet, buffer), 0);

  ParsedConnAck parsed;
  parsed.bytes.assign(buffer.data(), buffer.data() + buffer.size());
  parsed.properties = mqtt::Properties(&allocator);
  mqtt::Packet* raw = nullptr;
  EXPECT_EQ(parser.parse_packet(parsed.bytes.data(), parsed.bytes.size(), &raw), 0);
  auto* connack = static_cast<mqtt::ConnAckPacket*>(raw);
  parsed.session_present = connack->session_present;
  parsed.properties = mqtt::Properties(connack->properties, &allocator);
  connack->~ConnAckPacket();
  allocator.deallocate(connack, sizeof(mqtt::ConnAckPacket));
  return parsed;
}

bool contains_bytes(const std::vector<uint8_t>& bytes, uint8_t a, uint8_t b)
{
  for (size_t i = 0; i + 1 < bytes.size(); ++i) {
    if (bytes[i] == a && bytes[i + 1] == b) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(Phase0ConnAck, V5AnnouncesConfiguredCapabilities)
{
  MQTTAllocator allocator("phase0_connack", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::ProtocolLimits limits = mqtt::ProtocolLimits::defaults();
  limits.max_packet_size = 4096;
  limits.receive_maximum = 32;
  limits.topic_alias_maximum = 16;
  limits.max_qos = 2;
  limits.retain_available = false;
  limits.wildcard_subscription_available = false;
  limits.shared_subscription_available = false;
  limits.subscription_identifier_available = false;

  ParsedConnAck connack = announce(allocator, limits, 5, 60, false, false, true);
  EXPECT_FALSE(connack.session_present);
  EXPECT_EQ(connack.properties.maximum_packet_size, 4096u);
  EXPECT_EQ(connack.properties.receive_maximum, 32u);
  EXPECT_EQ(connack.properties.topic_alias_maximum, 16u);
  EXPECT_EQ(connack.properties.maximum_qos, 0u);
  EXPECT_FALSE(contains_bytes(connack.bytes, 0x24, 0x02));
  EXPECT_FALSE(connack.properties.retain_available);
  EXPECT_FALSE(connack.properties.wildcard_subscription_available);
  EXPECT_FALSE(connack.properties.shared_subscription_available);
  EXPECT_FALSE(connack.properties.subscription_identifier_available);
  EXPECT_EQ(connack.properties.server_keep_alive, 0u);
  EXPECT_FALSE(connack.properties.has_session_expiry_interval);
}

TEST(Phase0ConnAck, MaximumQosZeroIsWritten)
{
  MQTTAllocator allocator("phase0_qos0", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::ProtocolLimits limits = mqtt::ProtocolLimits::defaults();
  limits.max_qos = 0;
  ParsedConnAck connack = announce(allocator, limits, 5, 60, false, false, false);
  EXPECT_TRUE(contains_bytes(connack.bytes, 0x24, 0x00));
}

TEST(Phase0ConnAck, PersistenceCapsMaximumQosAtOne)
{
  MQTTAllocator allocator("phase0_qos1", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::ProtocolLimits limits = mqtt::ProtocolLimits::defaults();
  limits.max_qos = 2;
  ParsedConnAck connack = announce(allocator, limits, 5, 60, false, true, true);
  EXPECT_TRUE(connack.session_present);
  EXPECT_EQ(connack.properties.maximum_qos, 1u);
}

TEST(Phase0ConnAck, ServerKeepAliveAndSessionExpiry)
{
  MQTTAllocator allocator("phase0_keepalive", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::ProtocolLimits limits = mqtt::ProtocolLimits::defaults();
  limits.keep_alive_max = 30;

  ParsedConnAck rewritten = announce(allocator, limits, 5, 0, true, false, false);
  EXPECT_EQ(rewritten.properties.server_keep_alive, 30u);
  EXPECT_TRUE(rewritten.properties.has_session_expiry_interval);
  EXPECT_EQ(rewritten.properties.session_expiry_interval, 0u);
  EXPECT_FALSE(rewritten.session_present);

  ParsedConnAck within = announce(allocator, limits, 5, 30, false, false, false);
  EXPECT_EQ(within.properties.server_keep_alive, 0u);

  ParsedConnAck legacy = announce(allocator, limits, 4, 0, true, false, true);
  ASSERT_EQ(legacy.bytes.size(), 4u);
  EXPECT_EQ(legacy.bytes[0], 0x20);
  EXPECT_EQ(legacy.bytes[2], 0x00);
  EXPECT_EQ(legacy.bytes[3], 0x00);
}

TEST(Phase0Config, UnknownKeyWarnsAndDeprecatedKeepAliveIsIgnored)
{
  const std::string path = write_yaml(
      "server:\n"
      "  port: 1883\n"
      "mqtt:\n"
      "  keep_alive_default: 15\n"
      "  not_a_real_key: 1\n"
      "  max_packet_size: 1024\n");
  mqtt::ConfigManager manager;
  EXPECT_EQ(manager.load_from_file(path), 0);
  EXPECT_TRUE(warnings_contain(manager, "mqtt.keep_alive_default"));
  EXPECT_TRUE(warnings_contain(manager, "mqtt.not_a_real_key"));
  EXPECT_EQ(manager.get_config().mqtt.keep_alive_default, 60);
  EXPECT_EQ(manager.get_config().mqtt.connect_timeout_seconds, 10u);
  EXPECT_FALSE(manager.get_config().mqtt.implicit_no_local);
  EXPECT_FALSE(manager.get_config().mqtt.subscription_identifier_available);
  EXPECT_FALSE(manager.get_config().mqtt.shared_subscription_available);
  ::unlink(path.c_str());
}

TEST(Phase0Config, WebsocketKeysAreListed)
{
  const std::string path = write_yaml(
      "server:\n"
      "  port: 1883\n"
      "websocket:\n"
      "  enabled: true\n"
      "  port: 18080\n");
  mqtt::ConfigManager manager;
  EXPECT_EQ(manager.load_from_file(path), 0);
  EXPECT_TRUE(warnings_contain(manager, "websocket.enabled"));
  EXPECT_TRUE(warnings_contain(manager, "websocket.port"));
  ::unlink(path.c_str());
}

TEST(Phase0Config, UnsupportedCapabilitiesFailStartup)
{
  const std::string shared = write_yaml(
      "server:\n  port: 1883\nmqtt:\n  shared_subscription_available: true\n");
  mqtt::ConfigManager shared_manager;
  EXPECT_NE(shared_manager.load_from_file(shared), 0);
  EXPECT_NE(shared_manager.last_error().find("shared_subscription_available"), std::string::npos);
  ::unlink(shared.c_str());

  const std::string subid = write_yaml(
      "server:\n  port: 1883\nmqtt:\n  subscription_identifier_available: true\n");
  mqtt::ConfigManager subid_manager;
  EXPECT_NE(subid_manager.load_from_file(subid), 0);
  EXPECT_NE(subid_manager.last_error().find("subscription_identifier_available"), std::string::npos);
  ::unlink(subid.c_str());
}

TEST(Phase0Config, LimitValidation)
{
  const std::string packet = write_yaml(
      "server:\n  port: 1883\n"
      "memory:\n  client_max_size: 131072\n"
      "mqtt:\n  max_packet_size: 131072\n");
  mqtt::ConfigManager packet_manager;
  EXPECT_NE(packet_manager.load_from_file(packet), 0);
  EXPECT_NE(packet_manager.last_error().find("max_packet_size"), std::string::npos);
  ::unlink(packet.c_str());

  const std::string receive = write_yaml(
      "server:\n  port: 1883\nmqtt:\n  receive_maximum: 0\n");
  mqtt::ConfigManager receive_manager;
  EXPECT_NE(receive_manager.load_from_file(receive), 0);
  EXPECT_NE(receive_manager.last_error().find("receive_maximum"), std::string::npos);
  ::unlink(receive.c_str());

  const std::string alias = write_yaml(
      "server:\n  port: 1883\nmqtt:\n  topic_alias_maximum: 65536\n");
  mqtt::ConfigManager alias_manager;
  EXPECT_NE(alias_manager.load_from_file(alias), 0);
  EXPECT_NE(alias_manager.last_error().find("topic_alias_maximum"), std::string::npos);
  ::unlink(alias.c_str());
}

TEST(Phase0Config, SampleConfigs)
{
  mqtt::ConfigManager stock;
  EXPECT_EQ(stock.load_from_file(std::string(MQTTS_SOURCE_DIR) + "/mqtts.yaml"), 0);
  EXPECT_TRUE(warnings_contain(stock, "websocket.enabled"));
  EXPECT_FALSE(warnings_contain(stock, "keep_alive_default"));
  EXPECT_EQ(stock.get_config().mqtt.connect_timeout_seconds, 10u);
  EXPECT_FALSE(stock.get_config().mqtt.implicit_no_local);
  EXPECT_EQ(stock.get_config().retain.max_messages, 100000u);
  EXPECT_EQ(stock.get_config().retain.max_payload_bytes, stock.get_config().mqtt.max_packet_size);

  mqtt::ConfigManager router;
  EXPECT_NE(router.load_from_file(std::string(MQTTS_SOURCE_DIR) + "/mqtts_with_router.yaml"), 0);
  EXPECT_TRUE(warnings_contain(router, "router.enabled"));
  EXPECT_TRUE(warnings_contain(router, "server.worker_threads"));
  EXPECT_NE(router.last_error().find("max_packet_size"), std::string::npos);
}

TEST(Phase0Counters, ThreadCountersAggregateIntoJson)
{
  mqtt::GlobalSessionManager manager;
  ASSERT_EQ(manager.pre_register_threads(1, 4), 0);
  ASSERT_NE(manager.register_thread_manager(std::this_thread::get_id()), nullptr);
  ASSERT_EQ(manager.finalize_thread_registration(), 0);

  manager.note_publish_received(5);
  manager.note_publish_received(7);
  manager.note_publish_sent(4);
  manager.note_bytes_received(100);
  manager.note_bytes_sent(40);

  mqtt::ProtocolCounterSnapshot snapshot;
  manager.collect_protocol_counters(snapshot);
  EXPECT_EQ(snapshot.publish_received, 2u);
  EXPECT_EQ(snapshot.publish_received_bytes, 12u);
  EXPECT_EQ(snapshot.publish_sent, 1u);
  EXPECT_EQ(snapshot.publish_sent_bytes, 4u);
  EXPECT_EQ(snapshot.bytes_received, 100u);
  EXPECT_EQ(snapshot.bytes_sent, 40u);
  EXPECT_EQ(snapshot.keepalive_timeouts, 0u);
  EXPECT_EQ(snapshot.wills_published, 0u);
  EXPECT_EQ(snapshot.retained_messages, 0u);
  EXPECT_EQ(snapshot.malformed_packet_disconnects, 0u);

  const std::string json_path = "/tmp/mqtts-phase0-counters.json";
  mqtt::ProcessMonitor monitor(&manager, 5);
  monitor.set_json_output_file(json_path);
  monitor.print_status_once();

  std::ifstream input(json_path);
  std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  for (const char* field : {"\"publish_received\"", "\"publish_received_bytes\"", "\"publish_sent\"",
                            "\"publish_sent_bytes\"", "\"bytes_received\"", "\"bytes_sent\"",
                            "\"keepalive_timeouts\"", "\"connect_timeouts\"", "\"session_takeovers\"",
                            "\"malformed_packet_disconnects\"", "\"outbound_packet_size_drops\"",
                            "\"inflight_window_delays\"", "\"wills_published\"", "\"wills_rejected\"",
                            "\"retained_messages\"", "\"retained_bytes\"", "\"retained_not_stored\"",
                            "\"retained_delivery_truncated\""}) {
    EXPECT_NE(json.find(field), std::string::npos) << field;
  }
  EXPECT_NE(json.find("\"publish_received\": 2"), std::string::npos);
  ::unlink(json_path.c_str());
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
