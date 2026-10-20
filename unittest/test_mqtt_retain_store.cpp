#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_retain_store.h"
#include "mqtt_stl_allocator.h"

namespace {

using clock_type = std::chrono::steady_clock;

mqtt::PublishPacket publish(MQTTAllocator* allocator, const std::string& topic, const std::string& payload,
                            uint8_t qos = 0)
{
  mqtt::PublishPacket packet(allocator);
  packet.retain = true;
  packet.qos = qos;
  packet.topic_name = mqtt::to_mqtt_string(topic, allocator);
  packet.payload = mqtt::to_mqtt_bytes(std::vector<uint8_t>(payload.begin(), payload.end()), allocator);
  return packet;
}

mqtt::RetainConfig limits_of(size_t messages, size_t payload, size_t total)
{
  mqtt::RetainConfig limits;
  limits.max_messages = messages;
  limits.max_payload_bytes = payload;
  limits.max_total_bytes = total;
  limits.max_deliver_per_subscribe = 1000;
  return limits;
}

std::vector<std::string> topics_of(const mqtt::RetainMatchResult& matched)
{
  std::vector<std::string> topics;
  for (size_t i = 0; i < matched.messages.size(); ++i) {
    topics.push_back(mqtt::from_mqtt_string(matched.messages[i].content->topic_name));
  }
  return topics;
}

}  // namespace

TEST(RetainHandling, ZeroAlwaysOneIfNewTwoNever)
{
  EXPECT_TRUE(mqtt::retain_handling_delivers(0, false));
  EXPECT_TRUE(mqtt::retain_handling_delivers(0, true));
  EXPECT_TRUE(mqtt::retain_handling_delivers(1, false));
  EXPECT_FALSE(mqtt::retain_handling_delivers(1, true));
  EXPECT_FALSE(mqtt::retain_handling_delivers(2, false));
  EXPECT_FALSE(mqtt::retain_handling_delivers(2, true));
}

TEST(RetainStore, ReplaceDeleteMatchAndDollar)
{
  MQTTAllocator allocator("retain-unit", MQTTMemoryTag::MEM_TAG_RETAIN, 0);
  mqtt::RetainStore store(&allocator);
  const mqtt::MQTTString sender = mqtt::to_mqtt_string("pub", &allocator);
  const mqtt::RetainConfig limits = limits_of(100, 1000, 100000);
  const auto now = clock_type::time_point(std::chrono::seconds(10));

  mqtt::PublishPacket first = publish(&allocator, "a/b", "one");
  const mqtt::RetainWriteResult stored = store.store(first, sender, limits, now);
  EXPECT_EQ(stored.kind, mqtt::RetainWriteResult::Stored);
  EXPECT_EQ(stored.message_delta, 1);
  EXPECT_EQ(store.message_count(), 1u);
  EXPECT_EQ(store.total_bytes(), 3u);

  mqtt::PublishPacket replacement = publish(&allocator, "a/b", "two!", 1);
  const mqtt::RetainWriteResult replaced = store.store(replacement, sender, limits, now);
  EXPECT_EQ(replaced.kind, mqtt::RetainWriteResult::Replaced);
  EXPECT_EQ(replaced.message_delta, 0);
  EXPECT_EQ(replaced.byte_delta, 1);
  EXPECT_EQ(store.total_bytes(), 4u);

  store.store(publish(&allocator, "a/c", "c"), sender, limits, now);
  store.store(publish(&allocator, "a/b/c", "deep"), sender, limits, now);
  store.store(publish(&allocator, "sport", "root"), sender, limits, now);
  store.store(publish(&allocator, "sport/tennis", "ball"), sender, limits, now);
  store.store(publish(&allocator, "$sys/a", "secret"), sender, limits, now);
  store.store(publish(&allocator, "a/$b", "money"), sender, limits, now);

  const mqtt::RetainMatchResult plus = store.match("a/+", 10, now);
  EXPECT_EQ(topics_of(plus), (std::vector<std::string>{"a/$b", "a/b", "a/c"}));
  EXPECT_FALSE(plus.truncated);
  EXPECT_EQ(plus.messages[1].content->qos, 1);
  EXPECT_TRUE(plus.messages[1].content->retain);

  const mqtt::RetainMatchResult hash = store.match("a/#", 10, now);
  EXPECT_EQ(topics_of(hash), (std::vector<std::string>{"a/$b", "a/b", "a/b/c", "a/c"}));

  const mqtt::RetainMatchResult sport = store.match("sport/#", 10, now);
  EXPECT_EQ(topics_of(sport), (std::vector<std::string>{"sport", "sport/tennis"}));

  const mqtt::RetainMatchResult root = store.match("#", 10, now);
  const std::vector<std::string> root_topics = topics_of(root);
  EXPECT_EQ(std::find(root_topics.begin(), root_topics.end(), "$sys/a"), root_topics.end());
  EXPECT_NE(std::find(root_topics.begin(), root_topics.end(), "a/b"), root_topics.end());

  const mqtt::RetainMatchResult single = store.match("+", 10, now);
  const std::vector<std::string> single_topics = topics_of(single);
  EXPECT_EQ(std::find(single_topics.begin(), single_topics.end(), "$sys"), single_topics.end());
  EXPECT_EQ(std::find(single_topics.begin(), single_topics.end(), "$sys/a"), single_topics.end());

  const mqtt::RetainMatchResult system = store.match("$sys/#", 10, now);
  EXPECT_EQ(topics_of(system), (std::vector<std::string>{"$sys/a"}));

  mqtt::PublishPacket clear = publish(&allocator, "a/c", "");
  const mqtt::RetainWriteResult removed = store.store(clear, sender, limits, now);
  EXPECT_EQ(removed.kind, mqtt::RetainWriteResult::Removed);
  EXPECT_EQ(topics_of(store.match("a/c", 10, now)).size(), 0u);

  const mqtt::RetainMatchResult truncated = store.match("#", 1, now);
  EXPECT_TRUE(truncated.truncated);
  ASSERT_EQ(truncated.messages.size(), 1u);
  EXPECT_EQ(mqtt::from_mqtt_string(truncated.messages[0].content->topic_name), "a/$b");
}

