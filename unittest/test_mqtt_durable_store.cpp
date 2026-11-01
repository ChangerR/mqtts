#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include "mqtt_append_log.h"
#include "mqtt_durable_store.h"
using mqtt::DurableStore;

static std::string checked(const std::string& data)
{
  mqtt::journal::Encoder e;
  e.u32(mqtt::journal::checksum(data.data(), data.size()));
  return e.data + data;
}

static void test_live_packet_id_is_shared(const std::string& path)
{
  mqtt::PersistenceConfig cfg;
  cfg.path = path;
  cfg.partitions = 1;
  cfg.segment_bytes = 8192;
  cfg.max_disk_bytes = 1024 * 1024;
  cfg.checkpoint_interval_ms = 600000;
  DurableStore store(cfg);
  auto session = store.connect("reader", "owner", false, 60);
  assert(session.ok);
  assert(store.subscribe("reader", session.epoch, "topic", 1).ok);
  assert(store.claim_live_packet_id("reader", session.epoch, 1) ==
         DurableStore::PacketIdClaim::Reserved);
  assert(store.claim_live_packet_id("reader", session.epoch, 1) ==
         DurableStore::PacketIdClaim::InUse);
  assert(store.publish("topic", "payload", "writer", 0).ok);
  auto batch = store.fetch("reader", session.epoch, 0, 32);
  assert(batch.ok && batch.deliveries.size() == 1);
  assert(batch.deliveries[0].packet_id == 2);
  assert(store.claim_live_packet_id("reader", session.epoch, 2) ==
         DurableStore::PacketIdClaim::InUse);
  store.release_live_packet_id("reader", session.epoch, 1);
  assert(store.claim_live_packet_id("reader", session.epoch, 1) ==
         DurableStore::PacketIdClaim::Reserved);
  store.release_live_packet_id("reader", session.epoch, 1);
  assert(store.claim_live_packet_id("reader", session.epoch, 3) ==
         DurableStore::PacketIdClaim::Reserved);
  assert(store.disconnect("reader", session.epoch).ok);
  auto again = store.connect("reader", "owner", false, 60);
  assert(again.ok);
  assert(store.claim_live_packet_id("reader", again.epoch, 3) ==
         DurableStore::PacketIdClaim::Reserved);
  store.release_live_packet_id("reader", again.epoch, 3);
}

static void test_compaction(const std::string& path)
{
  mqtt::PersistenceConfig cfg;
  cfg.path = path;
  cfg.partitions = 1;
  cfg.segment_bytes = 8192;
  cfg.max_disk_bytes = 1024 * 1024;
  cfg.checkpoint_interval_ms = 600000;
  {
    DurableStore store(cfg);
    auto slow = store.connect("slow", "owner", false, 60);
    auto fast = store.connect("fast", "owner", false, 60);
    assert(store.subscribe("slow", slow.epoch, "slow", 1).ok);
    assert(store.subscribe("fast", fast.epoch, "fast", 1).ok);
    assert(store.disconnect("slow", slow.epoch).ok);
    // Sparse pending records share segments with much larger, consumed records.
    // Without live-record compaction this exhausts the 384 KiB partition quota.
    for (int i = 0; i < 200; ++i) {
      assert(store.publish("slow", "pending-" + std::to_string(i), "writer", 0).ok);
      assert(store.publish("fast", std::string(7000, 'x'), "writer", 0).ok);
      auto batch = store.fetch("fast", fast.epoch, 0, 32);
      assert(batch.ok && batch.deliveries.size() == 1);
      assert(store.acknowledge("fast", fast.epoch, batch.deliveries[0].packet_id).ok);
      if (i % 10 == 9)
        store.checkpoint();
    }
    auto stats = store.statistics();
    assert(stats.pending == 200 && stats.bytes == stats.unique_bytes);
    assert(stats.message_partition_bytes[0] == 0 && stats.checkpoint_bytes < 16000);
  }
  {
    DurableStore store(cfg);
    auto slow = store.connect("slow", "owner", false, 60);
    assert(slow.present && store.statistics().pending == 200);
    size_t count = 0;
    while (store.statistics().pending) {
      auto batch = store.fetch("slow", slow.epoch, 0, 32);
      assert(batch.ok && !batch.deliveries.empty());
      for (const auto& d : batch.deliveries) {
        assert(d.wire == "pending-" + std::to_string(count++) && d.dup);
        assert(store.acknowledge("slow", slow.epoch, d.packet_id).ok);
      }
    }
    assert(count == 200 && store.statistics().unique_bytes == 0);
  }
}

