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
}  // namespace journal
}  // namespace mqtt