TEST(RetainStore, ExpiryAndLimits)
{
  MQTTAllocator allocator("retain-limits", MQTTMemoryTag::MEM_TAG_RETAIN, 0);
  mqtt::RetainStore store(&allocator);
  const mqtt::MQTTString sender = mqtt::to_mqtt_string("pub", &allocator);
  mqtt::RetainConfig limits = limits_of(2, 4, 6);
  const auto now = clock_type::time_point(std::chrono::seconds(50));

  mqtt::PublishPacket expiring = publish(&allocator, "t/exp", "x");
  expiring.properties.has_message_expiry_interval = true;
  expiring.properties.message_expiry_interval = 5;
  EXPECT_EQ(store.store(expiring, sender, limits, now).kind, mqtt::RetainWriteResult::Stored);
  EXPECT_EQ(store.match("t/exp", 10, now + std::chrono::seconds(4)).messages.size(), 1u);
  const mqtt::RetainMatchResult expired = store.match("t/exp", 10, now + std::chrono::seconds(5));
  EXPECT_TRUE(expired.messages.empty());
  EXPECT_EQ(expired.message_delta, -1);
  EXPECT_EQ(store.message_count(), 0u);

  mqtt::PublishPacket immediate = publish(&allocator, "t/now", "x");
  immediate.properties.has_message_expiry_interval = true;
  immediate.properties.message_expiry_interval = 0;
  EXPECT_EQ(store.store(immediate, sender, limits, now).kind, mqtt::RetainWriteResult::Absent);

  EXPECT_EQ(store.store(publish(&allocator, "t/a", "aa"), sender, limits, now).kind,
            mqtt::RetainWriteResult::Stored);
  EXPECT_EQ(store.store(publish(&allocator, "t/b", "bb"), sender, limits, now).kind,
            mqtt::RetainWriteResult::Stored);
  const mqtt::RetainWriteResult count = store.store(publish(&allocator, "t/c", "c"), sender, limits, now);
  EXPECT_EQ(count.kind, mqtt::RetainWriteResult::Rejected);
  EXPECT_STREQ(count.warn, "retained message count limit");
  EXPECT_EQ(store.message_count(), 2u);
  EXPECT_TRUE(store.store(publish(&allocator, "t/c", "c"), sender, limits, now).warn == nullptr);

  mqtt::RetainConfig bytes = limits_of(10, 4, 3);
  MQTTAllocator allocator2("retain-bytes", MQTTMemoryTag::MEM_TAG_RETAIN, 0);
  mqtt::RetainStore byte_store(&allocator2);
  EXPECT_EQ(byte_store.store(publish(&allocator2, "b/a", "abc"), sender, bytes, now).kind,
            mqtt::RetainWriteResult::Stored);
  EXPECT_EQ(byte_store.store(publish(&allocator2, "b/b", "z"), sender, bytes, now).kind,
            mqtt::RetainWriteResult::Rejected);
  ASSERT_EQ(byte_store.match("b/a", 1, now).messages.size(), 1u);
  EXPECT_EQ(byte_store.match("b/a", 1, now).messages[0].content->payload.size(), 3u);

  const mqtt::RetainWriteResult shrunk =
      byte_store.store(publish(&allocator2, "b/a", "z"), sender, bytes, now);
  EXPECT_EQ(shrunk.kind, mqtt::RetainWriteResult::Replaced);
  EXPECT_EQ(byte_store.total_bytes(), 1u);
  EXPECT_EQ(byte_store.store(publish(&allocator2, "b/a", "abcd"), sender, bytes, now).kind,
            mqtt::RetainWriteResult::Rejected);
  EXPECT_EQ(byte_store.total_bytes(), 1u);
  EXPECT_EQ(byte_store.store(publish(&allocator2, "b/wide", "12345"), sender, bytes, now).kind,
            mqtt::RetainWriteResult::Rejected);
}

