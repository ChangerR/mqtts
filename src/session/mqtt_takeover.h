#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace mqtt {

class MQTTProtocolHandler;

// 新连接等待旧连接完成退订和注销。done 由旧连接所在的事件线程写入。
struct SessionTakeoverGate
{
  std::atomic<bool> done{false};
};

struct SessionTakeoverRequest
{
  std::string client_id;
  MQTTProtocolHandler* newcomer = nullptr;
  std::shared_ptr<SessionTakeoverGate> gate;
};

}  // namespace mqtt
