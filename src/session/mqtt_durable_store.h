#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "mqtt_config.h"

namespace mqtt {

// Topic-partitioned append logs and separate sharded session journals.
// Filesystem work is performed by OS workers, never MQTT event threads.
class DurableStore
{
 public:
  class Signal
  {
   public:
    Signal();
    ~Signal();
    uint64_t load() const { return revision_.load(std::memory_order_acquire); }
    void notify();
    void wait(uint64_t observed, int timeout_ms);

   private:
    int fd_ = -1;
    std::atomic<uint64_t> revision_{1};
  };
  struct Delivery
  {
    int64_t sequence = 0;
    uint16_t packet_id = 0;
    bool dup = false;
    int64_t expires = 0;
    std::string wire;
  };
  struct Result
  {
    bool ok = false, present = false, stale = false, wake = false;
    std::shared_ptr<Signal> revision;
    uint64_t epoch = 0;
    std::string error;
    std::vector<std::pair<std::string, uint8_t>> subscriptions;
    std::vector<std::string> targets;
    std::vector<Delivery> deliveries;
    // Shared lifetime bounds delivery copies even when a socket remains slow.
    std::shared_ptr<void> reservation;
  };
  explicit DurableStore(const PersistenceConfig& config);
  ~DurableStore();
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  Result connect(const std::string& client, const std::string& owner, bool clean, uint32_t expiry);
  Result disconnect(const std::string& client, uint64_t epoch, int64_t expiry_override = -1);
  Result subscribe(const std::string& client, uint64_t epoch, const std::string& filter,
                   uint8_t qos);
  Result unsubscribe(const std::string& client, uint64_t epoch, const std::string& filter);
  Result publish(const std::string& topic, const std::string& wire, const std::string& sender,
                 int64_t expires);
  Result fetch(const std::string& client, uint64_t epoch, int64_t after, uint16_t receive_maximum);
  Result acknowledge(const std::string& client, uint64_t epoch, uint16_t packet_id,
                     bool wait_for_commit = true);
  bool has_subscriptions() const;
  uint32_t max_session_expiry() const;
  static int64_t now_ms();
  struct Statistics
  {
    size_t sessions = 0, pending = 0, bytes = 0;
  };
  Statistics statistics() const;
  // Maintenance/administration API; performs blocking disk work outside MQTT threads.
  void checkpoint();
  void import_legacy(const std::string& exported);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace mqtt
