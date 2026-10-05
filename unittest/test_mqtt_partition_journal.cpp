#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include "mqtt_append_log.h"
#include "mqtt_durable_store.h"
using mqtt::DurableStore;
using mqtt::journal::AppendLog;
using mqtt::journal::Encoder;
using mqtt::journal::Location;
namespace fs = std::filesystem;

static std::vector<fs::path> segments(const std::string& directory)
{
  std::vector<fs::path> found;
  for (const auto& entry : fs::directory_iterator(directory))
    if (entry.path().extension() == ".log")
      found.push_back(entry.path());
  std::sort(found.begin(), found.end());
  return found;
}
int main()
{
  char temporary[] = "/tmp/mqtts-journal-test-XXXXXX";
  assert(mkdtemp(temporary));
  std::string root = temporary;
  auto noop = [](bool ok, const Location&) { assert(ok); };
  uint64_t cut = 0;
  {
    AppendLog log(root + "/append", 4096, 16384);
    std::vector<std::shared_ptr<mqtt::journal::Completion>> jobs;
    for (int i = 0; i < 12; ++i)
      jobs.push_back(log.append(std::string(512, char('a' + i)), noop));
    for (const auto& job : jobs)
      assert(job->wait());
    assert(log.seal()->wait());
    cut = log.serial();
    assert(cut == 12);
    assert(segments(root + "/append").size() >= 2);
    size_t n = 0;
    log.replay(0, [&](const Location& location, const std::string& data) {
      assert(location.serial == ++n && data == std::string(512, char('a' + n - 1)));
    });
    assert(n == 12);
    log.prune(cut, {});
    assert(segments(root + "/append").empty());
  }
  {
    AppendLog log(root + "/append", 4096, 16384);
    log.replay(cut, [](const Location&, const std::string&) { assert(false); });
    auto next = log.append("after checkpoint", noop);
    assert(next->wait() && next->location.serial == cut + 1);
  }
  auto last = segments(root + "/append").back();
  auto good_size = fs::file_size(last);
  {
    std::ofstream out(last, std::ios::binary | std::ios::app);
    out.write("partial", 7);
  }
  {
    AppendLog log(root + "/append", 4096, 16384);
    assert(fs::file_size(last) == good_size);
    size_t n = 0;
    log.replay(cut, [&](const Location&, const std::string& data) {
      assert(data == "after checkpoint");
      ++n;
    });
    assert(n == 1);
  }
  {
    Encoder e;
    e.u32(0x314c514d);
    e.u32(100);
    e.u64(cut + 2);
    e.u32(0);
    e.u32(mqtt::journal::checksum(e.data.data(), e.data.size()));
    std::ofstream out(last, std::ios::binary | std::ios::app);
    out.write(e.data.data(), e.data.size());
    out.write("torn", 4);
  }
  {
    AppendLog log(root + "/append", 4096, 16384);
    assert(fs::file_size(last) == good_size);
  }
  {
    std::fstream file(last, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(24);
    file.put('!');
  }
  bool rejected = false;
  try {
    AppendLog log(root + "/append", 4096, 16384);
  } catch (const std::exception&) {
    rejected = true;
  }
  assert(rejected);
  {
    AppendLog log(root + "/quota", 4096, 8192);
    assert(log.append(std::string(6000, 'x'), noop)->wait());
    rejected = false;
    try {
      log.append(std::string(4000, 'x'), noop);
    } catch (const std::exception&) {
      rejected = true;
    }
    assert(rejected);
    // Quota rejection doesn't poison the writer or discard its accepted prefix.
    assert(log.append("small", noop)->wait());
    assert(log.seal()->wait());
    log.prune(log.serial(), {});
    assert(log.append(std::string(7000, 'y'), noop)->wait());
  }
  {
    AppendLog log(root + "/gap", 4096, 65536);
    for (int i = 0; i < 3; ++i)
      assert(log.append(std::string(4096, 'x'), noop)->wait());
  }
  fs::remove(segments(root + "/gap")[1]);
  rejected = false;
  try {
    AppendLog log(root + "/gap", 4096, 65536);
    log.replay(0, [](const Location&, const std::string&) {});
  } catch (const std::exception&) {
    rejected = true;
  }
  assert(rejected);

  mqtt::PersistenceConfig cfg;
  cfg.enabled = true;
  cfg.path = root + "/packets";
  cfg.max_messages = 70000;
  cfg.max_messages_per_session = 70000;
  cfg.segment_bytes = 65536;
  {
    DurableStore store(cfg);
    auto s = store.connect("reader", "owner", false, 300);
    assert(s.ok);
    assert(store.subscribe("reader", s.epoch, "topic/#", 1).ok);
    std::vector<std::thread> writers;
    for (int writer = 0; writer < 16; ++writer)
      writers.emplace_back([&, writer] {
        for (int i = 0; i < 4125; ++i)
          assert(store.publish("topic/" + std::to_string(writer), "message", "writer", 0).ok);
      });
    for (auto& thread : writers)
      thread.join();
    assert(store.statistics().pending == 66000);
    store.checkpoint();
  }
  {
    DurableStore store(cfg);
    auto s = store.connect("reader", "owner", false, 300);
    assert(s.ok && s.present);
    int64_t cursor = 0;
    size_t received = 0;
    while (received < 66000) {
      auto batch = store.fetch("reader", s.epoch, cursor, 32);
      assert(batch.ok);
      if (batch.deliveries.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      for (const auto& d : batch.deliveries) {
        assert(d.sequence > cursor && d.packet_id && d.wire == "message");
        cursor = d.sequence;
        assert(store.acknowledge("reader", s.epoch, d.packet_id, false).ok);
        ++received;
      }
    }
    store.checkpoint();
    assert(store.statistics().pending == 0);
    for (int i = 0; i < 40; ++i)
      assert(store.publish("topic/new", "after wrap", "writer", 0).ok);
  }
  {
    DurableStore store(cfg);
    assert(store.statistics().pending == 40);
    auto s = store.connect("reader", "owner", false, 300);
    assert(s.ok);
    auto batch = store.fetch("reader", s.epoch, 0, 32);
    assert(batch.deliveries.size() == 32);
    // ACK the largest packet only; older holes must survive another restart.
    assert(store.acknowledge("reader", s.epoch, batch.deliveries.back().packet_id).ok);
    store.checkpoint();
  }
  {
    DurableStore store(cfg);
    assert(store.statistics().pending == 39);
  }
  cfg.partitions = 2;
  rejected = false;
  try {
    DurableStore store(cfg);
  } catch (const std::exception&) {
    rejected = true;
  }
  assert(rejected);
  {
    mqtt::PersistenceConfig heartbeat;
    heartbeat.enabled = true;
    heartbeat.path = root + "/heartbeat";
    heartbeat.partitions = 1;
    heartbeat.max_request_bytes = 4 * 1024 * 1024;
    DurableStore store(heartbeat);
    // A single unbounded heartbeat would exceed the request budget even
    // though every session and the total metadata fit their limits.
    for (int i = 0; i < 80; ++i)
      assert(store.connect(std::to_string(i) + std::string(60000, 'h'), "owner", false, 300).ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    assert(store.connect("heartbeat-check", "owner", false, 300).ok);
    store.checkpoint();
  }
  fs::remove_all(root);
  puts(
      "PASS log rotation, torn tails, corruption, quotas, reclamation, missing records, packet-ID "
      "exhaustion/wrap, ACK gaps and partition identity");
}
