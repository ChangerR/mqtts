#ifndef MQTT_SERIALIZE_BUFFER_H
#define MQTT_SERIALIZE_BUFFER_H

#include <cstdint>
#include <cstring>
#include "mqtt_allocator.h"

namespace mqtt {

/**
 * @brief 高效的序列化缓冲区，支持动态扩展和内存复用
 *
 * 这个类专门用于MQTT包的序列化，避免频繁的内存分配/释放
 * 支持增长策略和内存复用，提高性能
 */
class MQTTSerializeBuffer
{
  static constexpr size_t INITIAL_CAPACITY = 256;
  static constexpr size_t MAX_CAPACITY = 1024 * 1024;  // 1MB最大限制
  static constexpr size_t GROWTH_FACTOR = 2;

 public:
  /**
   * @brief 构造函数
   * @param allocator 内存分配器
   */
  explicit MQTTSerializeBuffer(MQTTAllocator* allocator);

  /**
   * @brief 析构函数
   */
  ~MQTTSerializeBuffer();

  // 禁止拷贝构造和赋值
  MQTTSerializeBuffer(const MQTTSerializeBuffer&) = delete;
  MQTTSerializeBuffer& operator=(const MQTTSerializeBuffer&) = delete;

  /**
   * @brief 重置buffer，保留已分配的内存以便复用
   */
  void clear();

  /**
   * @brief 确保buffer有足够的容量
   * @param min_capacity 最小需要的容量
   * @return 0成功，非0失败
   */
  int reserve(size_t min_capacity);

  /**
   * @brief 添加单个字节
   * @param byte 要添加的字节
   * @return 0成功，非0失败
   */
  int push_back(uint8_t byte);

  /**
   * @brief 添加数据块
   * @param data 数据指针
   * @param length 数据长度
   * @return 0成功，非0失败
   */
  int append(const uint8_t* data, size_t length);

  /**
   * @brief 添加数据块（重载版本）
   * @param data 数据
   * @param length 数据长度
   * @return 0成功，非0失败
   */
  int append(const char* data, size_t length);

  /**
   * @brief 获取数据指针
   * @return 数据指针
   */
  uint8_t* data() { return buffer_; }
  const uint8_t* data() const { return buffer_; }

  /**
   * @brief 获取当前大小
   * @return 当前数据大小
   */
  size_t size() const { return size_; }

  /**
   * @brief 获取容量
   * @return 当前容量
   */
  size_t capacity() const { return capacity_; }

  /**
   * @brief 检查是否为空
   * @return true如果为空
   */
  bool empty() const { return size_ == 0; }

  /**
   * @brief 索引访问操作符
   * @param index 索引
   * @return 字节引用
   */
  uint8_t& operator[](size_t index);
  const uint8_t& operator[](size_t index) const;

  /**
   * @brief 在指定位置插入字节
   * @param pos 位置
   * @param byte 字节值
   * @return 0成功，非0失败
   */
  int insert(size_t pos, uint8_t byte);

  /**
   * @brief 获取可用空间大小
   * @return 可用空间大小
   */
  size_t available_space() const { return capacity_ - size_; }

  /**
   * @brief 设置size（用于直接写入buffer后更新大小）
   * @param new_size 新的大小，必须不超过capacity
   */
  void set_size(size_t new_size);

 private:
  /**
   * @brief 扩展容量
   * @param min_capacity 最小需要的容量
   * @return 0成功，非0失败
   */
  int grow(size_t min_capacity);

 private:
  MQTTAllocator* allocator_;
  uint8_t* buffer_;
  size_t capacity_;
  size_t size_;
};

}  // namespace mqtt

#endif  // MQTT_SERIALIZE_BUFFER_H