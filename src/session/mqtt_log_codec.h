#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mqtt {
namespace journal {
// Versioned little-endian encoding shared by logs, checkpoints and the offline importer.
class Encoder
{
 public:
  std::string data;
  void u8(uint8_t v) { data.push_back(char(v)); }
  void u32(uint32_t v)
  {
    for (int i = 0; i < 4; ++i)
      u8(uint8_t(v >> (8 * i)));
  }
  void u64(uint64_t v)
  {
    for (int i = 0; i < 8; ++i)
      u8(uint8_t(v >> (8 * i)));
  }
  void text(const std::string& s)
  {
    if (s.size() > 0xffffffffULL)
      throw std::runtime_error("journal string too large");
    u32(uint32_t(s.size()));
    data.append(s);
  }
};
class Decoder
{
 public:
  explicit Decoder(const std::string& value) : data_(value) {}
  uint8_t u8()
  {
    need(1);
    return uint8_t(data_[pos_++]);
  }
  uint32_t u32()
  {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= uint32_t(u8()) << (8 * i);
    return v;
  }
  uint64_t u64()
  {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= uint64_t(u8()) << (8 * i);
    return v;
  }
  std::string text()
  {
    size_t n = u32();
    need(n);
    auto s = data_.substr(pos_, n);
    pos_ += n;
    return s;
  }
  void end()
  {
    if (pos_ != data_.size())
      throw std::runtime_error("journal trailing fields");
  }
  size_t remaining() const { return data_.size() - pos_; }

