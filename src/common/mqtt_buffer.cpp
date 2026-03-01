#include "mqtt_buffer.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "mqtt_define.h"

namespace mqtt {

MQTTBuffer::MQTTBuffer(MQTTAllocator* allocator, size_t initial_capacity)
    : allocator_(allocator), data_(nullptr), size_(0), capacity_(0), read_pos_(0) {
  assert(allocator_ != nullptr && "MQTTBuffer requires a non-null MQTTAllocator");
  if (initial_capacity > 0) {
    (void)reserve(initial_capacity);
  }
}

MQTTBuffer::MQTTBuffer(const MQTTBuffer& other)
    : allocator_(other.allocator_), data_(nullptr), size_(0), capacity_(0), read_pos_(other.read_pos_) {
  if (other.size_ > 0 && reserve(other.size_)) {
    std::memcpy(data_, other.data_, other.size_);
    size_ = other.size_;
  }
}

MQTTBuffer& MQTTBuffer::operator=(const MQTTBuffer& other) {
  if (this == &other) {
    return *this;
  }

  clear();
  if (data_) {
    free_bytes(data_, capacity_);
    data_ = nullptr;
    capacity_ = 0;
  }

  allocator_ = other.allocator_;
  read_pos_ = other.read_pos_;
  if (other.size_ > 0 && reserve(other.size_)) {
    std::memcpy(data_, other.data_, other.size_);
    size_ = other.size_;
  }
  return *this;
}

MQTTBuffer::MQTTBuffer(MQTTBuffer&& other) noexcept
    : allocator_(other.allocator_),
      data_(other.data_),
      size_(other.size_),
      capacity_(other.capacity_),
      read_pos_(other.read_pos_) {
  other.data_ = nullptr;
  other.size_ = 0;
  other.capacity_ = 0;
  other.read_pos_ = 0;
}

MQTTBuffer& MQTTBuffer::operator=(MQTTBuffer&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (data_) {
    free_bytes(data_, capacity_);
  }

  allocator_ = other.allocator_;
  data_ = other.data_;
  size_ = other.size_;
  capacity_ = other.capacity_;
  read_pos_ = other.read_pos_;

  other.data_ = nullptr;
  other.size_ = 0;
  other.capacity_ = 0;
  other.read_pos_ = 0;

  return *this;
}

MQTTBuffer::~MQTTBuffer() {
  if (data_) {
    free_bytes(data_, capacity_);
    data_ = nullptr;
  }
}

void MQTTBuffer::clear() {
  size_ = 0;
  read_pos_ = 0;
}

BufferResult MQTTBuffer::resize(size_t new_size) {
  if (new_size > capacity_ && !reserve(new_size)) {
    return BufferResult(MQ_ERR_MEMORY_ALLOC);
  }

  size_ = new_size;
  if (read_pos_ > size_) {
    read_pos_ = size_;
  }
  return BufferResult(MQ_SUCCESS);
}

void MQTTBuffer::reset_read_pos() {
  read_pos_ = 0;
}

BufferResult MQTTBuffer::reserve(size_t capacity) {
  if (capacity <= capacity_) {
    return BufferResult(MQ_SUCCESS);
  }

  uint8_t* new_data = allocate_bytes(capacity);
  if (!new_data) {
    return BufferResult(MQ_ERR_MEMORY_ALLOC);
  }

  if (data_ != nullptr) {
    if (size_ > 0) {
      std::memcpy(new_data, data_, size_);
    }
    free_bytes(data_, capacity_);
  }

  data_ = new_data;
  capacity_ = capacity;
  return BufferResult(MQ_SUCCESS);
}

BufferResult MQTTBuffer::ensure_capacity(size_t needed) {
  if (needed <= capacity_) {
    return BufferResult(MQ_SUCCESS);
  }

  size_t new_capacity = capacity_ == 0 ? 64 : capacity_;
  while (new_capacity < needed) {
    new_capacity *= 2;
  }
  return reserve(new_capacity);
}

BufferResult MQTTBuffer::append(const void* data, size_t len) {
  if (len == 0) {
    return BufferResult(MQ_SUCCESS);
  }
  if (!data) {
    return BufferResult(MQ_ERR_INVALID_ARGS);
  }

  const size_t needed = size_ + len;
  if (!ensure_capacity(needed)) {
    return BufferResult(MQ_ERR_MEMORY_ALLOC);
  }

  std::memcpy(data_ + size_, data, len);
  size_ += len;
  return BufferResult(MQ_SUCCESS);
}

BufferResult MQTTBuffer::append(const uint8_t* data, size_t len) {
  return append(static_cast<const void*>(data), len);
}

BufferResult MQTTBuffer::append(const char* data, size_t len) {
  return append(static_cast<const void*>(data), len);
}

BufferResult MQTTBuffer::append_byte(uint8_t value) {
  return append(&value, sizeof(value));
}

BufferResult MQTTBuffer::push_back(uint8_t value) {
  return append_byte(value);
}

BufferResult MQTTBuffer::append_string(const MQTTString& value) {
  return append(value.data(), value.size());
}

BufferResult MQTTBuffer::append_cstr(const char* value) {
  if (value == nullptr) {
    return BufferResult(MQ_ERR_INVALID_ARGS);
  }
  return append(value, std::strlen(value));
}

BufferResult MQTTBuffer::append_decimal_uint(uint64_t value) {
  char digits[32];
  size_t index = sizeof(digits);

  do {
    digits[--index] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0);

  return append(digits + index, sizeof(digits) - index);
}

BufferResult MQTTBuffer::append_decimal_int(int64_t value) {
  uint64_t abs_value = 0;
  if (value < 0) {
    if (!append_byte(static_cast<uint8_t>('-'))) {
      return BufferResult(MQ_ERR_MEMORY_ALLOC);
    }
    abs_value = static_cast<uint64_t>(-(value + 1));
    abs_value += 1;
  } else {
    abs_value = static_cast<uint64_t>(value);
  }

  return append_decimal_uint(abs_value);
}

BufferResult MQTTBuffer::insert(size_t pos, uint8_t value) {
  if (pos > size_) {
    return BufferResult(MQ_ERR_INVALID_ARGS);
  }

  if (!ensure_capacity(size_ + 1)) {
    return BufferResult(MQ_ERR_MEMORY_ALLOC);
  }

  if (pos < size_) {
    std::memmove(data_ + pos + 1, data_ + pos, size_ - pos);
  }
  data_[pos] = value;
  ++size_;
  return BufferResult(MQ_SUCCESS);
}

void MQTTBuffer::set_size(size_t new_size) {
  if (new_size <= capacity_) {
    size_ = new_size;
    if (read_pos_ > size_) {
      read_pos_ = size_;
    }
  }
}

BufferResult MQTTBuffer::put_uint16_be(uint16_t value) {
  uint8_t bytes[2] = {static_cast<uint8_t>((value >> 8) & 0xFF), static_cast<uint8_t>(value & 0xFF)};
  return append(bytes, sizeof(bytes));
}

BufferResult MQTTBuffer::put_uint16_le(uint16_t value) {
  uint8_t bytes[2] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF)};
  return append(bytes, sizeof(bytes));
}

