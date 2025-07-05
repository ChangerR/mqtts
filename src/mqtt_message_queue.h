#ifndef MQTT_MESSAGE_QUEUE_H
#define MQTT_MESSAGE_QUEUE_H

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include "mqtt_parser.h"

namespace mqtt {

/**
 * @brief 共享的消息内容（不包含session特定信息）
 * 用于在多个session之间共享同一消息内容，减少内存占用
 */
struct SharedMessageContent
{
  MQTTString topic_name;                            // 主题名称
  MQTTByteVector payload;                           // 消息载荷
  bool dup;                                         // 重复标志
  uint8_t qos;                                      // 服务质量
  bool retain;                                      // 保留标志
  Properties properties;                            // MQTT v5 属性
  std::chrono::steady_clock::time_point timestamp;  // 消息创建时间戳
  
  // 原始发送者信息
  MQTTString sender_client_id;                      // 发送者客户端ID
  
  // 引用计数（用于内存管理）
  mutable std::atomic<int> ref_count{0};
  
  // 构造函数 - 从消息基本信息创建
  SharedMessageContent(const MQTTString& topic, const MQTTByteVector& msg_payload, 
                      uint8_t msg_qos, bool msg_retain, bool msg_dup,
                      const Properties& props, const MQTTString& sender)
      : topic_name(topic),
        payload(msg_payload),
        dup(msg_dup),
        qos(msg_qos),
        retain(msg_retain),
        properties(props),
        timestamp(std::chrono::steady_clock::now()),
        sender_client_id(sender)
  {
    ref_count.store(0);  // 初始化为0，第一次被智能指针包装时会增加到1
  }
  
  // 增加引用计数
  void add_ref() const
  {
    ref_count.fetch_add(1);
  }
  
  // 减少引用计数
  void release() const
  {
    if (ref_count.fetch_sub(1) == 1) {
      delete this;
    }
  }
};

/**
 * @brief 共享内容的智能指针包装
 */
class SharedMessageContentPtr
{
private:
  SharedMessageContent* content_;
  
public:
  SharedMessageContentPtr() : content_(nullptr) {}
  
  explicit SharedMessageContentPtr(SharedMessageContent* content) : content_(content)
  {
    if (content_) {
      content_->add_ref();
    }
  }
  
  // 拷贝构造
  SharedMessageContentPtr(const SharedMessageContentPtr& other) : content_(other.content_)
  {
    if (content_) {
      content_->add_ref();
    }
  }
  
  // 移动构造
  SharedMessageContentPtr(SharedMessageContentPtr&& other) noexcept : content_(other.content_)
  {
    other.content_ = nullptr;
  }
  
  // 拷贝赋值
  SharedMessageContentPtr& operator=(const SharedMessageContentPtr& other)
  {
    if (this != &other) {
      if (content_) {
        content_->release();
      }
      content_ = other.content_;
      if (content_) {
        content_->add_ref();
      }
    }
    return *this;
  }
  
  // 移动赋值
  SharedMessageContentPtr& operator=(SharedMessageContentPtr&& other) noexcept
  {
    if (this != &other) {
      if (content_) {
        content_->release();
      }
      content_ = other.content_;
      other.content_ = nullptr;
    }
    return *this;
  }
  
  // 析构函数
  ~SharedMessageContentPtr()
  {
    if (content_) {
      content_->release();
    }
  }
  
  // 访问操作
  SharedMessageContent* operator->() const { return content_; }
  SharedMessageContent& operator*() const { return *content_; }
  SharedMessageContent* get() const { return content_; }
  bool is_valid() const { return content_ != nullptr; }
  
  // 重置
  void reset()
  {
    if (content_) {
      content_->release();
      content_ = nullptr;
    }
  }
};

/**
 * @brief 待发送的消息信息
 * 包含共享的消息内容和目标客户端信息
 */
struct PendingMessageInfo
{
  SharedMessageContentPtr content;                  // 共享的消息内容
  MQTTString target_client_id;                      // 目标客户端ID
  std::chrono::steady_clock::time_point enqueue_time;  // 入队时间戳
  
  // 构造函数 - 从消息基本信息创建
  PendingMessageInfo(const MQTTString& topic, const MQTTByteVector& payload, 
                    uint8_t qos, bool retain, bool dup,
                    const Properties& properties, const MQTTString& sender_id,
                    const MQTTString& target_id)
      : content(new SharedMessageContent(topic, payload, qos, retain, dup, properties, sender_id)),
        target_client_id(target_id),
        enqueue_time(std::chrono::steady_clock::now())
  {
  }
  
