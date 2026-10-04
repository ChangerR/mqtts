#include <sqlite3.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include "mqtt_durable_store.h"
using mqtt::DurableStore;
static int64_t scalar(const std::string& path, const char* query)
{
  sqlite3* db = nullptr;
  assert(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
  sqlite3_stmt* row = nullptr;
  assert(sqlite3_prepare_v2(db, query, -1, &row, nullptr) == SQLITE_OK);
  assert(sqlite3_step(row) == SQLITE_ROW);
  auto n = sqlite3_column_int64(row, 0);
  sqlite3_finalize(row);
  sqlite3_close(db);
  return n;
}
int main()
{
  char dir[] = "/tmp/mqtts-store-XXXXXX";
  assert(mkdtemp(dir));
  mqtt::PersistenceConfig cfg;
  cfg.enabled = true;
  cfg.path = std::string(dir) + "/sessions.db";
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
    assert(scalar(cfg.path, "SELECT pending FROM counters") == 0);
    assert(scalar(cfg.path, "SELECT count(*) FROM messages") == 0);
    for (int i = 0; i < 64; ++i)
      assert(store.publish("room/one", "data", "writer", 0).ok);
    assert(!store.publish("room/one", "overflow", "writer", 0).ok);
    assert(scalar(cfg.path, "SELECT pending FROM counters") == 64);
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
    assert(scalar(cfg.path, "SELECT bytes FROM counters") == 0);
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
    auto failed_ack = store.connect("failed-ack", "owner", false, 60);
    assert(store.subscribe("failed-ack", failed_ack.epoch, "ack", 1).ok);
    assert(store.publish("ack", "ack-payload", "writer", 0).ok);
    auto flight = store.fetch("failed-ack", failed_ack.epoch, 0, 1);
    assert(flight.deliveries.size() == 1);
    sqlite3* blocker = nullptr;
    assert(sqlite3_open(cfg.path.c_str(), &blocker) == SQLITE_OK);
    assert(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(store.acknowledge("failed-ack", failed_ack.epoch, flight.deliveries[0].packet_id, false)
               .ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    assert(sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(blocker);
    assert(store.fetch("failed-ack", failed_ack.epoch, flight.deliveries[0].sequence, 1).stale);
    auto retry = store.connect("failed-ack", "owner", false, 60);
    auto replay = store.fetch("failed-ack", retry.epoch, 0, 1);
    assert(replay.deliveries.size() == 1 && replay.deliveries[0].dup);
    assert(store.disconnect("failed-ack", retry.epoch, 0).ok);
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
    assert(scalar(cfg.path, "SELECT pending FROM counters") == 64);
  }
  for (auto suffix : {"", "-wal", "-shm", ".lock"})
    unlink((cfg.path + suffix).c_str());
  rmdir(dir);
  puts(
      "PASS persistent restart, ownership, epochs, receive window, expiry, deduplication, quotas "
      "and concurrent commits");
}