 private:
  const std::string& data_;
  size_t pos_ = 0;
  void need(size_t n)
  {
    if (n > data_.size() - pos_)
      throw std::runtime_error("truncated journal record");
  }
};
uint32_t checksum(const char* data, size_t size);
uint64_t topic_hash(const std::string& text);

// Version 1 subscription body: filter, QoS, version, MQTT option bits.
// A journal record that ends after QoS is the legacy body; options decode as 0.
constexpr uint8_t kStoredSubscriptionVersion = 1;
struct StoredSubscription
{
  uint8_t qos = 0;
  bool no_local = false;
  bool retain_as_published = false;
  uint8_t retain_handling = 0;
};
inline uint8_t subscription_flags(const StoredSubscription& sub)
{
  return static_cast<uint8_t>((sub.qos & 0x03) | (sub.no_local ? 0x04 : 0) |
                              (sub.retain_as_published ? 0x08 : 0) |
                              ((sub.retain_handling & 0x03) << 4));
}
inline void apply_subscription_flags(StoredSubscription& sub, uint8_t flags)
{
  if ((flags & 0xC0) != 0 || ((flags >> 4) & 0x03) == 3)
    throw std::runtime_error("invalid stored subscription options");
  if ((flags & 0x03) != (sub.qos & 0x03))
    throw std::runtime_error("stored subscription QoS mismatch");
  sub.no_local = (flags & 0x04) != 0;
  sub.retain_as_published = (flags & 0x08) != 0;
  sub.retain_handling = static_cast<uint8_t>((flags >> 4) & 0x03);
}
inline void write_subscription_record(Encoder& e, const std::string& filter,
                                      const StoredSubscription& sub)
{
  e.text(filter);
  e.u8(sub.qos);
  e.u8(kStoredSubscriptionVersion);
  e.u8(subscription_flags(sub));
}
inline void read_subscription_options(Decoder& d, StoredSubscription& sub)
{
  const uint8_t version = d.u8();
  if (version != kStoredSubscriptionVersion)
    throw std::runtime_error("unsupported subscription record version");
  apply_subscription_flags(sub, d.u8());
}
// Legacy journal subscriptions have no tail. A versioned tail must be complete.
inline void read_legacy_subscription_options(Decoder& d, StoredSubscription& sub)
{
  if (d.remaining() == 0)
    return;
  read_subscription_options(d, sub);
}

// Retain log version 1. A missing retain log is an empty set (journals written
// before retain persistence). Subscription records are unchanged.
constexpr uint8_t kStoredRetainVersion = 2;
constexpr uint8_t kRetainSet = 1;
constexpr uint8_t kRetainClear = 2;
struct StoredRetain
{
  std::string topic;
  uint8_t qos = 0;
  int64_t expires_ms = 0;
  std::string payload;
  std::string sender;
  bool has_payload_format = false;
  uint8_t payload_format = 0;
  std::string content_type;
  std::string response_topic;
  std::string correlation_data;
  std::vector<std::pair<std::string, std::string>> user_properties;
};
inline void write_retain_properties(Encoder& e, const StoredRetain& item)
{
  uint8_t flags = 0;
  if (item.has_payload_format)
    flags = static_cast<uint8_t>(flags | 1u);
  if (!item.content_type.empty())
    flags = static_cast<uint8_t>(flags | 2u);
  if (!item.response_topic.empty())
    flags = static_cast<uint8_t>(flags | 4u);
  if (!item.correlation_data.empty())
    flags = static_cast<uint8_t>(flags | 8u);
  if (!item.user_properties.empty())
    flags = static_cast<uint8_t>(flags | 16u);
  e.u8(flags);
  if (flags & 1u)
    e.u8(item.payload_format);
  if (flags & 2u)
    e.text(item.content_type);
  if (flags & 4u)
    e.text(item.response_topic);
  if (flags & 8u)
    e.text(item.correlation_data);
  if (flags & 16u) {
    e.u32(static_cast<uint32_t>(item.user_properties.size()));
    for (size_t i = 0; i < item.user_properties.size(); ++i) {
      e.text(item.user_properties[i].first);
      e.text(item.user_properties[i].second);
    }
  }
}
inline void read_retain_properties(Decoder& d, StoredRetain& item)
{
  const uint8_t flags = d.u8();
  if (flags & 1u) {
    item.has_payload_format = true;
    item.payload_format = d.u8();
  }
  if (flags & 2u)
    item.content_type = d.text();
  if (flags & 4u)
    item.response_topic = d.text();
  if (flags & 8u)
    item.correlation_data = d.text();
  if (flags & 16u) {
    const uint32_t count = d.u32();
    item.user_properties.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      std::string key = d.text();
      std::string value = d.text();
      item.user_properties.emplace_back(std::move(key), std::move(value));
    }
  }
}
inline void write_retain_set(Encoder& e, const StoredRetain& item)
{
  e.u8(kRetainSet);
  e.u8(kStoredRetainVersion);
  e.text(item.topic);
  e.u8(item.qos);
  e.u64(static_cast<uint64_t>(item.expires_ms));
  e.text(item.payload);
  e.text(item.sender);
  write_retain_properties(e, item);
}
inline void write_retain_clear(Encoder& e, const std::string& topic)
{
  e.u8(kRetainClear);
  e.u8(kStoredRetainVersion);
  e.text(topic);
}
// cleared is true for a delete record. The decoder must consume the whole body.
inline StoredRetain read_retain_record(Decoder& d, bool& cleared)
{
  const uint8_t kind = d.u8();
  const uint8_t version = d.u8();
  if (version != 1 && version != kStoredRetainVersion)
    throw std::runtime_error("unsupported retain record version");
  StoredRetain item;
  item.topic = d.text();
  if (item.topic.empty() || item.topic.find('+') != std::string::npos ||
      item.topic.find('#') != std::string::npos || item.topic.find('\0') != std::string::npos)
    throw std::runtime_error("invalid retained topic");
  if (kind == kRetainClear) {
    cleared = true;
    d.end();
    return item;
  }
  if (kind != kRetainSet)
    throw std::runtime_error("invalid retain journal record");
  cleared = false;
  item.qos = d.u8();
  if (item.qos > 2)
    throw std::runtime_error("invalid retained qos");
  item.expires_ms = static_cast<int64_t>(d.u64());
  if (item.expires_ms < 0)
    throw std::runtime_error("invalid retained expiry");
  item.payload = d.text();
  if (item.payload.empty())
    throw std::runtime_error("empty retained payload");
  item.sender = d.text();
  if (version >= 2)
    read_retain_properties(d, item);
  d.end();
  return item;
}
}  // namespace journal
}  // namespace mqtt