static void test_native_upgrade(const std::string& path, uint32_t version)
{
  using namespace mqtt::journal;
  mqtt::PersistenceConfig cfg;
  cfg.path = path;
  cfg.partitions = 1;
  cfg.checkpoint_interval_ms = 600000;
  ensure_directory(path);
  Encoder format;
  format.text("MQTTS-PARTITION-LOG");
  format.u32(version);
  format.u32(1);
  atomic_file(path + "/FORMAT", checked(format.data));
  Location location;
  {
    AppendLog log(path + "/messages-0", cfg.segment_bytes, cfg.max_disk_bytes / 4);
    Encoder message;
    message.u8(8);
    message.u64(2);
    message.u64(0);
    message.text("v2-payload");
    message.u32(1);
    message.text("legacy");
    message.u64(1);
    message.u32(7);
    auto appended = log.append(message.data, [&](bool ok, const Location& p) {
      assert(ok);
      location = p;
    });
    assert(appended->wait());
    assert(log.seal()->wait());
  }
  Encoder checkpoint;
  checkpoint.text("MQTTS-CHECKPOINT-" + std::to_string(version));
  checkpoint.u32(1);
  checkpoint.u64(2);
  checkpoint.u64(1);
  checkpoint.u64(0);
  checkpoint.u32(1);
  checkpoint.u64(2);
  checkpoint.u32(0);
  if (version == 2) {
    checkpoint.u64(location.segment);
    checkpoint.u64(location.offset);
    checkpoint.u64(location.serial);
    checkpoint.u32(location.size);
  } else {
    checkpoint.u64(0);
    checkpoint.text("v2-payload");
  }
  checkpoint.u32(1);
  checkpoint.text("legacy");
  checkpoint.text("owner");
  checkpoint.u64(1);
  checkpoint.u64(1);
  checkpoint.u32(60);
  checkpoint.u64(DurableStore::now_ms() + 60000);
  checkpoint.u8(0);
  checkpoint.u32(8);
  if (version >= 4)
    checkpoint.u64(0);
  checkpoint.u32(1);
  checkpoint.text("topic");
  checkpoint.u8(1);
  if (version >= 5) {
    checkpoint.u8(kStoredSubscriptionVersion);
    checkpoint.u8(1);
  }
  checkpoint.u32(1);
  checkpoint.u64(2);
  checkpoint.u32(7);
  atomic_file(path + "/CHECKPOINT", checked(checkpoint.data));
  {
    DurableStore store(cfg);
    assert(store.statistics().pending == 1 && store.statistics().unique_bytes == 10);
    store.checkpoint();
    assert(store.statistics().message_partition_bytes[0] == 0);
  }
  {
    auto marker = read_file(path + "/FORMAT", 4096).substr(4);
    Decoder d(marker);
    assert(d.text() == "MQTTS-PARTITION-LOG" && d.u32() == 6);
    DurableStore store(cfg);
    auto c = store.connect("legacy", "owner", false, 60);
    auto batch = store.fetch("legacy", c.epoch, 0, 32);
    assert(c.present && c.subscriptions.size() == 1);
    assert(c.subscriptions[0].first == "topic");
    assert(c.subscriptions[0].second.qos == 1);
    assert(!c.subscriptions[0].second.no_local);
    assert(!c.subscriptions[0].second.retain_as_published);
    assert(c.subscriptions[0].second.retain_handling == 0);
    assert(batch.deliveries.size() == 1);
    assert(batch.deliveries[0].wire == "v2-payload" && batch.deliveries[0].packet_id == 7);
    assert(store.retained_messages().empty());
  }
}

static std::vector<std::filesystem::path> log_files(const std::string& directory)
{
  std::vector<std::filesystem::path> logs;
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    if (entry.path().extension() == ".log")
      logs.push_back(entry.path());
  return logs;
}

