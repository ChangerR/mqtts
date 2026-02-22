#include "mqtt_serialize_buffer.h"
#include <algorithm>
#include "logger.h"
#include "mqtt_define.h"

namespace mqtt {

// 定义静态常量
constexpr size_t MQTTSerializeBuffer::INITIAL_CAPACITY;
constexpr size_t MQTTSerializeBuffer::MAX_CAPACITY;
constexpr size_t MQTTSerializeBuffer::GROWTH_FACTOR;

MQTTSerializeBuffer::MQTTSerializeBuffer(MQTTAllocator* allocator)
    : allocator_(allocator), buffer_(nullptr), capacity_(0), size_(0)
{
  if (!allocator_) {
    LOG_ERROR("Invalid allocator for MQTTSerializeBuffer");
    return;
  }

  // 分配初始容量
  buffer_ = static_cast<uint8_t*>(allocator_->allocate(INITIAL_CAPACITY));
  if (buffer_) {
    capacity_ = INITIAL_CAPACITY;
  } else {
    LOG_ERROR("Failed to allocate initial buffer for MQTTSerializeBuffer");
  }
}

MQTTSerializeBuffer::~MQTTSerializeBuffer()
{
  if (buffer_ && allocator_) {
    allocator_->deallocate(buffer_, capacity_);
    buffer_ = nullptr;
    capacity_ = 0;
    size_ = 0;
  }
}

void MQTTSerializeBuffer::clear()
{
  size_ = 0;
  // 保留buffer_和capacity_以便复用
}

int MQTTSerializeBuffer::reserve(size_t min_capacity)
{
  if (min_capacity <= capacity_) {
    return MQ_SUCCESS;
  }

  if (min_capacity > MAX_CAPACITY) {
    LOG_ERROR("Requested capacity {} exceeds maximum {}", min_capacity, MAX_CAPACITY);
    return MQ_ERR_MEMORY_ALLOC;
  }

  return grow(min_capacity);
}

int MQTTSerializeBuffer::push_back(uint8_t byte)
{
  if (size_ >= capacity_) {
    int ret = grow(size_ + 1);
    if (ret != MQ_SUCCESS) {
      return ret;
    }
  }

  buffer_[size_++] = byte;
  return MQ_SUCCESS;
}

int MQTTSerializeBuffer::append(const uint8_t* data, size_t length)
{
  if (!data || length == 0) {
    return MQ_SUCCESS;
  }

  size_t required_capacity = size_ + length;
  if (required_capacity > capacity_) {
    int ret = grow(required_capacity);
    if (ret != MQ_SUCCESS) {
      return ret;
    }
  }

  std::memcpy(buffer_ + size_, data, length);
  size_ += length;
  return MQ_SUCCESS;
}

int MQTTSerializeBuffer::append(const char* data, size_t length)
{
  return append(reinterpret_cast<const uint8_t*>(data), length);
}

uint8_t& MQTTSerializeBuffer::operator[](size_t index)
{
  // 注意：这里不做边界检查以提高性能，调用者需要确保index有效
  return buffer_[index];
}

const uint8_t& MQTTSerializeBuffer::operator[](size_t index) const
{
  // 注意：这里不做边界检查以提高性能，调用者需要确保index有效
  return buffer_[index];
}

int MQTTSerializeBuffer::insert(size_t pos, uint8_t byte)
{
  if (pos > size_) {
    LOG_ERROR("Insert position {} out of range (size: {})", pos, size_);
    return MQ_ERR_PACKET_INVALID;  // 使用现有的错误码
  }

  if (size_ >= capacity_) {
    int ret = grow(size_ + 1);
    if (ret != MQ_SUCCESS) {
      return ret;
    }
  }

  // 移动数据为新字节让出空间
  if (pos < size_) {
    std::memmove(buffer_ + pos + 1, buffer_ + pos, size_ - pos);
  }

  buffer_[pos] = byte;
  size_++;
  return MQ_SUCCESS;
}

void MQTTSerializeBuffer::set_size(size_t new_size)
{
  if (new_size <= capacity_) {
    size_ = new_size;
  } else {
    LOG_ERROR("Attempted to set size {} beyond capacity {}", new_size, capacity_);
  }
}

int MQTTSerializeBuffer::grow(size_t min_capacity)
{
  if (!allocator_) {
    LOG_ERROR("No allocator available for buffer growth");
    return MQ_ERR_MEMORY_ALLOC;
  }

  // 计算新容量，使用增长因子，但至少满足最小要求
  size_t new_capacity = std::max(capacity_ * GROWTH_FACTOR, min_capacity);

  // 确保不超过最大限制
  if (new_capacity > MAX_CAPACITY) {
    if (min_capacity > MAX_CAPACITY) {
      LOG_ERROR("Required capacity {} exceeds maximum {}", min_capacity, MAX_CAPACITY);
      return MQ_ERR_MEMORY_ALLOC;
    }
    new_capacity = MAX_CAPACITY;
  }

  // 分配新buffer
  uint8_t* new_buffer = static_cast<uint8_t*>(allocator_->allocate(new_capacity));
  if (!new_buffer) {
    LOG_ERROR("Failed to allocate new buffer of size {}", new_capacity);
    return MQ_ERR_MEMORY_ALLOC;
  }

  // 复制现有数据
  if (buffer_ && size_ > 0) {
    std::memcpy(new_buffer, buffer_, size_);
  }

  // 释放旧buffer
  if (buffer_) {
    allocator_->deallocate(buffer_, capacity_);
  }

  // 更新状态
  buffer_ = new_buffer;
  capacity_ = new_capacity;

  LOG_DEBUG("Buffer grown from {} to {} bytes", capacity_ / GROWTH_FACTOR, new_capacity);
  return MQ_SUCCESS;
}

}  // namespace mqtt