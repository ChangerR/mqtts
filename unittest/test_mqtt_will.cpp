#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <thread>
#include "mqtt_allocator.h"
#include "mqtt_packet.h"
#include "mqtt_will.h"

TEST(WillDelay, UsesSessionExpiryOnlyWhenPersistent)
{
  EXPECT_EQ(mqtt::effective_will_delay_seconds(false, 30, 60), 0u);
  EXPECT_EQ(mqtt::effective_will_delay_seconds(true, 30, 60), 30u);
  EXPECT_EQ(mqtt::effective_will_delay_seconds(true, 30, 2), 2u);
  EXPECT_EQ(mqtt::effective_will_delay_seconds(true, 5, 0), 0u);
}

TEST(WillConnect, RejectsQosRetainAndWildcards)
{
  const mqtt::WillConnectDecision ok = mqtt::validate_will_connect(1, false, "a/b", 1, true);
  EXPECT_TRUE(ok.accept);
  EXPECT_EQ(ok.reason, mqtt::ReasonCode::Success);

  const mqtt::WillConnectDecision qos = mqtt::validate_will_connect(2, false, "a/b", 1, true);
  EXPECT_FALSE(qos.accept);
  EXPECT_EQ(qos.reason, mqtt::ReasonCode::QoSNotSupported);

  const mqtt::WillConnectDecision retain = mqtt::validate_will_connect(0, true, "a/b", 2, false);
  EXPECT_FALSE(retain.accept);
  EXPECT_EQ(retain.reason, mqtt::ReasonCode::RetainNotSupported);

  const mqtt::WillConnectDecision plus = mqtt::validate_will_connect(0, false, "a/+/b", 2, true);
  EXPECT_EQ(plus.reason, mqtt::ReasonCode::ProtocolError);
  const mqtt::WillConnectDecision hash = mqtt::validate_will_connect(0, false, "bad/#", 2, true);
  EXPECT_EQ(hash.reason, mqtt::ReasonCode::ProtocolError);
  const mqtt::WillConnectDecision empty = mqtt::validate_will_connect(0, false, "", 2, true);
  EXPECT_EQ(empty.reason, mqtt::ReasonCode::ProtocolError);
}

namespace {

std::unique_ptr<mqtt::WillScheduler::Entry> entry_for(const std::string& client_id,
                                                      const std::string& forward_id,
                                                      std::chrono::steady_clock::time_point due)
{
  std::unique_ptr<mqtt::WillScheduler::Entry> entry(new mqtt::WillScheduler::Entry());
  entry->client_id = client_id;
  entry->forward_id = forward_id;
  entry->due = due;
  entry->owner = std::this_thread::get_id();
  entry->body.topic = "will/unit";
  entry->body.payload.push_back('x');
  return entry;
}

}  // namespace

TEST(WillScheduler, CancelDropsAndCleanStartFires)
{
  mqtt::WillScheduler scheduler;
  const auto later = std::chrono::steady_clock::now() + std::chrono::hours(1);
  scheduler.schedule(entry_for("same", "same", later));
  EXPECT_TRUE(scheduler.generation_matches("same", 1));

  const auto waiting =
      scheduler.take_due(std::this_thread::get_id(), std::chrono::steady_clock::now(), false);
  EXPECT_TRUE(waiting.empty());

  scheduler.observe_connect("same", false);
  EXPECT_FALSE(scheduler.generation_matches("same", 1));
  const auto cancelled = scheduler.take_due(
      std::this_thread::get_id(), std::chrono::steady_clock::now() + std::chrono::hours(2), true);
  EXPECT_TRUE(cancelled.empty());

  scheduler.schedule(entry_for("same", "transport-id", later));
  scheduler.observe_connect("same", true);
  const auto fired =
      scheduler.take_due(std::this_thread::get_id(), std::chrono::steady_clock::now(), false);
  ASSERT_EQ(fired.size(), 1u);
  EXPECT_EQ(fired[0]->forward_id, "transport-id");
  EXPECT_TRUE(scheduler.generation_matches("same", fired[0]->generation));
}

TEST(WillScheduler, AlreadyDueWillSurvivesSessionResume)
{
  mqtt::WillScheduler scheduler;
  const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  scheduler.schedule(entry_for("due", "due", past));
  scheduler.observe_connect("due", false, true);
  EXPECT_TRUE(scheduler.generation_matches("due", 1));
  const auto fired =
      scheduler.take_due(std::this_thread::get_id(), std::chrono::steady_clock::now(), false);
  ASSERT_EQ(fired.size(), 1u);
  EXPECT_EQ(fired[0]->client_id, "due");
}

