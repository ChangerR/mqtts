#pragma once

#include <cstddef>
#include <cstdint>

#include "mqtt_allocator.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {

class BufferResult {
 public:
  BufferResult() : code_(0) {}
  explicit BufferResult(int code) : code_(code) {}
  explicit BufferResult(bool ok) : code_(ok ? 0 : -1) {}

  operator int() const { return code_; }
  operator bool() const { return code_ == 0; }
  bool operator!() const { return code_ != 0; }
  bool ok() const { return code_ == 0; }
  int code() const { return code_; }

 private:
  int code_;
};

class MQTTBuffer {
 public:
  explicit MQTTBuffer(MQTTAllocator* allocator, size_t initial_capacity = 0);
  MQTTBuffer(const MQTTBuffer& other);
  MQTTBuffer& operator=(const MQTTBuffer& other);
  MQTTBuffer(MQTTBuffer&& other) noexcept;
  MQTTBuffer& operator=(MQTTBuffer&& other) noexcept;
  ~MQTTBuffer();

  void clear();
  BufferResult resize(size_t new_size);
  void reset_read_pos();

  BufferResult reserve(size_t capacity);
  BufferResult append(const void* data, size_t len);
  BufferResult append(const uint8_t* data, size_t len);
  BufferResult append(const char* data, size_t len);
  BufferResult append_byte(uint8_t value);
  BufferResult push_back(uint8_t value);
  BufferResult append_string(const MQTTString& value);
  BufferResult append_cstr(const char* value);
  BufferResult append_decimal_uint(uint64_t value);
  BufferResult append_decimal_int(int64_t value);
  BufferResult insert(size_t pos, uint8_t value);
  void set_size(size_t new_size);

  BufferResult put_uint16_be(uint16_t value);
  BufferResult put_uint16_le(uint16_t value);
  BufferResult put_uint32_be(uint32_t value);
  BufferResult put_uint32_le(uint32_t value);
  BufferResult put_uint64_be(uint64_t value);
  BufferResult put_uint64_le(uint64_t value);

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
  size_t available_space() const { return capacity_ - size_; }

  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }
  uint8_t& operator[](size_t index) { return data_[index]; }
  const uint8_t& operator[](size_t index) const { return data_[index]; }

  MQTTString serialize() const;
  bool deserialize(const MQTTString& input);

 private:
  BufferResult ensure_capacity(size_t needed);
  uint8_t* allocate_bytes(size_t bytes);
  void free_bytes(uint8_t* ptr, size_t bytes);

  MQTTAllocator* allocator_;
  uint8_t* data_;
  size_t size_;
  size_t capacity_;
  size_t read_pos_;
};

}  // namespace mqtt
