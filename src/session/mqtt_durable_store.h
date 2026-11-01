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
#include "mqtt_log_codec.h"

namespace mqtt {
namespace journal {
struct Completion;
}

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
    bool ok = false, present = false, stale = false, wake = false, not_authorized = false;
    bool quota_exceeded = false;
    // The durable mutex was busy or frozen. Nothing was mutated; retry outside any
    // lock that must not be held across a wait.
    bool deferred = false;
    std::shared_ptr<Signal> revision;
    uint64_t epoch = 0;
    int64_t overflow_sequence = 0;
    size_t accepted_targets = 0;
    std::string error;
    std::vector<std::pair<std::string, journal::StoredSubscription>> subscriptions;
    std::vector<std::string> targets;
    std::vector<Delivery> deliveries;
    // Shared lifetime bounds delivery copies even when a socket remains slow.
    std::shared_ptr<void> reservation;
    // Set when the caller passed wait = false: the record is admitted and ordered,
    // but not yet durable. await() blocks the calling coroutine until it is.
    std::shared_ptr<journal::Completion> commit;
  };
  // Waits for a deferred commit and folds its outcome into ok/error.
  static void await(Result& result);
  explicit DurableStore(const PersistenceConfig& config);
  ~DurableStore();
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  Result connect(const std::string& client, const std::string& owner, bool clean, uint32_t expiry);
  Result disconnect(const std::string& client, uint64_t epoch, int64_t expiry_override = -1);
  // no_spin returns deferred instead of waiting when the journal mutex is busy.
  Result subscribe(const std::string& client, uint64_t epoch, const std::string& filter,
                   const journal::StoredSubscription& subscription, bool wait = true,
                   bool no_spin = false);
  Result subscribe(const std::string& client, uint64_t epoch, const std::string& filter,
                   uint8_t qos)
  {
    journal::StoredSubscription subscription;
    subscription.qos = qos;
    return subscribe(client, epoch, filter, subscription);
  }
  Result unsubscribe(const std::string& client, uint64_t epoch, const std::string& filter);
  // implicit_no_local forces the publisher out, matching mqtt.implicit_no_local.
  // Otherwise the publisher is skipped only when every matching stored subscription
  // has No Local set. Legacy records decode with that flag clear.
  Result publish(const std::string& topic, const std::string& wire, const std::string& sender,
                 int64_t expires, bool implicit_no_local = false, bool wait = true,
                 bool no_spin = false);
  Result fetch(const std::string& client, uint64_t epoch, int64_t after, uint16_t receive_maximum);
  Result begin_delivery(const std::string& client, uint64_t epoch, int64_t sequence,
                        uint16_t packet_id);
  Result acknowledge(const std::string& client, uint64_t epoch, uint16_t packet_id,
                     bool wait_for_commit = true);
  enum class DiscardReason { NotAuthorized, PacketTooLarge, Malformed };
  Result discard(const std::string& client, uint64_t epoch, uint16_t packet_id,
                 DiscardReason reason);
  // 在线路径占住的报文标识符。和 sessions.packets 一起构成“在途 ID 已占用”。
  // 不进入确认日志，PUBACK 不会把它当成持久化队列里的消息。
  enum class PacketIdClaim { Reserved, InUse, Busy, Quota, Absent };
  PacketIdClaim claim_live_packet_id(const std::string& client, uint64_t epoch, uint16_t id);
  void release_live_packet_id(const std::string& client, uint64_t epoch, uint16_t id);

  bool has_subscriptions() const;
  uint32_t max_session_expiry() const;
  static int64_t now_ms();
  struct Statistics
  {
    size_t sessions = 0, pending = 0, bytes = 0;
    size_t unique_bytes = 0, checkpoint_bytes = 0;
    size_t isolated_sessions = 0, overflow_skipped = 0;
    std::vector<size_t> message_partition_bytes, session_partition_bytes;
    size_t discarded_denied = 0, discarded_oversize = 0, discarded_malformed = 0;
    size_t retained = 0, retain_log_bytes = 0;
  };
  Statistics statistics() const;
  // Versioned retain log under the store directory. Empty payload is not a put;
  // deletion is clear_retained. With wait false the call returns once the record is
  // admitted; Result::commit then tracks durability. A failed put or clear restores
  // the previous value.
  struct RetainedProperties
  {
    uint8_t payload_format_indicator = 0;
    std::string content_type;
    std::string response_topic;
    std::string correlation_data;
    std::vector<std::pair<std::string, std::string>> user_properties;
  };
  struct RetainedMessage
  {
    std::string topic;
    uint8_t qos = 0;
    std::string payload;
    std::string sender;
    int64_t expires_ms = 0;
    RetainedProperties properties;
  };
  Result put_retained(const std::string& topic, uint8_t qos, const std::string& payload,
                      const std::string& sender, int64_t expires_ms, bool wait);
  Result put_retained(const std::string& topic, uint8_t qos, const std::string& payload,
                      const std::string& sender, int64_t expires_ms, bool wait,
                      const RetainedProperties& properties, bool no_spin = false);
  Result clear_retained(const std::string& topic, bool wait, bool no_spin = false);
  // Committed, unexpired messages. Absent on journals that predate the retain log.
  std::vector<RetainedMessage> retained_messages() const;
  // Queue one QoS 1 publish into the online persistent session opened with epoch.
  // present is false when that client has no online durable session; stale is set
  // when a different session (another epoch) is online for the same client.
  Result enqueue_directed(const std::string& client, uint64_t epoch, const std::string& wire,
                          int64_t expires, bool wait = true);
  // Maintenance/administration API; performs blocking disk work outside MQTT threads.
  void checkpoint();
  void import_legacy(const std::string& exported);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace mqtt