TEST(WillScheduler, EndedSessionPublishesWillImmediately)
{
  mqtt::WillScheduler scheduler;
  const auto later = std::chrono::steady_clock::now() + std::chrono::hours(1);
  scheduler.schedule(entry_for("gone", "gone", later));
  scheduler.observe_connect("gone", false, false);
  const auto fired =
      scheduler.take_due(std::this_thread::get_id(), std::chrono::steady_clock::now(), false);
  ASSERT_EQ(fired.size(), 1u);
  EXPECT_EQ(fired[0]->forward_id, "gone");
}

TEST(WillScheduler, CleanStartKeepsPredecessorBesideNewWill)
{
  mqtt::WillScheduler scheduler;
  const auto later = std::chrono::steady_clock::now() + std::chrono::hours(1);
  scheduler.schedule(entry_for("swap", "old-transport", later));
  scheduler.observe_connect("swap", true);
  const auto inflight =
      scheduler.take_due(std::this_thread::get_id(), std::chrono::steady_clock::now(), false);
  ASSERT_EQ(inflight.size(), 1u);
  const uint64_t predecessor = inflight[0]->generation;

  scheduler.schedule(entry_for("swap", "new-transport", later));
  EXPECT_TRUE(scheduler.generation_matches("swap", predecessor));
  const auto waiting =
      scheduler.take_due(std::this_thread::get_id(), std::chrono::steady_clock::now(), false);
  EXPECT_TRUE(waiting.empty());
  scheduler.complete("swap", predecessor);
  EXPECT_FALSE(scheduler.generation_matches("swap", predecessor));
  EXPECT_TRUE(scheduler.generation_matches("swap", 2));
}

TEST(WillScheduler, ReclaimsClientIdWithoutAWill)
{
  mqtt::WillScheduler scheduler;
  scheduler.observe_connect("fresh", false);
  scheduler.schedule(entry_for("fresh", "fresh", std::chrono::steady_clock::now()));
  EXPECT_TRUE(scheduler.generation_matches("fresh", 1));
  const auto taken =
      scheduler.take_due(std::this_thread::get_id(), std::chrono::steady_clock::now(), false);
  ASSERT_EQ(taken.size(), 1u);
  scheduler.complete("fresh", taken[0]->generation);
  EXPECT_FALSE(scheduler.generation_matches("fresh", 1));
  scheduler.schedule(entry_for("fresh", "fresh", std::chrono::steady_clock::now()));
  EXPECT_TRUE(scheduler.generation_matches("fresh", 1));
}

TEST(WillScheduler, OtherThreadAndFlush)
{
  mqtt::WillScheduler scheduler;
  const auto later = std::chrono::steady_clock::now() + std::chrono::hours(1);
  scheduler.schedule(entry_for("owned", "owned", later));

  const auto foreign = scheduler.take_due(std::thread::id(), later + std::chrono::hours(1), true);
  EXPECT_TRUE(foreign.empty());
  EXPECT_TRUE(scheduler.generation_matches("owned", 1));

  const auto flushed =
      scheduler.take_due(std::this_thread::get_id(), std::chrono::steady_clock::now(), true);
  ASSERT_EQ(flushed.size(), 1u);
  EXPECT_EQ(flushed[0]->client_id, "owned");
}

TEST(WillBody, CopiesConnectFields)
{
  MQTTAllocator allocator("will-body", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::ConnectPacket packet(&allocator);
  packet.flags.will_flag = 1;
  packet.flags.will_qos = 1;
  packet.flags.will_retain = 1;
  packet.will_topic = mqtt::to_mqtt_string("will/topic", &allocator);
  packet.will_payload = mqtt::to_mqtt_string("hi", &allocator);
  packet.will_properties.will_delay_interval = 9;
  packet.will_properties.payload_format_indicator = 1;
  packet.will_properties.has_message_expiry_interval = true;
  packet.will_properties.message_expiry_interval = 12;
  packet.will_properties.content_type = mqtt::to_mqtt_string("text", &allocator);

  const mqtt::WillBody body = mqtt::will_body_from_connect(packet);
  EXPECT_EQ(body.topic, "will/topic");
  EXPECT_EQ(std::string(body.payload.begin(), body.payload.end()), "hi");
  EXPECT_EQ(body.qos, 1);
  EXPECT_TRUE(body.retain);
  EXPECT_EQ(body.delay_seconds, 9u);
  EXPECT_EQ(body.payload_format, 1);
  EXPECT_TRUE(body.has_message_expiry);
  EXPECT_EQ(body.message_expiry, 12u);
  EXPECT_EQ(body.content_type, "text");
}