static void test_retain_journal(const std::string& path)
{
  mqtt::PersistenceConfig cfg;
  cfg.path = path;
  cfg.partitions = 1;
  cfg.segment_bytes = 8192;
  cfg.max_disk_bytes = 1024 * 1024;
  cfg.checkpoint_interval_ms = 600000;
  const int64_t soon = DurableStore::now_ms() + 60000;
  {
    DurableStore store(cfg);
    assert(store.retained_messages().empty());
    DurableStore::RetainedProperties properties;
    properties.payload_format_indicator = 1;
    properties.content_type = "text/plain";
    properties.response_topic = "reply/to";
    properties.correlation_data = std::string("\0corr", 5);
    properties.user_properties.push_back(std::make_pair("k", "v"));
    assert(store.put_retained("keep/t", 1, "saved", "pub", 0, true, properties).ok);
    assert(store.put_retained("keep/async", 0, "later", "pub", 0, false).ok);
    assert(store.put_retained("keep/disk", 1, "qos1", "writer", soon, true).ok);
    assert(store.put_retained("keep/gone", 1, "temp", "pub", 0, true).ok);
    assert(store.clear_retained("keep/gone", true).ok);
    assert(store.put_retained("keep/expired", 1, "old", "pub", DurableStore::now_ms() - 1, true).ok);
    assert(store.statistics().retained == 3);
    auto session = store.connect("reader", "owner", false, 60);
    assert(session.ok);
    assert(store.subscribe("reader", session.epoch, "keep/#", 1).ok);
    auto other_epoch = store.enqueue_directed("reader", session.epoch + 1, "directed-wire", soon);
    assert(other_epoch.ok && other_epoch.stale && !other_epoch.present);
    auto queued = store.enqueue_directed("reader", session.epoch, "directed-wire", soon);
    assert(queued.ok && queued.present && queued.accepted_targets == 1);
    auto absent = store.enqueue_directed("missing", 1, "directed-wire", 0);
    assert(absent.ok && !absent.present);
    auto batch = store.fetch("reader", session.epoch, 0, 8);
    assert(batch.ok && batch.deliveries.size() == 1);
    assert(batch.deliveries[0].wire == "directed-wire");
    assert(batch.deliveries[0].expires == soon);
    assert(store.acknowledge("reader", session.epoch, batch.deliveries[0].packet_id).ok);
    store.checkpoint();
    assert(store.statistics().retain_log_bytes == 0);
    assert(store.statistics().retained == 3);
  }
  {
    DurableStore store(cfg);
    auto kept = store.retained_messages();
    assert(kept.size() == 3);
    bool saw_saved = false, saw_async = false, saw_disk = false, saw_gone = false;
    for (const auto& item : kept) {
      if (item.topic == "keep/t") {
        saw_saved = item.payload == "saved" && item.qos == 1 && item.sender == "pub" &&
                    item.expires_ms == 0 && item.properties.payload_format_indicator == 1 &&
                    item.properties.content_type == "text/plain" &&
                    item.properties.response_topic == "reply/to" &&
                    item.properties.correlation_data == std::string("\0corr", 5) &&
                    item.properties.user_properties.size() == 1 &&
                    item.properties.user_properties[0].first == "k" &&
                    item.properties.user_properties[0].second == "v";
      } else if (item.topic == "keep/async")
        saw_async = item.payload == "later" && item.qos == 0;
      else if (item.topic == "keep/disk")
        saw_disk = item.payload == "qos1" && item.expires_ms == soon;
      else if (item.topic == "keep/gone" || item.topic == "keep/expired")
        saw_gone = true;
    }
    assert(saw_saved && saw_async && saw_disk && !saw_gone);
    auto session = store.connect("reader", "owner", false, 60);
    assert(session.present);
    assert(store.fetch("reader", session.epoch, 0, 8).deliveries.empty());
    assert(store.statistics().pending == 0);
  }
  // A complete corrupt frame stays on disk. A torn tail is the incomplete
  // header/body case AppendLog already repairs.
  auto logs = log_files(path + "/retain");
  const bool added_tail = logs.empty();
  if (added_tail) {
    DurableStore store(cfg);
    assert(store.put_retained("keep/tail", 0, "body", "pub", 0, true).ok);
  }
  logs = log_files(path + "/retain");
  assert(logs.size() == 1);
  const auto good_size = std::filesystem::file_size(logs[0]);
  {
    std::ofstream out(logs[0], std::ios::binary | std::ios::app);
    out.write("torn", 4);
  }
  {
    DurableStore store(cfg);
    assert(store.retained_messages().size() == (added_tail ? 4u : 3u));
  }
  assert(std::filesystem::file_size(logs[0]) == good_size);
  {
    std::fstream file(logs[0], std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(24);
    file.put('!');
  }
  bool rejected = false;
  try {
    DurableStore store(cfg);
  } catch (const std::exception&) {
    rejected = true;
  }
  assert(rejected);
  assert(std::filesystem::file_size(logs[0]) == good_size);
}

static void test_subscription_options(const std::string& path)
{
  mqtt::PersistenceConfig cfg;
  cfg.path = path;
  cfg.partitions = 1;
  cfg.checkpoint_interval_ms = 600000;
  mqtt::journal::StoredSubscription rich;
  rich.qos = 1;
  rich.no_local = true;
  rich.retain_as_published = true;
  rich.retain_handling = 2;
  {
    DurableStore store(cfg);
    auto session = store.connect("reader", "owner", false, 60);
    assert(session.ok);
    assert(store.subscribe("reader", session.epoch, "room/#", rich).ok);
    assert(store.publish("room/one", "self", "reader", 0).targets.empty());
    assert(store.publish("room/one", "other", "writer", 0).targets.size() == 1);
    mqtt::journal::StoredSubscription plain;
    plain.qos = 1;
    assert(store.subscribe("reader", session.epoch, "room/+", plain).ok);
    assert(store.publish("room/one", "overlap", "reader", 0).targets.size() == 1);
    assert(store.unsubscribe("reader", session.epoch, "room/+").ok);
    assert(store.publish("room/one", "again", "reader", 0).targets.empty());
    mqtt::journal::StoredSubscription echo;
    echo.qos = 1;
    auto echo_session = store.connect("echo", "owner", false, 60);
    assert(store.subscribe("echo", echo_session.epoch, "echo", echo).ok);
    assert(store.publish("echo", "mine", "echo", 0).targets.size() == 1);
    assert(store.publish("echo", "mine", "echo", 0, true).targets.empty());
    mqtt::journal::StoredSubscription qos0 = rich;
    qos0.qos = 0;
    assert(store.subscribe("reader", session.epoch, "room/qos0", qos0).ok);
    store.checkpoint();
  }
  {
    DurableStore store(cfg);
    auto session = store.connect("reader", "owner", false, 60);
    assert(session.present);
    const mqtt::journal::StoredSubscription* kept = nullptr;
    const mqtt::journal::StoredSubscription* zero = nullptr;
    for (const auto& sub : session.subscriptions) {
      if (sub.first == "room/#")
        kept = &sub.second;
      if (sub.first == "room/qos0")
        zero = &sub.second;
    }
    assert(kept && kept->qos == 1 && kept->no_local && kept->retain_as_published &&
           kept->retain_handling == 2);
    assert(zero && zero->qos == 0 && zero->no_local && zero->retain_as_published &&
           zero->retain_handling == 2);
    assert(store.publish("room/one", "after", "reader", 0).targets.empty());
    assert(store.publish("room/one", "after", "writer", 0).targets.size() == 1);
    auto echo = store.connect("echo", "owner", false, 60);
    assert(echo.present && echo.subscriptions.size() == 1 && !echo.subscriptions[0].second.no_local &&
           echo.subscriptions[0].second.retain_handling == 0);
    assert(store.publish("echo", "after", "echo", 0).targets.size() == 1);
  }
}

static void append_record(mqtt::journal::AppendLog& log, const std::string& body)
{
  auto appended = log.append(body, [](bool ok, const mqtt::journal::Location&) { assert(ok); });
  assert(appended->wait());
}

static void test_legacy_subscription_journal(const std::string& path)
{
  using namespace mqtt::journal;
  mqtt::PersistenceConfig cfg;
  cfg.path = path;
  cfg.partitions = 1;
  cfg.segment_bytes = 8192;
  cfg.max_disk_bytes = 1024 * 1024;
  cfg.checkpoint_interval_ms = 600000;
  ensure_directory(path);
  Encoder format;
  format.text("MQTTS-PARTITION-LOG");
  format.u32(4);
  format.u32(1);
  atomic_file(path + "/FORMAT", checked(format.data));
  {
    AppendLog log(path + "/sessions-0", cfg.segment_bytes, cfg.max_disk_bytes / 4);
    Encoder connect;
    connect.u8(1);
    connect.text("legacy");
    connect.text("owner");
    connect.u64(1);
    connect.u64(1);
    connect.u32(60);
    connect.u64(DurableStore::now_ms() + 60000);
    connect.u8(0);
    append_record(log, connect.data);
    Encoder sub;
    sub.u8(3);
    sub.text("legacy");
    sub.u64(1);
    sub.text("old/filter");
    sub.u8(1);
    append_record(log, sub.data);
    assert(log.seal()->wait());
  }
  {
    DurableStore store(cfg);
    auto session = store.connect("legacy", "owner", false, 60);
    assert(session.ok && session.present && session.subscriptions.size() == 1);
    assert(session.subscriptions[0].first == "old/filter");
    assert(session.subscriptions[0].second.qos == 1);
    assert(!session.subscriptions[0].second.no_local);
    assert(!session.subscriptions[0].second.retain_as_published);
    assert(session.subscriptions[0].second.retain_handling == 0);
    assert(store.publish("old/filter", "self", "legacy", 0).targets.size() == 1);
  }

  const std::string bad = path + "-bad";
  cfg.path = bad;
  ensure_directory(bad);
  Encoder bad_format;
  bad_format.text("MQTTS-PARTITION-LOG");
  bad_format.u32(5);
  bad_format.u32(1);
  atomic_file(bad + "/FORMAT", checked(bad_format.data));
  {
    AppendLog log(bad + "/sessions-0", cfg.segment_bytes, cfg.max_disk_bytes / 4);
    Encoder connect;
    connect.u8(1);
    connect.text("legacy");
    connect.text("owner");
    connect.u64(1);
    connect.u64(1);
    connect.u32(60);
    connect.u64(DurableStore::now_ms() + 60000);
    connect.u8(0);
    append_record(log, connect.data);
    Encoder sub;
    sub.u8(3);
    sub.text("legacy");
    sub.u64(1);
    sub.text("old/filter");
    sub.u8(1);
    sub.u8(99);
    sub.u8(0);
    append_record(log, sub.data);
    assert(log.seal()->wait());
  }
  std::vector<std::filesystem::path> logs;
  for (const auto& entry : std::filesystem::directory_iterator(bad + "/sessions-0"))
    if (entry.path().extension() == ".log")
      logs.push_back(entry.path());
  assert(logs.size() == 1);
  const auto preserved = std::filesystem::file_size(logs[0]);
  bool rejected = false;
  try {
    DurableStore store(cfg);
  } catch (const std::exception&) {
    rejected = true;
  }
  assert(rejected);
  assert(std::filesystem::file_size(logs[0]) == preserved);
}

static void test_overflow_isolation(const std::string& path)
{
  mqtt::PersistenceConfig cfg;
  cfg.path = path;
  cfg.overflow_policy = "isolate";
  cfg.max_messages = 10;
  cfg.max_messages_per_session = 2;
  int64_t gap = 0;
  {
    DurableStore store(cfg);
    auto slow = store.connect("slow", "owner", false, 60);
    auto fast = store.connect("fast", "owner", false, 60);
    assert(store.subscribe("slow", slow.epoch, "topic", 1).ok);
    assert(store.subscribe("fast", fast.epoch, "topic", 1).ok);
    assert(store.disconnect("slow", slow.epoch).ok);
    for (int i = 0; i < 6; ++i) {
      const auto data = "message-" + std::to_string(i);
      auto published = store.publish("topic", data, "writer", 0);
      assert(published.ok && published.accepted_targets == (i < 2 ? 2 : 1));
      auto delivery = store.fetch("fast", fast.epoch, 0, 32);
      assert(delivery.deliveries.size() == 1 && delivery.deliveries[0].wire == data);
      assert(store.acknowledge("fast", fast.epoch, delivery.deliveries[0].packet_id).ok);
    }
    assert(store.statistics().pending == 2 && store.statistics().isolated_sessions == 1);
    assert(store.statistics().overflow_skipped == 4);
    // Recover the gap from the message log before a checkpoint exists.
  }
  {
    DurableStore store(cfg);
    auto slow = store.connect("slow", "owner", false, 60);
    gap = slow.overflow_sequence;
    assert(gap && slow.present && store.statistics().isolated_sessions == 1);
    assert(store.subscribe("slow", slow.epoch, "topic", 1).quota_exceeded);
    auto delivery = store.fetch("slow", slow.epoch, 0, 32);
    assert(delivery.overflow_sequence == gap && delivery.deliveries.size() == 2);
    assert(delivery.deliveries[0].wire == "message-0" &&
           delivery.deliveries[1].wire == "message-1");
    store.checkpoint();
  }
  {
    DurableStore store(cfg);
    auto slow = store.connect("slow", "owner", false, 60);
    assert(slow.overflow_sequence == gap);
    auto delivery = store.fetch("slow", slow.epoch, 0, 32);
    for (const auto& d : delivery.deliveries)
      assert(store.acknowledge("slow", slow.epoch, d.packet_id).ok);
    assert(store.statistics().pending == 0);
    auto reset = store.connect("slow", "owner", true, 60);
    assert(reset.ok && !reset.overflow_sequence && store.statistics().isolated_sessions == 0);
  }
  cfg.path = path + "-bytes";
  cfg.max_bytes_per_session = 4;
  {
    DurableStore store(cfg);
    auto slow = store.connect("slow", "owner", false, 60);
    assert(store.subscribe("slow", slow.epoch, "topic", 1).ok);
    assert(store.publish("topic", "abcd", "writer", 0).ok);
    assert(store.publish("topic", "e", "writer", 0).ok);
    assert(store.statistics().isolated_sessions == 1 && store.statistics().bytes == 4);
  }
}

static void test_incremental_routing_and_expiry(const std::string& path)
{
  mqtt::PersistenceConfig cfg;
  cfg.path = path;
  cfg.checkpoint_interval_ms = 600000;
  DurableStore store(cfg);
  for (int i = 0; i < 64; ++i) {
    auto name = "other-" + std::to_string(i);
    auto s = store.connect(name, "owner", false, 60);
    for (int j = 0; j < 16; ++j)
      assert(store.subscribe(name, s.epoch, name + "/" + std::to_string(j) + "/#", 1).ok);
  }
  auto changing = store.connect("changing", "owner", false, 60);
  for (int i = 0; i < 100; ++i) {
    auto filter = "changing/" + std::to_string(i) + "/+";
    assert(store.subscribe("changing", changing.epoch, filter, 1).ok);
    assert(
        store.publish("changing/" + std::to_string(i) + "/leaf", "x", "writer", 0).targets.size() ==
        1);
    assert(store.subscribe("changing", changing.epoch, filter, 0).ok);
    assert(
        store.publish("changing/" + std::to_string(i) + "/leaf", "x", "writer", 0).targets.empty());
    assert(store.unsubscribe("changing", changing.epoch, filter).ok);
  }
  changing = store.connect("changing", "owner", true, 60);
  assert(store.statistics().pending == 0);
  auto expired = store.connect("expires", "owner", false, 60);
  auto resumed = store.connect("resumed", "owner", false, 1);
  assert(store.subscribe("expires", expired.epoch, "ttl", 1).ok);
  assert(store.subscribe("resumed", resumed.epoch, "ttl", 1).ok);
  assert(store.disconnect("resumed", resumed.epoch).ok);
  resumed = store.connect("resumed", "owner", false, 60);
  assert(store.publish("ttl", "shared", "writer", DurableStore::now_ms() + 100).ok);
  auto in_flight = store.fetch("resumed", resumed.epoch, 0, 1);
  assert(in_flight.deliveries.size() == 1);
  for (int i = 0; i < 40 && store.statistics().pending != 1; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(store.statistics().pending == 1 && store.statistics().unique_bytes == 6);
  // Cancelling the old offline deadline must preserve the reconnected session.
  assert(store.acknowledge("resumed", resumed.epoch, in_flight.deliveries[0].packet_id).ok);
  assert(store.statistics().unique_bytes == 0);
  assert(store.publish("ttl", "session-expiry", "writer", 0).ok);
  assert(store.disconnect("expires", expired.epoch, 1).ok);
  assert(store.disconnect("resumed", resumed.epoch, 1).ok);
  for (int i = 0; i < 40 && store.statistics().pending; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(store.statistics().pending == 0 && store.statistics().unique_bytes == 0);
  assert(store.publish("ttl", "gone", "writer", 0).targets.empty());
  // Unrelated filters remain live after repeated branch removal and node reuse.
  assert(store.publish("other-63/15/leaf", "still routed", "writer", 0).targets.size() == 1);
}

static bool has_retained(const DurableStore& store, const std::string& topic, const std::string& payload)
{
  for (const auto& m : store.retained_messages())
    if (m.topic == topic && m.payload == payload)
      return true;
  return false;
}

static mqtt::PersistenceConfig retain_config(const std::string& path)
{
  mqtt::PersistenceConfig cfg;
  cfg.path = path;
  cfg.partitions = 1;
  cfg.segment_bytes = 8192;
  cfg.max_disk_bytes = 1024 * 1024;
  cfg.checkpoint_interval_ms = 600000;
  return cfg;
}

// RETAIN snapshots and retain log records written before properties were stored.
static void test_legacy_retain_encodings(const std::string& path)
{
  using namespace mqtt::journal;
  auto cfg = retain_config(path);
  { DurableStore store(cfg); }
  Encoder snapshot;
  snapshot.text("MQTTS-RETAIN-1");
  snapshot.u64(0);
  snapshot.u32(2);
  snapshot.text("old/snap");
  snapshot.u8(1);
  snapshot.u64(0);
  snapshot.text("from-snapshot");
  snapshot.text("pub");
  snapshot.text("old/cleared");
  snapshot.u8(0);
  snapshot.u64(0);
  snapshot.text("cleared-later");
  snapshot.text("pub");
  atomic_file(path + "/RETAIN", checked(snapshot.data));
  {
    AppendLog log(path + "/retain", cfg.segment_bytes, cfg.max_disk_bytes / 32);
    Encoder set;
    set.u8(kRetainSet);
    set.u8(1);
    set.text("old/log");
    set.u8(1);
    set.u64(0);
    set.text("from-log");
    set.text("writer");
    Encoder clear;
    clear.u8(kRetainClear);
    clear.u8(1);
    clear.text("old/cleared");
    assert(log.append(set.data, [](bool ok, const Location&) { assert(ok); })->wait());
    assert(log.append(clear.data, [](bool ok, const Location&) { assert(ok); })->wait());
  }
  DurableStore store(cfg);
  auto kept = store.retained_messages();
  assert(kept.size() == 2);
  assert(has_retained(store, "old/snap", "from-snapshot"));
  assert(has_retained(store, "old/log", "from-log"));
  for (const auto& item : kept)
    assert(item.properties.content_type.empty() && item.properties.user_properties.empty());
  store.checkpoint();
  assert(read_file(path + "/RETAIN", 4096).find("MQTTS-RETAIN-2") != std::string::npos);
}

// Fill the retain log until a record smaller than the mutations under test no
// longer fits. A rejected 1000-byte put used to leave a gap a clear could occupy.
// wait=false only reserves bytes, so this finishes before the 1 s pressure checkpoint.
static void fill_retain_log(DurableStore& store)
{
  for (int i = 0; i < 10000; ++i) {
    auto filled = store.clear_retained("a", false);
    if (!filled.ok) {
      assert(filled.quota_exceeded);
      assert(filled.error.find("disk") != std::string::npos);
      return;
    }
  }
  assert(!"retain log never filled");
}

// Regression: a refused replace or clear used to erase the committed value, and the
// next checkpoint made that loss permanent.
static void test_retain_failure_keeps_previous(const std::string& path)
{
  {
    auto cfg = retain_config(path + "-oversize");
    cfg.max_request_bytes = 4096;
    {
      DurableStore store(cfg);
      assert(store.put_retained("keep", 1, "old-value", "pub", 0, true).ok);
      auto replaced = store.put_retained("keep", 1, std::string(5000, 'n'), "pub", 0, true);
      assert(!replaced.ok && replaced.quota_exceeded);
      auto async = store.put_retained("keep", 0, std::string(5000, 'n'), "pub", 0, false);
      assert(!async.ok && async.quota_exceeded && !async.commit);
      assert(has_retained(store, "keep", "old-value"));
      store.checkpoint();
    }
    DurableStore store(cfg);
    assert(has_retained(store, "keep", "old-value"));
  }
  const char* cases[] = {"clear", "expired", "replace"};
  for (const char* name : cases) {
    const std::string mode(name);
    // One segment for the retain log, so admission fills in a few dozen records.
    auto cfg = retain_config(path + "-" + mode);
    cfg.segment_bytes = 4096;
    cfg.max_disk_bytes = 32 * 4096;
    {
      DurableStore store(cfg);
      assert(store.put_retained("keep", 1, "old-value", "pub", 0, true).ok);
      fill_retain_log(store);
      DurableStore::Result refused;
      if (mode == "clear")
        refused = store.clear_retained("keep", true);
      else if (mode == "expired")
        refused = store.put_retained("keep", 1, "new", "pub", DurableStore::now_ms() - 1, true);
      else
        refused = store.put_retained("keep", 1, "new-value", "pub", 0, true);
      assert(!refused.ok && refused.quota_exceeded);
      assert(has_retained(store, "keep", "old-value"));
      store.checkpoint();
      // The rolled-back value is still the latest mutation for later writes.
      assert(store.put_retained("other", 1, "after", "pub", 0, true).ok);
    }
    DurableStore store(cfg);
    assert(has_retained(store, "keep", "old-value"));
    assert(has_retained(store, "other", "after"));
  }
  {
    auto cfg = retain_config(path + "-deferred");
    DurableStore store(cfg);
    auto pending = store.put_retained("keep", 0, "v1", "pub", 0, false);
    assert(pending.ok && pending.commit);
    DurableStore::await(pending);
    assert(pending.ok && !pending.commit);
    assert(has_retained(store, "keep", "v1"));
  }
}

// Regression: each put walked and copied every retained payload to size the snapshot.
static void test_retain_put_cost(const std::string& path)
{
  mqtt::PersistenceConfig cfg;
  cfg.path = path;
  cfg.partitions = 4;
  cfg.max_disk_bytes = 4294967296ULL;
  cfg.checkpoint_interval_ms = 600000;
  DurableStore store(cfg);
  const std::string payload(256, 'p');
  const int total = 50000, batch = 1000;
  auto timed = [&](int from) {
    auto start = std::chrono::steady_clock::now();
    for (int i = from; i < from + batch; ++i) {
      auto r = store.put_retained("store/" + std::to_string(i), 0, payload, "pub", 0, false);
      assert(r.ok);
    }
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() /
           batch;
  };
  const double first = timed(0);
  for (int i = batch; i < total - batch; ++i)
    assert(store.put_retained("store/" + std::to_string(i), 0, payload, "pub", 0, false).ok);
  const double last = timed(total - batch);
  std::printf("durable put_retained: %.1f us/op at empty, %.1f us/op at %d entries\n", first, last,
              total);
  assert(last < first * 10 + 200.0);
}

// Regression: the retain log and both RETAIN generations sat outside max_disk_bytes.
static void test_retain_disk_budget(const std::string& path)
{
  auto cfg = retain_config(path);
  {
    DurableStore store(cfg);
    for (int i = 0;; ++i) {
      auto r = store.put_retained("big/" + std::to_string(i), 1, std::string(2000, 'b'), "pub", 0, true);
      if (!r.ok) {
        assert(r.quota_exceeded);
        break;
      }
      if (i % 4 == 3)
        store.checkpoint();
      assert(i < 10000);
    }
    store.checkpoint();
  }
  const auto retain_bytes = std::filesystem::file_size(path + "/RETAIN");
  assert(retain_bytes <= cfg.max_disk_bytes / 32);
  uintmax_t total = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
    if (!entry.is_regular_file())
      continue;
    const auto name = entry.path().filename().string();
    // FORMAT and the ownership lock sit outside the data budget.
    if (name == "FORMAT" || name.find(".lock") != std::string::npos)
      continue;
    total += entry.file_size();
  }
  assert(total <= cfg.max_disk_bytes);
  bool rejected = false;
  try {
    auto bad = retain_config(path + "-small");
    bad.max_disk_bytes = 31 * 8192;
    DurableStore store(bad);
  } catch (const std::exception&) {
    rejected = true;
  }
  assert(rejected);
}

int main()
{
  char dir[] = "/tmp/mqtts-store-XXXXXX";
  assert(mkdtemp(dir));
  mqtt::PersistenceConfig cfg;
  cfg.enabled = true;
  cfg.path = std::string(dir) + "/journal";
  cfg.max_messages = 64;
  cfg.max_messages_per_session = 64;
  uint16_t first = 0;
  {
    DurableStore store(cfg);
    bool locked = false;
    try {
      DurableStore second(cfg);
    } catch (const std::exception&) {
      locked = true;
    }
    assert(locked);
    auto a = store.connect("reader", "owner", false, 60);
    assert(a.ok && !a.present && a.epoch);
    assert(store.subscribe("reader", a.epoch, "room/#", 1).ok);
    assert(store.subscribe("reader", a.epoch, "room/+", 1).ok);
    assert(store.disconnect("reader", a.epoch).ok);
    auto takeover = store.connect("reader", "attacker", true, 60);
    assert(!takeover.ok && takeover.not_authorized);
    assert(store.publish("room/one", "payload", "publisher", 0).targets.size() == 1);
    auto b = store.connect("reader", "owner", false, 60);
    assert(b.ok && b.present && b.subscriptions.size() == 2);
    assert(store.acknowledge("reader", a.epoch, 1).stale);
    auto d = store.fetch("reader", b.epoch, 0, 1);
    assert(d.ok && d.deliveries.size() == 1 && !d.deliveries[0].dup);
    first = d.deliveries[0].packet_id;
    assert(first != 0);
    // Close without disconnect or PUBACK, modeling process restart.
  }
  {
    DurableStore store(cfg);
    auto c = store.connect("reader", "owner", false, 60);
    assert(c.ok && c.present);
    auto d = store.fetch("reader", c.epoch, 0, 1);
    assert(d.ok && d.deliveries.size() == 1 && d.deliveries[0].dup &&
           d.deliveries[0].packet_id == first);
    assert(d.deliveries[0].wire == "payload");
    assert(store.acknowledge("reader", c.epoch, first).ok);
    assert(store.statistics().pending == 0);
    for (int i = 0; i < 64; ++i)
      assert(store.publish("room/one", "data", "writer", 0).ok);
    assert(!store.publish("room/one", "overflow", "writer", 0).ok);
    assert(store.statistics().pending == 64);
    auto batch = store.fetch("reader", c.epoch, 0, 2);
    assert(batch.deliveries.size() == 2);
    for (const auto& delivery : batch.deliveries)
      assert(store.begin_delivery("reader", c.epoch, delivery.sequence, delivery.packet_id).ok);
    auto last = batch.deliveries.back().sequence;
    assert(store.fetch("reader", c.epoch, last, 2).deliveries.empty());
    // Reconnect with a smaller receive window, including already assigned IDs.
    auto next = store.connect("reader", "owner", false, 60);
    auto one = store.fetch("reader", next.epoch, 0, 1);
    assert(one.deliveries.size() == 1 && one.deliveries[0].dup);
    assert(store.disconnect("reader", c.epoch).stale);
    auto clean = store.connect("reader", "owner", true, 60);
    assert(clean.ok && !clean.present && clean.subscriptions.empty());
    assert(store.statistics().bytes == 0);
    assert(store.subscribe("reader", clean.epoch, "#", 1).ok);
    assert(store.publish("$SYS/state", "secret", "writer", 0).targets.empty());
    assert(store.subscribe("reader", clean.epoch, "$SYS/#", 1).ok);
    assert(store.publish("$SYS/state", "state", "writer", 0).targets.size() == 1);
    assert(store.disconnect("reader", clean.epoch, 0).ok);
    auto expired = store.connect("expiring", "owner", false, 1);
    assert(store.subscribe("expiring", expired.epoch, "a", 1).ok);
    assert(store.publish("a", "already-expired", "writer", DurableStore::now_ms() - 1)
               .targets.empty());
    assert(store.disconnect("expiring", expired.epoch).ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    assert(!store.connect("expiring", "owner", false, 60).present);
    auto concurrent = store.connect("parallel", "owner", false, 60);
    assert(store.subscribe("parallel", concurrent.epoch, "load/#", 1).ok);
    std::vector<std::thread> writers;
    for (int i = 0; i < 8; ++i)
      writers.emplace_back([&] {
        for (int j = 0; j < 8; ++j)
          assert(store.publish("load/one", "parallel-data", "writer", 0).ok);
      });
    for (auto& thread : writers)
      thread.join();
    assert(store.statistics().pending == 64);
  }
  {
    // Checkpointing keeps exact ACK gaps across topic partitions and compaction.
    DurableStore store(cfg);
    auto c = store.connect("parallel", "owner", false, 60);
    auto batch = store.fetch("parallel", c.epoch, 0, 32);
    assert(batch.ok && batch.deliveries.size() == 32);
    for (size_t i = 1; i < batch.deliveries.size(); i += 2)
      assert(store.acknowledge("parallel", c.epoch, batch.deliveries[i].packet_id).ok);
    assert(store.statistics().pending == 48);
    store.checkpoint();
  }
  {
    DurableStore store(cfg);
    auto c = store.connect("parallel", "owner", false, 60);
    int64_t cursor = 0;
    size_t count = 0;
    while (store.statistics().pending) {
      auto batch = store.fetch("parallel", c.epoch, cursor, 32);
      assert(batch.ok && !batch.deliveries.empty());
      for (const auto& d : batch.deliveries) {
        assert(d.dup && d.sequence > cursor);
        cursor = d.sequence;
        assert(store.acknowledge("parallel", c.epoch, d.packet_id).ok);
        ++count;
      }
    }
    assert(count == 48);
    store.checkpoint();
    for (int i = 0; i < 32; ++i)
      assert(store.publish("load/" + std::to_string(i), "partition-data", "writer", 0).ok);
    store.checkpoint();
  }
  {
    DurableStore store(cfg);
    assert(store.statistics().pending == 32);
    auto c = store.connect("parallel", "owner", true, 60);
    assert(c.ok && store.statistics().pending == 0);
    store.checkpoint();
    for (const auto& name : {"zulu", "alpha", "tango", "bravo"}) {
      auto reader = store.connect(name, "owner", false, 60);
      assert(reader.ok && store.subscribe(name, reader.epoch, "fanout/#", 1).ok);
      assert(store.subscribe(name, reader.epoch, "fanout/one", 1).ok);
    }
    auto fanout = store.publish("fanout/one", "one publication", "writer", 0);
    assert(fanout.ok);
    assert((fanout.targets == std::vector<std::string>{"alpha", "bravo", "tango", "zulu"}));
    assert(store.statistics().bytes == store.statistics().unique_bytes * 4);
  }
  test_live_packet_id_is_shared(std::string(dir) + "/live-packet-id");
  test_compaction(std::string(dir) + "/compact");
  test_native_upgrade(std::string(dir) + "/upgrade-2", 2);
  test_native_upgrade(std::string(dir) + "/upgrade-3", 3);
  test_native_upgrade(std::string(dir) + "/upgrade-4", 4);
  test_native_upgrade(std::string(dir) + "/upgrade-5", 5);
  test_legacy_retain_encodings(std::string(dir) + "/retain-v1");
  test_retain_failure_keeps_previous(std::string(dir) + "/retain-failure");
  test_retain_put_cost(std::string(dir) + "/retain-cost");
  test_retain_disk_budget(std::string(dir) + "/retain-budget");
  test_subscription_options(std::string(dir) + "/options");
  test_retain_journal(std::string(dir) + "/retain");
  test_legacy_subscription_journal(std::string(dir) + "/legacy-options");
  test_overflow_isolation(std::string(dir) + "/overflow");
  test_incremental_routing_and_expiry(std::string(dir) + "/indexes");
  std::filesystem::remove_all(dir);
  puts(
      "PASS persistent restart, ownership, epochs, receive window, expiry, deduplication, quotas "
      "and concurrent commits");
}