BufferResult MQTTBuffer::put_uint32_be(uint32_t value) {
  uint8_t bytes[4] = {static_cast<uint8_t>((value >> 24) & 0xFF), static_cast<uint8_t>((value >> 16) & 0xFF),
                      static_cast<uint8_t>((value >> 8) & 0xFF), static_cast<uint8_t>(value & 0xFF)};
  return append(bytes, sizeof(bytes));
}

BufferResult MQTTBuffer::put_uint32_le(uint32_t value) {
  uint8_t bytes[4] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF),
                      static_cast<uint8_t>((value >> 16) & 0xFF), static_cast<uint8_t>((value >> 24) & 0xFF)};
  return append(bytes, sizeof(bytes));
}

BufferResult MQTTBuffer::put_uint64_be(uint64_t value) {
  uint8_t bytes[8];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<uint8_t>((value >> (56 - (i * 8))) & 0xFF);
  }
  return append(bytes, sizeof(bytes));
}

BufferResult MQTTBuffer::put_uint64_le(uint64_t value) {
  uint8_t bytes[8];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
  }
  return append(bytes, sizeof(bytes));
}

bool MQTTBuffer::get_byte(uint8_t& value) {
  if (remaining() < 1) {
    return false;
  }
  value = data_[read_pos_++];
  return true;
}