  // 构造函数 - 从共享内容创建
  PendingMessageInfo(const SharedMessageContentPtr& shared_content, const MQTTString& target_id)
      : content(shared_content),
        target_client_id(target_id),
        enqueue_time(std::chrono::steady_clock::now())
  {
  }
  
  // 获取基本信息
  const MQTTString& get_target_client_id() const { return target_client_id; }
  const MQTTString& get_topic() const { return content->topic_name; }
  const MQTTString& get_sender_client_id() const { return content->sender_client_id; }
  const MQTTByteVector& get_payload() const { return content->payload; }
  uint8_t get_qos() const { return content->qos; }
  bool is_retain() const { return content->retain; }
  bool is_dup() const { return content->dup; }
  const Properties& get_properties() const { return content->properties; }
  size_t get_payload_size() const { return content->payload.size(); }
  
  // 检查消息是否有效
  bool is_valid() const { return content.is_valid(); }
};

/**
 * @brief 兼容性结构 - 保持向后兼容
 * 重定向到新的 PendingMessageInfo
 */
struct PendingMessage : public PendingMessageInfo
{
  // 兼容性构造函数 - 从PublishPacket创建（用于渐进式迁移）
  PendingMessage(const PublishPacket& packet, const MQTTString& target, const MQTTString& sender)
      : PendingMessageInfo(packet.topic_name, packet.payload, packet.qos, 
                          packet.retain, packet.dup, packet.properties, sender, target)
  {
  }
  
  // 从PendingMessageInfo创建
  PendingMessage(const PendingMessageInfo& info)
      : PendingMessageInfo(info)
  {
  }
  
  // 兼容性接口 - 这些方法现在委托给基类
  const MQTTString& get_sender_client_id() const { return PendingMessageInfo::get_sender_client_id(); }
};

/**
 * @brief 消息内容缓存管理器
 * 用于管理共享消息内容，避免重复创建相同的消息内容
 */
class MessageContentCache
{
private:
  mutable std::mutex cache_mutex_;
  std::unordered_map<std::string, SharedMessageContentPtr> content_cache_;
  
  // 生成缓存键
  std::string generate_cache_key(const MQTTString& topic, const MQTTString& sender, 
                                size_t payload_size, uint8_t qos) const
  {
    return from_mqtt_string(topic) + "|" + from_mqtt_string(sender) + "|" + 
           std::to_string(payload_size) + "|" + std::to_string(qos);
  }
  
public:
  /**
   * @brief 获取或创建共享消息内容
   */
  SharedMessageContentPtr get_or_create_content(const MQTTString& topic, 
                                               const MQTTByteVector& payload,
                                               uint8_t qos, bool retain, bool dup,
                                               const Properties& properties,
                                               const MQTTString& sender_id)
  {
    std::string cache_key = generate_cache_key(topic, sender_id, payload.size(), qos);
    
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    // 查找现有内容
    auto it = content_cache_.find(cache_key);
    if (it != content_cache_.end() && it->second.is_valid()) {
      return it->second;
    }
    
    // 创建新内容
    SharedMessageContentPtr new_content(new SharedMessageContent(topic, payload, qos, retain, dup, properties, sender_id));
    content_cache_[cache_key] = new_content;
    
    return new_content;
  }
  
  /**
   * @brief 兼容性接口 - 从PublishPacket创建或获取共享内容
   */
  SharedMessageContentPtr get_or_create_content_from_packet(const PublishPacket& packet, const MQTTString& sender_id)
  {
    return get_or_create_content(packet.topic_name, packet.payload, packet.qos, 
                                packet.retain, packet.dup, packet.properties, sender_id);
  }
  
  /**
   * @brief 清理过期的缓存内容
   * @param max_age_seconds 最大缓存时间（秒）
   */
  void cleanup_expired_content(int max_age_seconds = 300)
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto max_age = std::chrono::seconds(max_age_seconds);
    
    auto it = content_cache_.begin();
    while (it != content_cache_.end()) {
      if (!it->second.is_valid() || 
          (now - it->second->timestamp) > max_age) {
        it = content_cache_.erase(it);
      } else {
        ++it;
      }
    }
  }
  
  /**
   * @brief 获取缓存统计信息
   */
  size_t get_cache_size() const
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return content_cache_.size();
  }
  
  /**
   * @brief 清空所有缓存
   */
  void clear_cache()
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    content_cache_.clear();
  }
};

}  // namespace mqtt

#endif  // MQTT_MESSAGE_QUEUE_H