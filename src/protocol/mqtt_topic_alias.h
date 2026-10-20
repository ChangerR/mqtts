#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include "mqtt_packet.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {

// 一条连接上的入站主题别名表。只在接入该连接的事件线程上使用。
// 映射从该连接的 MQTTAllocator 分配，不跨重连。分配失败时调用方发 DISCONNECT 0x97。
// 出站主题别名不在这里。
class InboundTopicAliases
{
 public:
  enum class Result { Unused, Resolved, Invalid, Unknown, Quota };

  explicit InboundTopicAliases(MQTTAllocator* allocator) : allocator_(allocator), entries_(nullptr)
  {
  }

  ~InboundTopicAliases() { reset(); }

  InboundTopicAliases(const InboundTopicAliases&) = delete;
  InboundTopicAliases& operator=(const InboundTopicAliases&) = delete;

  // present 表示 PUBLISH 带了 Topic Alias 属性，包括值为 0。
  // 成功时 topic 是解析后的主题；调用方还要把报文上的 topic_alias 清零。
  Result apply(bool present, uint16_t alias, uint16_t maximum, MQTTString& topic)
  {
    if (!present) {
      return Result::Unused;
    }
    if (alias == 0 || alias > maximum) {
      return Result::Invalid;
    }
    try {
      if (!topic.empty()) {
        MQTTString stored(topic.data(), topic.size(), MQTTStrAllocator(allocator_));
        Map& table = ensure();
        const auto found = table.find(alias);
        if (found == table.end()) {
          table.emplace(alias, std::move(stored));
        } else {
          found->second.assign(stored.data(), stored.size());
        }
        return Result::Resolved;
      }
      if (entries_ == nullptr) {
        return Result::Unknown;
      }
      const auto found = entries_->find(alias);
      if (found == entries_->end()) {
        return Result::Unknown;
      }
      topic.assign(found->second.data(), found->second.size());
      return Result::Resolved;
    } catch (const std::bad_alloc&) {
      return Result::Quota;
    }
  }

 private:
  using Map = MQTTMap<uint16_t, MQTTString>;

  Map& ensure()
  {
    if (entries_ != nullptr) {
      return *entries_;
    }
    if (allocator_ == nullptr) {
      throw std::bad_alloc();
    }
    void* memory = allocator_->allocate(sizeof(Map));
    if (memory == nullptr) {
      throw std::bad_alloc();
    }
    try {
      entries_ =
          new (memory) Map(MQTTSTLAllocator<std::pair<const uint16_t, MQTTString>>(allocator_));
    } catch (...) {
      allocator_->deallocate(memory, sizeof(Map));
      entries_ = nullptr;
      throw;
    }
    return *entries_;
  }

  void reset()
  {
    if (entries_ == nullptr) {
      return;
    }
    entries_->~Map();
    if (allocator_ != nullptr) {
      allocator_->deallocate(entries_, sizeof(Map));
    }
    entries_ = nullptr;
  }

  MQTTAllocator* allocator_;
  Map* entries_;
};

inline ReasonCode inbound_alias_disconnect_reason(InboundTopicAliases::Result result)
{
  switch (result) {
    case InboundTopicAliases::Result::Invalid:
      return ReasonCode::TopicAliasInvalid;
    case InboundTopicAliases::Result::Unknown:
      return ReasonCode::ProtocolError;
    case InboundTopicAliases::Result::Quota:
      return ReasonCode::QuotaExceeded;
    default:
      return ReasonCode::Success;
  }
}

}  // namespace mqtt
