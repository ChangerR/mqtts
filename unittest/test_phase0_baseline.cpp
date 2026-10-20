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
#include "mqtt_connection_watch.h"
#include "mqtt_protocol_handler.h"
#include "mqtt_runtime.h"
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

TEST(Phase0ConnAck, ParsedMaximumQosZeroSurvivesReserialize)
{
  MQTTAllocator allocator("phase0_qos0_parse", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::ConnAckPacket packet(&allocator);
  packet.type = mqtt::PacketType::CONNACK;
  packet.reason_code = mqtt::ReasonCode::Success;
  packet.properties.has_maximum_qos = true;
  packet.properties.maximum_qos = 0;

  mqtt::MQTTParser parser(&allocator);
  parser.set_protocol_version_hint(5);
  mqtt::MQTTBuffer buffer(&allocator);
  ASSERT_EQ(parser.serialize_connack(&packet, buffer), 0);

  mqtt::Packet* raw = nullptr;
  ASSERT_EQ(parser.parse_packet(buffer.data(), buffer.size(), &raw), 0);
  auto* parsed = static_cast<mqtt::ConnAckPacket*>(raw);
  EXPECT_TRUE(parsed->properties.has_maximum_qos);
  EXPECT_EQ(parsed->properties.maximum_qos, 0u);

  mqtt::MQTTBuffer again(&allocator);
  ASSERT_EQ(parser.serialize_connack(parsed, again), 0);
  std::vector<uint8_t> bytes(again.data(), again.data() + again.size());
  EXPECT_TRUE(contains_bytes(bytes, 0x24, 0x00));

  parsed->~ConnAckPacket();
  allocator.deallocate(parsed, sizeof(mqtt::ConnAckPacket));
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
  mqtt::ConfigManager auth;
  EXPECT_EQ(auth.load_from_file(std::string(MQTTS_SOURCE_DIR) + "/mqtts_with_auth.yaml"), 0);
  EXPECT_EQ(auth.get_config().mqtt.max_packet_size, 268435455u);
  EXPECT_GE(auth.get_config().memory.client_max_size,
            static_cast<size_t>(auth.get_config().mqtt.max_packet_size) + 64 * 1024);

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
  manager.note_keepalive_timeout();
  manager.note_connect_timeout();
  manager.note_session_takeover();
  manager.note_malformed_disconnect();
  manager.note_outbound_packet_size_drop();
  manager.note_inflight_window_delay();
  manager.note_will_published();
  manager.note_will_rejected();
  manager.note_retained_delta(2, 9);
  manager.note_retained_delta(-1, -4);
  manager.note_retained_not_stored();
  manager.note_retained_delivery_truncated();

  mqtt::ProtocolCounterSnapshot snapshot;
  manager.collect_protocol_counters(snapshot);
  EXPECT_EQ(snapshot.publish_received, 2u);
  EXPECT_EQ(snapshot.publish_received_bytes, 12u);
  EXPECT_EQ(snapshot.publish_sent, 1u);
  EXPECT_EQ(snapshot.publish_sent_bytes, 4u);
  EXPECT_EQ(snapshot.bytes_received, 100u);
  EXPECT_EQ(snapshot.bytes_sent, 40u);
  EXPECT_EQ(snapshot.keepalive_timeouts, 1u);
  EXPECT_EQ(snapshot.connect_timeouts, 1u);
  EXPECT_EQ(snapshot.session_takeovers, 1u);
  EXPECT_EQ(snapshot.malformed_packet_disconnects, 1u);
  EXPECT_EQ(snapshot.outbound_packet_size_drops, 1u);
  EXPECT_EQ(snapshot.inflight_window_delays, 1u);
  EXPECT_EQ(snapshot.wills_published, 1u);
  EXPECT_EQ(snapshot.wills_rejected, 1u);
  EXPECT_EQ(snapshot.retained_messages, 1u);
  EXPECT_EQ(snapshot.retained_bytes, 5u);
  EXPECT_EQ(snapshot.retained_not_stored, 1u);
  EXPECT_EQ(snapshot.retained_delivery_truncated, 1u);

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
  EXPECT_NE(json.find("\"keepalive_timeouts\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"connect_timeouts\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"session_takeovers\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"malformed_packet_disconnects\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"outbound_packet_size_drops\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"inflight_window_delays\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"wills_published\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"wills_rejected\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"retained_messages\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"retained_bytes\": 5"), std::string::npos);
  EXPECT_NE(json.find("\"retained_not_stored\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"retained_delivery_truncated\": 1"), std::string::npos);
  ::unlink(json_path.c_str());
}

TEST(Phase0KeepAlive, NegotiatesServerKeepAliveOnlyForV5)
{
  EXPECT_EQ(mqtt::negotiated_keep_alive_seconds(0, 30, 5), 30);
  EXPECT_EQ(mqtt::negotiated_keep_alive_seconds(90, 30, 5), 30);
  EXPECT_EQ(mqtt::negotiated_keep_alive_seconds(15, 30, 5), 15);
  EXPECT_EQ(mqtt::negotiated_keep_alive_seconds(0, 30, 4), 0);
  EXPECT_EQ(mqtt::negotiated_keep_alive_seconds(90, 30, 4), 90);
  EXPECT_EQ(mqtt::negotiated_keep_alive_seconds(15, 0, 5), 15);
}

TEST(Phase0KeepAlive, DeadlineIsConnectThenOneAndAHalfKeepAlive)
{
  mqtt::ConnectionWatch watch;
  const auto start = std::chrono::steady_clock::now();
  watch.start(start, 10);
  mqtt::ConnectionWatch::DeadlineKind kind = mqtt::ConnectionWatch::DeadlineKind::None;
  EXPECT_EQ(watch.recv_deadline_ms(&kind), mqtt::ConnectionWatch::epoch_ms(start) + 10000);
  EXPECT_EQ(kind, mqtt::ConnectionWatch::DeadlineKind::Connect);

  watch.observe_connect(2, start);
  EXPECT_EQ(watch.recv_deadline_ms(&kind), mqtt::ConnectionWatch::epoch_ms(start) + 3000);
  EXPECT_EQ(kind, mqtt::ConnectionWatch::DeadlineKind::KeepAlive);

  watch.observe_connect(0, start);
  EXPECT_EQ(watch.recv_deadline_ms(&kind), -1);
  EXPECT_EQ(kind, mqtt::ConnectionWatch::DeadlineKind::None);
}

namespace {

class TakeoverProbe : public mqtt::MQTTProtocolHandler
{
 public:
  explicit TakeoverProbe(MQTTAllocator* allocator) : mqtt::MQTTProtocolHandler(allocator) {}

  void request_session_takeover(const std::shared_ptr<mqtt::SessionTakeoverGate>& gate) override
  {
    called.store(true);
    if (gate) {
      gate->done.store(true, std::memory_order_release);
    }
  }

  std::atomic<bool> called{false};
};

struct TakeoverLoop
{
  mqtt::GlobalSessionManager* manager = nullptr;
  std::atomic<bool>* stop = nullptr;
};

int takeover_loop(void* arg)
{
  auto* loop = static_cast<TakeoverLoop*>(arg);
  if (auto* local = loop->manager->get_thread_manager()) {
    local->process_control_messages();
  }
  return loop->stop->load() ? -1 : 0;
}

}  // namespace

TEST(Phase0Takeover, ControlMessageRunsOnTheOwningThread)
{
  MQTTAllocator allocator("phase0_takeover", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  const mqtt::MQTTString client_id = mqtt::to_mqtt_string("same-client", &allocator);
  mqtt::GlobalSessionManager manager;
  ASSERT_EQ(manager.pre_register_threads(2, 8), 0);

  std::atomic<bool> registered{false};
  std::atomic<bool> stop{false};
  TakeoverLoop loop{&manager, &stop};
  TakeoverProbe* original = nullptr;
  std::thread owner([&] {
    ASSERT_NE(manager.register_thread_manager(std::this_thread::get_id()), nullptr);
    original = new TakeoverProbe(&allocator);
    ASSERT_EQ(manager.register_session(client_id, original), 0);
    registered.store(true);
    mqtt::runtime::current_runtime().run_event_loop(takeover_loop, &loop);
  });
  while (!registered.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_NE(manager.register_thread_manager(std::this_thread::get_id()), nullptr);
  ASSERT_EQ(manager.finalize_thread_registration(), 0);

  TakeoverProbe replacement(&allocator);
  ASSERT_EQ(manager.register_session(client_id, &replacement), 0);
  EXPECT_TRUE(original->called.load());
  EXPECT_EQ(manager.find_client_manager(client_id), manager.get_thread_manager());

  stop.store(true);
  owner.join();
  delete original;
}

TEST(Phase0Takeover, TimedOutRequestDoesNotDisconnectLater)
{
  MQTTAllocator allocator("phase0_takeover_timeout", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  const mqtt::MQTTString client_id = mqtt::to_mqtt_string("timeout-client", &allocator);
  mqtt::GlobalSessionManager manager;
  ASSERT_EQ(manager.pre_register_threads(2, 8), 0);

  std::atomic<bool> registered{false};
  std::atomic<bool> stop{false};
  std::thread::id owner_id;
  TakeoverProbe* original = nullptr;
  std::thread owner([&] {
    auto* local = manager.register_thread_manager(std::this_thread::get_id());
    ASSERT_NE(local, nullptr);
    local->set_session_removal_timeout_ms(40);
    owner_id = std::this_thread::get_id();
    original = new TakeoverProbe(&allocator);
    ASSERT_EQ(manager.register_session(client_id, original), 0);
    registered.store(true);
    while (!stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (auto* current = manager.get_thread_manager()) {
      current->process_control_messages();
    }
  });
  while (!registered.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_NE(manager.register_thread_manager(std::this_thread::get_id()), nullptr);
  ASSERT_EQ(manager.finalize_thread_registration(), 0);

  TakeoverProbe replacement(&allocator);
  EXPECT_EQ(manager.register_session(client_id, &replacement), MQ_ERR_TIMEOUT_V2);
  EXPECT_FALSE(original->called.load());

  stop.store(true);
  owner.join();
  EXPECT_FALSE(original->called.load());
  auto* owner_manager = manager.find_client_manager(client_id);
  ASSERT_NE(owner_manager, nullptr);
  EXPECT_EQ(owner_manager->get_thread_id(), owner_id);
  delete original;
}

namespace {

class HoldingProbe : public mqtt::MQTTProtocolHandler
{
 public:
  explicit HoldingProbe(MQTTAllocator* allocator) : mqtt::MQTTProtocolHandler(allocator) {}

  void request_session_takeover(const std::shared_ptr<mqtt::SessionTakeoverGate>& gate) override
  {
    calls.fetch_add(1);
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (gate) {
      gate->done.store(true, std::memory_order_release);
    }
  }

  std::atomic<int> calls{0};
  std::atomic<bool> release{false};
};

}  // namespace

TEST(Phase0Takeover, ReservationBlocksASecondClaim)
{
  MQTTAllocator allocator("phase0_takeover_reserve", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  const mqtt::MQTTString client_id = mqtt::to_mqtt_string("reserved-client", &allocator);
  mqtt::GlobalSessionManager manager;
  ASSERT_EQ(manager.pre_register_threads(3, 8), 0);

  std::atomic<int> ready{0};
  std::atomic<bool> stop{false};
  HoldingProbe* original = nullptr;
  std::thread owner([&] {
    ASSERT_NE(manager.register_thread_manager(std::this_thread::get_id()), nullptr);
    original = new HoldingProbe(&allocator);
    ASSERT_EQ(manager.register_session(client_id, original), 0);
    ready.fetch_add(1);
    while (!stop.load()) {
      if (auto* local = manager.get_thread_manager()) {
        local->process_control_messages();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  std::atomic<int> first_result{-1};
  std::atomic<int> second_result{-1};
  TakeoverProbe first_replacement(&allocator);
  TakeoverProbe second_replacement(&allocator);
  std::thread first([&] {
    ASSERT_NE(manager.register_thread_manager(std::this_thread::get_id()), nullptr);
    ready.fetch_add(1);
    while (ready.load() < 4) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    first_result.store(manager.register_session(client_id, &first_replacement));
    while (!stop.load()) {
      if (auto* local = manager.get_thread_manager()) {
        local->process_control_messages();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  std::thread second([&] {
    ASSERT_NE(manager.register_thread_manager(std::this_thread::get_id()), nullptr);
    ready.fetch_add(1);
    while (ready.load() < 4) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    second_result.store(manager.register_session(client_id, &second_replacement));
    // 抢到会话的线程就是下一次接管的主人，必须继续处理控制消息。
    while (!stop.load()) {
      if (auto* local = manager.get_thread_manager()) {
        local->process_control_messages();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  while (ready.load() < 3) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(manager.finalize_thread_registration(), 0);
  ready.fetch_add(1);

  const auto pause_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (original->calls.load() < 1 && std::chrono::steady_clock::now() < pause_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(original->calls.load(), 1);
  EXPECT_EQ(manager.find_client_manager(client_id)->get_thread_id(), owner.get_id());

  original->release.store(true);
  const auto done_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while ((first_result.load() != 0 || second_result.load() != 0) &&
         std::chrono::steady_clock::now() < done_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(first_result.load(), 0);
  EXPECT_EQ(second_result.load(), 0);
  EXPECT_EQ(original->calls.load(), 1);

  stop.store(true);
  first.join();
  second.join();
  owner.join();
  delete original;
}

TEST(Phase0Flow, PacketIdsSkipInflightAndWrap)
{
  mqtt::ConnectionFlow flow;
  flow.observe_client(2, 1024);
  const auto first = flow.allocate_outbound(nullptr, nullptr);
  const auto second = flow.allocate_outbound(nullptr, nullptr);
  const auto blocked = flow.allocate_outbound(nullptr, nullptr);
  ASSERT_EQ(first.status, mqtt::ConnectionFlow::AllocateStatus::Ready);
  ASSERT_EQ(first.id, 1);
  ASSERT_EQ(second.id, 2);
  EXPECT_EQ(blocked.status, mqtt::ConnectionFlow::AllocateStatus::WindowFull);
  EXPECT_EQ(flow.outbound_inflight(), 2);

  flow.release_outbound(1);
  const auto third = flow.allocate_outbound(nullptr, nullptr);
  EXPECT_EQ(third.status, mqtt::ConnectionFlow::AllocateStatus::Ready);
  EXPECT_EQ(third.id, 3);
  EXPECT_FALSE(flow.outbound_holds(1));

  mqtt::ConnectionFlow wide;
  wide.observe_client(65535, 268435455);
  uint16_t oldest = 0;
  for (int i = 0; i < 65535; ++i) {
    const auto allocated = wide.allocate_outbound(nullptr, nullptr);
    ASSERT_EQ(allocated.status, mqtt::ConnectionFlow::AllocateStatus::Ready) << i;
    ASSERT_EQ(allocated.id, static_cast<uint16_t>(i + 1));
    if (i == 0) {
      oldest = allocated.id;
    }
  }
  EXPECT_EQ(wide.allocate_outbound(nullptr, nullptr).status,
            mqtt::ConnectionFlow::AllocateStatus::WindowFull);
  wide.release_outbound(oldest);
  const auto reused = wide.allocate_outbound(nullptr, nullptr);
  EXPECT_EQ(reused.status, mqtt::ConnectionFlow::AllocateStatus::Ready);
  EXPECT_EQ(reused.id, oldest);
  EXPECT_EQ(wide.allocate_outbound(nullptr, nullptr).status,
            mqtt::ConnectionFlow::AllocateStatus::WindowFull);
}

TEST(Phase0Flow, OutboundAllocationSkipsIdsHeldElsewhere)
{
  mqtt::ConnectionFlow flow;
  const auto skipped = flow.allocate_outbound(
      [](uint16_t id, void*) { return id == 1 ? 1 : 0; }, nullptr);
  EXPECT_EQ(skipped.status, mqtt::ConnectionFlow::AllocateStatus::Ready);
  EXPECT_EQ(skipped.id, 2);
  EXPECT_FALSE(flow.outbound_holds(1));

  const auto busy = flow.allocate_outbound(
      [](uint16_t, void*) { return 2; }, nullptr);
  EXPECT_EQ(busy.status, mqtt::ConnectionFlow::AllocateStatus::Busy);
  EXPECT_EQ(flow.outbound_inflight(), 1);
}

TEST(Phase0Flow, InboundQos2DedupAndReceiveMaximum)
{
  mqtt::ConnectionFlow flow;
  flow.set_server_limits(128, 1, 2);
  EXPECT_FALSE(flow.inbound_too_large(128));
  EXPECT_TRUE(flow.inbound_too_large(129));
  EXPECT_EQ(flow.admit_qos2(7), mqtt::ConnectionFlow::Qos2Admit::Forward);
  EXPECT_EQ(flow.admit_qos2(7), mqtt::ConnectionFlow::Qos2Admit::Duplicate);
  EXPECT_EQ(flow.admit_qos2(8), mqtt::ConnectionFlow::Qos2Admit::Exceeded);
  EXPECT_EQ(flow.inbound_qos2_waiting(), 1);
  flow.release_qos2(7);
  EXPECT_EQ(flow.admit_qos2(8), mqtt::ConnectionFlow::Qos2Admit::Forward);
  flow.observe_client(1, 100);
  EXPECT_TRUE(flow.outbound_too_large(101));
  EXPECT_FALSE(flow.outbound_too_large(100));
  EXPECT_EQ(flow.max_qos(), 2);
}

TEST(Phase0Flow, AckMustMatchOutboundPhase)
{
  mqtt::ConnectionFlow flow;
  const auto qos1 = flow.allocate_outbound(nullptr, nullptr, 1);
  const auto qos2 = flow.allocate_outbound(nullptr, nullptr, 2);
  ASSERT_EQ(qos1.status, mqtt::ConnectionFlow::AllocateStatus::Ready);
  ASSERT_EQ(qos2.status, mqtt::ConnectionFlow::AllocateStatus::Ready);

  EXPECT_EQ(flow.classify_outbound_ack(qos2.id, mqtt::ConnectionFlow::AckKind::PubAck),
            mqtt::ConnectionFlow::AckDisposition::Unexpected);
  EXPECT_TRUE(flow.outbound_holds(qos2.id));
  EXPECT_EQ(flow.classify_outbound_ack(qos1.id, mqtt::ConnectionFlow::AckKind::PubComp),
            mqtt::ConnectionFlow::AckDisposition::Unexpected);
  EXPECT_TRUE(flow.outbound_holds(qos1.id));

  EXPECT_EQ(flow.classify_outbound_ack(qos2.id, mqtt::ConnectionFlow::AckKind::PubComp),
            mqtt::ConnectionFlow::AckDisposition::Unexpected);
  EXPECT_EQ(flow.classify_outbound_ack(qos2.id, mqtt::ConnectionFlow::AckKind::PubRec),
            mqtt::ConnectionFlow::AckDisposition::Continue);
  flow.mark_awaiting_pubcomp(qos2.id);
  EXPECT_EQ(flow.classify_outbound_ack(qos2.id, mqtt::ConnectionFlow::AckKind::PubComp),
            mqtt::ConnectionFlow::AckDisposition::Complete);
  flow.release_outbound(qos2.id);
  EXPECT_FALSE(flow.outbound_holds(qos2.id));

  EXPECT_EQ(flow.classify_outbound_ack(qos1.id, mqtt::ConnectionFlow::AckKind::PubAck),
            mqtt::ConnectionFlow::AckDisposition::Complete);
  flow.release_outbound(qos1.id);
  EXPECT_FALSE(flow.outbound_holds(qos1.id));
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
