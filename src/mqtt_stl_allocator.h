#ifndef MQTT_STL_ALLOCATOR_H
#define MQTT_STL_ALLOCATOR_H

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <map>
#include "mqtt_allocator.h"

namespace mqtt {

// STL兼容的分配器适配器
template <typename T>
class MQTTSTLAllocator
{
 public:
  using value_type = T;
  using pointer = T*;
  using const_pointer = const T*;
  using reference = T&;
  using const_reference = const T&;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;

  template <typename U>
  struct rebind
  {
    using other = MQTTSTLAllocator<U>;
  };

  MQTTSTLAllocator() : allocator_(nullptr) {}
  explicit MQTTSTLAllocator(MQTTAllocator* allocator) : allocator_(allocator) {}

  template <typename U>
  MQTTSTLAllocator(const MQTTSTLAllocator<U>& other) : allocator_(other.get_allocator())
  {
  }

  MQTTSTLAllocator(const MQTTSTLAllocator& other) = default;
  MQTTSTLAllocator& operator=(const MQTTSTLAllocator& other) = default;

  pointer allocate(size_type n)
  {
    MQTTAllocator* alloc = allocator_;
    if (!alloc) {
      // 使用root allocator作为默认分配器
      alloc = MQTTMemoryManager::get_instance().get_root_allocator();
    }
    return static_cast<pointer>(alloc->allocate(n * sizeof(T)));
  }

  void deallocate(pointer p, size_type n)
  {
    MQTTAllocator* alloc = allocator_;
    if (!alloc) {
      // 使用root allocator作为默认分配器
      alloc = MQTTMemoryManager::get_instance().get_root_allocator();
    }
    alloc->deallocate(p, n * sizeof(T));
  }

  template <typename U, typename... Args>
  void construct(U* p, Args&&... args)
  {
    new (p) U(std::forward<Args>(args)...);
  }

  template <typename U>
  void destroy(U* p)
  {
    p->~U();
  }

  MQTTAllocator* get_allocator() const { return allocator_; }

  // 获取实际使用的分配器（如果allocator_为空，返回root allocator）
  MQTTAllocator* get_effective_allocator() const
  {
    if (allocator_) {
      return allocator_;
    }
    return MQTTMemoryManager::get_instance().get_root_allocator();
  }

  template <typename U>
  bool operator==(const MQTTSTLAllocator<U>& other) const
  {
    return allocator_ == other.get_allocator();
  }

  template <typename U>
  bool operator!=(const MQTTSTLAllocator<U>& other) const
  {
    return !(*this == other);
  }

 private:
  MQTTAllocator* allocator_;
};

// 使用自定义分配器的容器类型别名
using MQTTStrAllocator = MQTTSTLAllocator<char>;
using MQTTString = std::basic_string<char, std::char_traits<char>, MQTTStrAllocator>;
using MQTTByteVector = std::vector<uint8_t, MQTTSTLAllocator<uint8_t>>;

template <typename T>
using MQTTVector = std::vector<T, MQTTSTLAllocator<T>>;

template <typename Key, typename Value>
using MQTTMap = std::unordered_map<Key, Value, std::hash<Key>, std::equal_to<Key>, MQTTSTLAllocator<std::pair<const Key, Value>>>;

// For compatibility with existing code
template<typename T>
using mqtt_stl_allocator = MQTTSTLAllocator<T>;

using MQTTStringPair = std::pair<MQTTString, MQTTString>;
using MQTTUserProperties = MQTTVector<MQTTStringPair>;

// 辅助函数：从std::string转换为MQTTString
inline MQTTString to_mqtt_string(const std::string& str, MQTTAllocator* allocator)
{
  MQTTStrAllocator alloc(allocator);
  return MQTTString(str.begin(), str.end(), alloc);
}

// 辅助函数：从MQTTString转换为std::string
inline std::string from_mqtt_string(const MQTTString& str)
{
  return std::string(str.begin(), str.end());
}

// 辅助函数：从std::vector转换为MQTTByteVector
inline MQTTByteVector to_mqtt_bytes(const std::vector<uint8_t>& vec, MQTTAllocator* allocator)
{
  MQTTSTLAllocator<uint8_t> alloc(allocator);
  return MQTTByteVector(vec.begin(), vec.end(), alloc);
}

// 辅助函数：从MQTTByteVector转换为std::vector
inline std::vector<uint8_t> from_mqtt_bytes(const MQTTByteVector& vec)
{
  return std::vector<uint8_t>(vec.begin(), vec.end());
}

}  // namespace mqtt

// 为MQTTString提供std::hash特化，支持在std::unordered_map中使用
namespace std {
template <>
struct hash<mqtt::MQTTString> {
    size_t operator()(const mqtt::MQTTString& str) const {
        // 直接对原始数据进行hash，避免字符串转换
        return std::hash<std::string>{}(std::string(str.data(), str.length()));
    }
};
}

#endif  // MQTT_STL_ALLOCATOR_H