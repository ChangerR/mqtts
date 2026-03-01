#pragma once

#include <cstddef>
#include <cstdint>

#include "mqtt_allocator.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {

class MQTTBuffer {
 public:
  explicit MQTTBuffer(MQTTAllocator* allocator = nullptr, size_t initial_capacity = 0);
  MQTTBuffer(const MQTTBuffer& other);
  MQTTBuffer& operator=(const MQTTBuffer& other);
  MQTTBuffer(MQTTBuffer&& other) noexcept;
  MQTTBuffer& operator=(MQTTBuffer&& other) noexcept;
  ~MQTTBuffer();

  void clear();
  void reset_read_pos();

  bool reserve(size_t capacity);
  bool append(const void* data, size_t len);
  bool append_byte(uint8_t value);

  bool put_uint16_be(uint16_t value);
  bool put_uint16_le(uint16_t value);
  bool put_uint32_be(uint32_t value);
  bool put_uint32_le(uint32_t value);
  bool put_uint64_be(uint64_t value);
  bool put_uint64_le(uint64_t value);

  bool get_byte(uint8_t& value);
  bool get_uint16_be(uint16_t& value);
  bool get_uint16_le(uint16_t& value);
  bool get_uint32_be(uint32_t& value);
  bool get_uint32_le(uint32_t& value);
  bool get_uint64_be(uint64_t& value);
  bool get_uint64_le(uint64_t& value);
  bool get_string(size_t len, MQTTString& value);

  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  size_t read_pos() const { return read_pos_; }
  size_t remaining() const { return size_ >= read_pos_ ? size_ - read_pos_ : 0; }
  bool empty() const { return size_ == 0; }

  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }

  MQTTString serialize() const;
  bool deserialize(const MQTTString& input);

 private:
  bool ensure_capacity(size_t needed);
  uint8_t* allocate_bytes(size_t bytes);
  void free_bytes(uint8_t* ptr, size_t bytes);

  MQTTAllocator* allocator_;
  uint8_t* data_;
  size_t size_;
  size_t capacity_;
  size_t read_pos_;
};

}  // namespace mqtt