bool MQTTBuffer::get_uint16_be(uint16_t& value) {
  if (remaining() < 2) {
    return false;
  }
  value = static_cast<uint16_t>(data_[read_pos_]) << 8 | static_cast<uint16_t>(data_[read_pos_ + 1]);
  read_pos_ += 2;
  return true;
}

bool MQTTBuffer::get_uint16_le(uint16_t& value) {
  if (remaining() < 2) {
    return false;
  }
  value = static_cast<uint16_t>(data_[read_pos_]) | (static_cast<uint16_t>(data_[read_pos_ + 1]) << 8);
  read_pos_ += 2;
  return true;
}

bool MQTTBuffer::get_uint32_be(uint32_t& value) {
  if (remaining() < 4) {
    return false;
  }
  value = (static_cast<uint32_t>(data_[read_pos_]) << 24) | (static_cast<uint32_t>(data_[read_pos_ + 1]) << 16) |
          (static_cast<uint32_t>(data_[read_pos_ + 2]) << 8) | static_cast<uint32_t>(data_[read_pos_ + 3]);
  read_pos_ += 4;
  return true;
}

bool MQTTBuffer::get_uint32_le(uint32_t& value) {
  if (remaining() < 4) {
    return false;
  }
  value = static_cast<uint32_t>(data_[read_pos_]) | (static_cast<uint32_t>(data_[read_pos_ + 1]) << 8) |
          (static_cast<uint32_t>(data_[read_pos_ + 2]) << 16) | (static_cast<uint32_t>(data_[read_pos_ + 3]) << 24);
  read_pos_ += 4;
  return true;
}

bool MQTTBuffer::get_uint64_be(uint64_t& value) {
  if (remaining() < 8) {
    return false;
  }
  value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<uint64_t>(data_[read_pos_ + i]);
  }
  read_pos_ += 8;
  return true;
}

bool MQTTBuffer::get_uint64_le(uint64_t& value) {
  if (remaining() < 8) {
    return false;
  }
  value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | static_cast<uint64_t>(data_[read_pos_ + i]);
  }
  read_pos_ += 8;
  return true;
}

bool MQTTBuffer::get_string(size_t len, MQTTString& value) {
  if (remaining() < len) {
    return false;
  }

  value.assign(reinterpret_cast<const char*>(data_ + read_pos_),
               reinterpret_cast<const char*>(data_ + read_pos_ + len));
  read_pos_ += len;
  return true;
}

MQTTString MQTTBuffer::serialize() const {
  if (size_ == 0) {
    return MQTTString(MQTTStrAllocator(allocator_));
  }

  assert(data_ != nullptr && "MQTTBuffer internal data must not be null when size_ > 0");
  return MQTTString(reinterpret_cast<const char*>(data_), size_, MQTTStrAllocator(allocator_));
}

bool MQTTBuffer::deserialize(const MQTTString& input) {
  clear();
  if (input.empty()) {
    return true;
  }
  return append(input.data(), input.size());
}

uint8_t* MQTTBuffer::allocate_bytes(size_t bytes) {
  if (bytes == 0) {
    return nullptr;
  }
  if (allocator_ == nullptr) {
    assert(false && "MQTTBuffer allocator must not be null");
    return nullptr;
  }
  return static_cast<uint8_t*>(allocator_->allocate(bytes));
}

void MQTTBuffer::free_bytes(uint8_t* ptr, size_t bytes) {
  if (!ptr) {
    return;
  }
  if (allocator_ == nullptr) {
    assert(false && "MQTTBuffer allocator must not be null");
    return;
  }
  allocator_->deallocate(ptr, bytes);
}

}  // namespace mqtt
