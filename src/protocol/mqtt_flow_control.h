#pragma once

#include <cstdint>
#include <cstring>

namespace mqtt {

// 报文标识符占用表。固定 8KiB，随 Handler 一起从连接分配器分配，插入不再另申请内存。
// 只在连接所在的事件线程上使用。
struct PacketIdSet {
  uint64_t word[1024];
  uint16_t count;

  PacketIdSet() : word(), count(0) {}

  bool test(uint16_t id) const
  {
    if (id == 0) {
      return false;
    }
    return (word[id >> 6] >> (id & 63)) & 1ull;
  }

  bool add(uint16_t id)
  {
    if (id == 0 || test(id)) {
      return false;
    }
    word[id >> 6] |= 1ull << (id & 63);
    ++count;
    return true;
  }

  bool erase(uint16_t id)
  {
    if (!test(id)) {
      return false;
    }
    word[id >> 6] &= ~(1ull << (id & 63));
    --count;
    return true;
  }
};

// 一条连接上的入站上限、出站窗口和 QoS 2 去重。
// 在线路径分配报文标识符时，用 claim 回调询问持久化路径是否已占用该标识符。
class ConnectionFlow {
 public:
  enum class Qos2Admit { Forward, Duplicate, Exceeded };
  enum class AllocateStatus { Ready, WindowFull, Busy, Quota, Exhausted };
  enum class AckKind { PubAck, PubRec, PubComp };
  enum class AckDisposition { Absent, Unexpected, Complete, Continue };

  struct Allocation {
    AllocateStatus status;
    uint16_t id;
  };

  ConnectionFlow()
      : server_max_packet_size_(1024 * 1024),
        server_receive_maximum_(65535),
        max_qos_(2),
        client_receive_maximum_(65535),
        client_maximum_packet_size_(268435455),
        next_packet_id_(1)
  {
  }

  void set_server_limits(uint32_t max_packet_size, uint16_t receive_maximum, uint8_t max_qos)
  {
    server_max_packet_size_ = max_packet_size ? max_packet_size : 1;
    server_receive_maximum_ = receive_maximum ? receive_maximum : 1;
    max_qos_ = max_qos;
  }

  void observe_client(uint16_t receive_maximum, uint32_t maximum_packet_size)
  {
    client_receive_maximum_ = receive_maximum ? receive_maximum : 65535;
    client_maximum_packet_size_ = maximum_packet_size ? maximum_packet_size : 268435455;
  }

  uint32_t inbound_limit() const { return server_max_packet_size_; }
  uint16_t server_receive_maximum() const { return server_receive_maximum_; }
  uint8_t max_qos() const { return max_qos_; }
  uint16_t client_receive_maximum() const { return client_receive_maximum_; }
  uint32_t client_maximum_packet_size() const { return client_maximum_packet_size_; }
  uint16_t outbound_inflight() const { return outbound_.count; }
  uint16_t inbound_qos2_waiting() const { return inbound_qos2_.count; }
  bool outbound_holds(uint16_t id) const { return outbound_.test(id); }

  bool inbound_too_large(size_t total_bytes) const
  {
    return total_bytes > server_max_packet_size_;
  }

  bool outbound_too_large(size_t serialized_bytes) const
  {
    return serialized_bytes > client_maximum_packet_size_;
  }

  bool outbound_window_full() const
  {
    return outbound_.count >= client_receive_maximum_ || outbound_.count >= 65535;
  }

  // claim 返回 0 表示调用方已独占该标识符，1 表示已被占用，2 表示锁忙，3 表示内存不足。
  // qos >= 2 记为等待 PUBREC；否则等待 PUBACK。
  Allocation allocate_outbound(int (*claim)(uint16_t, void*), void* arg, uint8_t qos = 0)
  {
    Allocation result{AllocateStatus::WindowFull, 0};
    if (outbound_window_full()) {
      return result;
    }
    for (int tried = 0; tried < 65535; ++tried) {
      const uint16_t id = next_packet_id_;
      next_packet_id_ = next_packet_id_ == 65535 ? 1 : static_cast<uint16_t>(next_packet_id_ + 1);
      if (outbound_.test(id)) {
        continue;
      }
      const int claimed = claim ? claim(id, arg) : 0;
      if (claimed == 2) {
        result.status = AllocateStatus::Busy;
        return result;
      }
      if (claimed == 3) {
        result.status = AllocateStatus::Quota;
        return result;
      }
      if (claimed != 0) {
        continue;
      }
      outbound_.add(id);
      if (qos >= 2) {
        outbound_qos2_.add(id);
      }
      result.status = AllocateStatus::Ready;
      result.id = id;
      return result;
    }
    result.status = AllocateStatus::Exhausted;
    return result;
  }

  void release_outbound(uint16_t id)
  {
    outbound_.erase(id);
    outbound_qos2_.erase(id);
    outbound_awaiting_pubcomp_.erase(id);
  }

  AckDisposition classify_outbound_ack(uint16_t id, AckKind kind) const
  {
    if (!outbound_.test(id)) {
      return AckDisposition::Absent;
    }
    const bool qos2 = outbound_qos2_.test(id);
    const bool awaiting_pubcomp = outbound_awaiting_pubcomp_.test(id);
    if (kind == AckKind::PubAck) {
      return qos2 ? AckDisposition::Unexpected : AckDisposition::Complete;
    }
    if (kind == AckKind::PubRec) {
      return qos2 ? AckDisposition::Continue : AckDisposition::Unexpected;
    }
    if (!qos2 || !awaiting_pubcomp) {
      return AckDisposition::Unexpected;
    }
    return AckDisposition::Complete;
  }

  void mark_awaiting_pubcomp(uint16_t id)
  {
    if (outbound_qos2_.test(id)) {
      outbound_awaiting_pubcomp_.add(id);
    }
  }

  Qos2Admit admit_qos2(uint16_t packet_id)
  {
    if (packet_id != 0 && inbound_qos2_.test(packet_id)) {
      return Qos2Admit::Duplicate;
    }
    if (inbound_qos2_.count >= server_receive_maximum_) {
      return Qos2Admit::Exceeded;
    }
    inbound_qos2_.add(packet_id);
    return Qos2Admit::Forward;
  }

  void release_qos2(uint16_t packet_id) { inbound_qos2_.erase(packet_id); }

 private:
  uint32_t server_max_packet_size_;
  uint16_t server_receive_maximum_;
  uint8_t max_qos_;
  uint16_t client_receive_maximum_;
  uint32_t client_maximum_packet_size_;
  uint16_t next_packet_id_;
  PacketIdSet inbound_qos2_;
  PacketIdSet outbound_;
  PacketIdSet outbound_qos2_;
  PacketIdSet outbound_awaiting_pubcomp_;
};

}  // namespace mqtt
