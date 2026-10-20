#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mqtt {

// 每条事件线程一份。字段名在本阶段定稿，阶段 1 的管理 API 直接沿用。
// 只有所在事件线程写入，监控线程只做 relaxed 读。结构按缓存行对齐，避免线程之间伪共享。
struct alignas(64) ProtocolCounters {
  std::atomic<uint64_t> publish_received;
  std::atomic<uint64_t> publish_received_bytes;
  std::atomic<uint64_t> publish_sent;
  std::atomic<uint64_t> publish_sent_bytes;
  std::atomic<uint64_t> bytes_received;
  std::atomic<uint64_t> bytes_sent;
  std::atomic<uint64_t> keepalive_timeouts;
  std::atomic<uint64_t> connect_timeouts;
  std::atomic<uint64_t> session_takeovers;
  std::atomic<uint64_t> malformed_packet_disconnects;
  std::atomic<uint64_t> outbound_packet_size_drops;
  std::atomic<uint64_t> inflight_window_delays;
  std::atomic<uint64_t> wills_published;
  std::atomic<uint64_t> wills_rejected;
  std::atomic<uint64_t> retained_messages;
  std::atomic<uint64_t> retained_bytes;
  std::atomic<uint64_t> retained_not_stored;
  std::atomic<uint64_t> retained_delivery_truncated;

  ProtocolCounters()
      : publish_received(0),
        publish_received_bytes(0),
        publish_sent(0),
        publish_sent_bytes(0),
        bytes_received(0),
        bytes_sent(0),
        keepalive_timeouts(0),
        connect_timeouts(0),
        session_takeovers(0),
        malformed_packet_disconnects(0),
        outbound_packet_size_drops(0),
        inflight_window_delays(0),
        wills_published(0),
        wills_rejected(0),
        retained_messages(0),
        retained_bytes(0),
        retained_not_stored(0),
        retained_delivery_truncated(0)
  {
  }

  ProtocolCounters(const ProtocolCounters&) = delete;
  ProtocolCounters& operator=(const ProtocolCounters&) = delete;
};

static_assert(alignof(ProtocolCounters) == 64, "protocol counters must start on a cache line");
static_assert(sizeof(ProtocolCounters) % 64 == 0, "protocol counters must not share a cache line");

struct ProtocolCounterSnapshot {
  uint64_t publish_received = 0;
  uint64_t publish_received_bytes = 0;
  uint64_t publish_sent = 0;
  uint64_t publish_sent_bytes = 0;
  uint64_t bytes_received = 0;
  uint64_t bytes_sent = 0;
  uint64_t keepalive_timeouts = 0;
  uint64_t connect_timeouts = 0;
  uint64_t session_takeovers = 0;
  uint64_t malformed_packet_disconnects = 0;
  uint64_t outbound_packet_size_drops = 0;
  uint64_t inflight_window_delays = 0;
  uint64_t wills_published = 0;
  uint64_t wills_rejected = 0;
  uint64_t retained_messages = 0;
  uint64_t retained_bytes = 0;
  uint64_t retained_not_stored = 0;
  uint64_t retained_delivery_truncated = 0;

  void add(const ProtocolCounters& counters)
  {
    publish_received += counters.publish_received.load(std::memory_order_relaxed);
    publish_received_bytes += counters.publish_received_bytes.load(std::memory_order_relaxed);
    publish_sent += counters.publish_sent.load(std::memory_order_relaxed);
    publish_sent_bytes += counters.publish_sent_bytes.load(std::memory_order_relaxed);
    bytes_received += counters.bytes_received.load(std::memory_order_relaxed);
    bytes_sent += counters.bytes_sent.load(std::memory_order_relaxed);
    keepalive_timeouts += counters.keepalive_timeouts.load(std::memory_order_relaxed);
    connect_timeouts += counters.connect_timeouts.load(std::memory_order_relaxed);
    session_takeovers += counters.session_takeovers.load(std::memory_order_relaxed);
    malformed_packet_disconnects +=
        counters.malformed_packet_disconnects.load(std::memory_order_relaxed);
    outbound_packet_size_drops +=
        counters.outbound_packet_size_drops.load(std::memory_order_relaxed);
    inflight_window_delays += counters.inflight_window_delays.load(std::memory_order_relaxed);
    wills_published += counters.wills_published.load(std::memory_order_relaxed);
    wills_rejected += counters.wills_rejected.load(std::memory_order_relaxed);
    retained_messages += counters.retained_messages.load(std::memory_order_relaxed);
    retained_bytes += counters.retained_bytes.load(std::memory_order_relaxed);
    retained_not_stored += counters.retained_not_stored.load(std::memory_order_relaxed);
    retained_delivery_truncated +=
        counters.retained_delivery_truncated.load(std::memory_order_relaxed);
  }
};

}  // namespace mqtt
