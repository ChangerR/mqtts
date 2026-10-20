#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace mqtt {

class MQTTProtocolHandler;

// 新连接等待旧连接完成退订和注销。done 由旧连接所在的事件线程写入。
// phase：0 尚未开始，1 已由旧连接线程接手，2 新连接超时已取消。
struct SessionTakeoverGate
{
  std::atomic<bool> done{false};
  std::atomic<int> phase{0};

  bool arm()
  {
    int expected = 0;
    return phase.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
  }

  bool cancel()
  {
    int expected = 0;
    if (!phase.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
      return false;
    }
    done.store(true, std::memory_order_release);
    return true;
  }
};

struct SessionTakeoverRequest
{
  std::string client_id;
  MQTTProtocolHandler* newcomer = nullptr;
  std::shared_ptr<SessionTakeoverGate> gate;
};

}  // namespace mqtt
