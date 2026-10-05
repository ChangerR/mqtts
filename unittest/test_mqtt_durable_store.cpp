#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>
#include "mqtt_durable_store.h"
using mqtt::DurableStore;
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
    assert(!store.connect("reader", "attacker", true, 60).ok);
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
  }
  std::filesystem::remove_all(dir);
  puts(
      "PASS persistent restart, ownership, epochs, receive window, expiry, deduplication, quotas "
      "and concurrent commits");
}
