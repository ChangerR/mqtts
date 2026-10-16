#pragma once

#include <chrono>
#include <cstdint>

namespace mqtt {

// 每条连接一份，只在接入它的事件线程上使用。截止时间由读协程计算，不为连接另开定时器。
class ConnectionWatch
{
 public:
  void start(std::chrono::steady_clock::time_point established, uint32_t connect_timeout_seconds)
  {
    established_ = established;
    last_packet_ = established;
    connect_timeout_seconds_ = connect_timeout_seconds;
    started_ = true;
  }

  bool started() const { return started_; }
  bool connect_complete() const { return connect_complete_; }
  uint16_t keep_alive_seconds() const { return keep_alive_seconds_; }

  // JSON 等非 MQTT 二进制模式不套用 CONNECT / 保活截止时间。
  void suppress() { suppressed_ = true; }

  void observe_packet(std::chrono::steady_clock::time_point now) { last_packet_ = now; }

  void observe_connect(uint16_t negotiated_keep_alive, std::chrono::steady_clock::time_point now)
  {
    connect_complete_ = true;
    keep_alive_seconds_ = negotiated_keep_alive;
    last_packet_ = now;
  }

  enum class DeadlineKind { None, Connect, KeepAlive };

  // 绝对时间，单位毫秒（steady_clock）。没有截止时间时返回 -1。
  int64_t recv_deadline_ms(DeadlineKind* kind = nullptr) const
  {
    if (kind) {
      *kind = DeadlineKind::None;
    }
    if (!started_ || suppressed_) {
      return -1;
    }
    if (!connect_complete_) {
      if (kind) {
        *kind = DeadlineKind::Connect;
      }
      return epoch_ms(established_) + static_cast<int64_t>(connect_timeout_seconds_) * 1000;
    }
    if (keep_alive_seconds_ == 0) {
      return -1;
    }
    if (kind) {
      *kind = DeadlineKind::KeepAlive;
    }
    return epoch_ms(last_packet_) + static_cast<int64_t>(keep_alive_seconds_) * 1500;
  }

  static int64_t epoch_ms(std::chrono::steady_clock::time_point when)
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
  }

  static std::chrono::steady_clock::time_point now() { return std::chrono::steady_clock::now(); }

 private:
  bool started_ = false;
  bool suppressed_ = false;
  bool connect_complete_ = false;
  uint32_t connect_timeout_seconds_ = 10;
  uint16_t keep_alive_seconds_ = 0;
  std::chrono::steady_clock::time_point established_{};
  std::chrono::steady_clock::time_point last_packet_{};
};

// K = 客户端值。keep_alive_max > 0 且 K 为 0 或 K > max 时，v5 改写为 max；3.x 不改写。
inline uint16_t negotiated_keep_alive_seconds(uint16_t client_keep_alive, uint16_t keep_alive_max,
                                             uint8_t protocol_version)
{
  if (protocol_version >= 5 && keep_alive_max > 0 &&
      (client_keep_alive == 0 || client_keep_alive > keep_alive_max)) {
    return keep_alive_max;
  }
  return client_keep_alive;
}

}  // namespace mqtt
