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

struct sqlite3;
namespace mqtt {

// All SQLite work is owned by one OS thread. Callers wait on eventfd through
// the coroutine runtime; neither filesystem IO nor a std::future blocks MQTT.
class DurableStore
{
 public:
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
    std::shared_ptr<std::atomic<uint64_t>> revision;
    uint64_t epoch = 0;
    std::string error;
    std::vector<std::pair<std::string, uint8_t>> subscriptions;
    std::vector<std::string> targets;
    std::vector<Delivery> deliveries;
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
  bool has_subscriptions() const { return subscription_count_.load() != 0; }
  uint32_t max_session_expiry() const { return config_.max_session_expiry_seconds; }
  static int64_t now_ms();

 private:
  struct Request;
  Request* active_request_ = nullptr;
  bool reserve_result(size_t bytes);
  PersistenceConfig config_;
  sqlite3* db_ = nullptr;
  int lock_fd_ = -1;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<std::shared_ptr<Request>> queue_;
  size_t outstanding_ = 0, bytes_ = 0, bulk_bytes_ = 0;
  bool stopping_ = false;
  std::thread worker_;
  std::atomic<size_t> subscription_count_{0};
  std::unordered_map<std::string, std::shared_ptr<std::atomic<uint64_t>>> signals_;
  std::unordered_map<std::string, uint64_t> failed_epochs_;
  Result execute(size_t bytes, std::function<Result(sqlite3*)> fn, bool bulk = false,
                 bool wait_for_commit = true, const std::string& failure_client = "",
                 uint64_t failure_epoch = 0);
  void work();
  void sweep(sqlite3* db, bool startup = false);
  struct TopicNode
  {
    std::unordered_map<std::string, size_t> children;
    std::vector<std::string> clients;
  };
  std::vector<TopicNode> topic_index_;
  bool index_dirty_ = true;
  std::vector<std::string> match_targets(sqlite3* db, const std::string& topic);
};
}  // namespace mqtt
