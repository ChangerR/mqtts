#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

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
}  // namespace journal
}  // namespace mqtt