TEST(RetainStore, LeadingEmptyLevelImmediateExpiryAndQuota)
{
  MQTTAllocator allocator("retain-slash", MQTTMemoryTag::MEM_TAG_RETAIN, 0);
  mqtt::RetainStore store(&allocator);
  const mqtt::MQTTString sender = mqtt::to_mqtt_string("pub", &allocator);
  const mqtt::RetainConfig limits = limits_of(10, 100, 1000);
  const auto now = clock_type::time_point(std::chrono::seconds(10));

  mqtt::PublishPacket slash = publish(&allocator, "/", "root");
  mqtt::PublishPacket slash_foo = publish(&allocator, "/foo", "bar");
  slash_foo.properties.has_message_expiry_interval = true;
  slash_foo.properties.message_expiry_interval = 2;
  EXPECT_EQ(store.store(slash, sender, limits, now).kind, mqtt::RetainWriteResult::Stored);
  EXPECT_EQ(store.store(slash_foo, sender, limits, now).kind, mqtt::RetainWriteResult::Stored);
  EXPECT_EQ(topics_of(store.match("/", 10, now)), (std::vector<std::string>{"/"}));
  EXPECT_EQ(topics_of(store.match("/foo", 10, now)), (std::vector<std::string>{"/foo"}));
  EXPECT_EQ(topics_of(store.match("+/+", 10, now)), (std::vector<std::string>{"/", "/foo"}));
  EXPECT_EQ(topics_of(store.match("#", 10, now)), (std::vector<std::string>{"/", "/foo"}));

  const mqtt::RetainMatchResult expired = store.match("/foo", 10, now + std::chrono::seconds(2));
  EXPECT_TRUE(expired.messages.empty());
  EXPECT_EQ(expired.message_delta, -1);
  EXPECT_EQ(store.message_count(), 1u);
  EXPECT_EQ(topics_of(store.match("#", 10, now + std::chrono::seconds(2))),
            (std::vector<std::string>{"/"}));

  mqtt::PublishPacket immediate = publish(&allocator, "/", "gone");
  immediate.properties.has_message_expiry_interval = true;
  immediate.properties.message_expiry_interval = 0;
  EXPECT_EQ(store.store(immediate, sender, limits, now).kind, mqtt::RetainWriteResult::Removed);
  EXPECT_EQ(store.message_count(), 0u);
  EXPECT_TRUE(store.match("/", 10, now).messages.empty());

  mqtt::RetainConfig tight = limits_of(1, 100, 1000);
  mqtt::PublishPacket short_lived = publish(&allocator, "quota/old", "x");
  short_lived.properties.has_message_expiry_interval = true;
  short_lived.properties.message_expiry_interval = 1;
  EXPECT_EQ(store.store(short_lived, sender, tight, now).kind, mqtt::RetainWriteResult::Stored);
  const mqtt::RetainWriteResult freed =
      store.store(publish(&allocator, "quota/new", "y"), sender, tight, now + std::chrono::seconds(1));
  EXPECT_EQ(freed.kind, mqtt::RetainWriteResult::Stored);
  EXPECT_EQ(freed.message_delta, 0);
  EXPECT_EQ(store.message_count(), 1u);
  EXPECT_EQ(topics_of(store.match("quota/+", 10, now + std::chrono::seconds(1))),
            (std::vector<std::string>{"quota/new"}));
}

TEST(RetainStore, ConcurrentReadersAndWriters)
{
  MQTTAllocator allocator("retain-race", MQTTMemoryTag::MEM_TAG_RETAIN, 0);
  mqtt::RetainStore store(&allocator);
  const mqtt::MQTTString sender = mqtt::to_mqtt_string("pub", &allocator);
  const mqtt::RetainConfig limits = limits_of(1000, 100, 100000);
  std::atomic<bool> failed(false);
  std::vector<std::thread> threads;
  for (int id = 0; id < 4; ++id) {
    threads.push_back(std::thread([&, id]() {
      try {
        for (int i = 0; i < 100; ++i) {
          const std::string topic = "c/" + std::to_string((id + i) % 8);
          const auto now = clock_type::now();
          if (i % 11 == 0) {
            store.store(publish(&allocator, topic, ""), sender, limits, now);
          } else {
            store.store(publish(&allocator, topic, "v" + std::to_string(i)), sender, limits, now);
          }
          const mqtt::RetainMatchResult matched = store.match("c/+", 20, now);
          if (matched.messages.size() > 8) {
            failed.store(true);
          }
        }
      } catch (...) {
        failed.store(true);
      }
    }));
  }
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].join();
  }
  EXPECT_FALSE(failed.load());
  EXPECT_LE(store.message_count(), 8u);
}
