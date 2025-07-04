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
 * @brief 共享的PUBLISH消息内容（不包含session特定的packet_id）
 * 使用引用计数管理内存，支持多个session共享同一消息内容
 */
struct SharedPublishContent
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
  
  // 构造函数
  SharedPublishContent(const PublishPacket& packet, const MQTTString& sender)
      : topic_name(packet.topic_name),
        payload(packet.payload),
        dup(packet.dup),
        qos(packet.qos),
        retain(packet.retain),
        properties(packet.properties),
        timestamp(std::chrono::steady_clock::now()),
        sender_client_id(sender)
  {
    ref_count.store(1);
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
class SharedPublishContentPtr
{
private:
  SharedPublishContent* content_;
  
public:
  SharedPublishContentPtr() : content_(nullptr) {}
  
  explicit SharedPublishContentPtr(SharedPublishContent* content) : content_(content)
  {
    if (content_) {
      content_->add_ref();
    }
  }
  
  // 拷贝构造
  SharedPublishContentPtr(const SharedPublishContentPtr& other) : content_(other.content_)
  {
    if (content_) {
      content_->add_ref();
    }
  }
  
  // 移动构造
  SharedPublishContentPtr(SharedPublishContentPtr&& other) noexcept : content_(other.content_)
  {
    other.content_ = nullptr;
  }
  
  // 拷贝赋值
  SharedPublishContentPtr& operator=(const SharedPublishContentPtr& other)
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
  SharedPublishContentPtr& operator=(SharedPublishContentPtr&& other) noexcept
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
  ~SharedPublishContentPtr()
  {
    if (content_) {
      content_->release();
    }
  }
  
  // 访问操作
  SharedPublishContent* operator->() const { return content_; }
  SharedPublishContent& operator*() const { return *content_; }
  SharedPublishContent* get() const { return content_; }
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
 * @brief Session特定的PUBLISH消息信息
 * 只包含session特定的数据，共享消息内容通过智能指针管理
 */
struct SessionPublishInfo
{
  SharedPublishContentPtr content;                  // 共享的消息内容
  MQTTString target_client_id;                      // 目标客户端ID
  uint16_t packet_id;                               // session特定的packet_id（0表示需要生成）
  
  // 构造函数 - 从原始PublishPacket创建
  SessionPublishInfo(const PublishPacket& packet, const MQTTString& target, const MQTTString& sender)
      : content(new SharedPublishContent(packet, sender)),
        target_client_id(target),
        packet_id(0)  // 初始为0，表示需要session自己生成
  {
  }
  
  // 构造函数 - 从共享内容创建
  SessionPublishInfo(const SharedPublishContentPtr& shared_content, const MQTTString& target)
      : content(shared_content),
        target_client_id(target),
        packet_id(0)  // 初始为0，表示需要session自己生成
  {
  }
  
  // 生成完整的PublishPacket（由session调用，传入session特定的packet_id）
  PublishPacket generate_publish_packet(uint16_t session_packet_id) const
  {
    if (!content.is_valid()) {
      return PublishPacket();  // 返回空包
    }
    
    PublishPacket packet;
    packet.type = PacketType::PUBLISH;
    packet.dup = content->dup;
    packet.qos = content->qos;
    packet.retain = content->retain;
    packet.topic_name = content->topic_name;
    packet.packet_id = session_packet_id;  // 使用session特定的packet_id
    packet.payload = content->payload;
    packet.properties = content->properties;
    
    return packet;
  }
  
  // 获取消息基本信息（用于日志等）
  const MQTTString& get_topic() const { return content->topic_name; }
  const MQTTString& get_sender() const { return content->sender_client_id; }
  size_t get_payload_size() const { return content->payload.size(); }
  uint8_t get_qos() const { return content->qos; }
  bool is_retain() const { return content->retain; }
  
  // 检查消息是否有效
  bool is_valid() const { return content.is_valid(); }
};

/**
 * @brief 待发送的消息结构（重构后的版本）
 * 使用SessionPublishInfo替代原来的PublishPacket，实现内存共享和延迟packet_id生成
 */
struct PendingMessage
{
  SessionPublishInfo session_info;                  // Session特定的消息信息
  std::chrono::steady_clock::time_point enqueue_time;  // 入队时间戳
  
  // 构造函数 - 从原始PublishPacket创建
  PendingMessage(const PublishPacket& packet, const MQTTString& target, const MQTTString& sender)
      : session_info(packet, target, sender),
        enqueue_time(std::chrono::steady_clock::now())
  {
  }
  
  // 构造函数 - 从SessionPublishInfo创建
  PendingMessage(const SessionPublishInfo& info)
      : session_info(info),
        enqueue_time(std::chrono::steady_clock::now())
  {
  }
  
  // 兼容性接口 - 获取目标客户端ID
  const MQTTString& get_target_client_id() const { return session_info.target_client_id; }
  
  // 兼容性接口 - 获取发送者客户端ID
  const MQTTString& get_sender_client_id() const { return session_info.get_sender(); }
  
  // 生成完整的PublishPacket（由session调用）
  PublishPacket generate_publish_packet(uint16_t session_packet_id) const
  {
    return session_info.generate_publish_packet(session_packet_id);
  }
  
  // 检查消息是否有效
  bool is_valid() const { return session_info.is_valid(); }
};

/**
 * @brief 消息内容缓存管理器
 * 用于管理共享消息内容，避免重复创建相同的消息内容
 */
class MessageContentCache
{
private:
  mutable std::mutex cache_mutex_;
  std::unordered_map<std::string, SharedPublishContentPtr> content_cache_;
  
  // 生成缓存键
  std::string generate_cache_key(const PublishPacket& packet, const MQTTString& sender) const
  {
    // 使用主题+发送者+时间戳的组合作为键
    return from_mqtt_string(packet.topic_name) + "|" + from_mqtt_string(sender) + "|" + 
           std::to_string(packet.payload.size());
  }
  
public:
  /**
   * @brief 获取或创建共享消息内容
   * @param packet 原始PublishPacket
   * @param sender 发送者客户端ID
   * @return 共享的消息内容指针
   */
  SharedPublishContentPtr get_or_create_content(const PublishPacket& packet, const MQTTString& sender)
  {
    std::string cache_key = generate_cache_key(packet, sender);
    
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    // 查找现有内容
    auto it = content_cache_.find(cache_key);
    if (it != content_cache_.end() && it->second.is_valid()) {
      return it->second;
    }
    
    // 创建新内容
    SharedPublishContentPtr new_content(new SharedPublishContent(packet, sender));
    content_cache_[cache_key] = new_content;
    
    return new_content;
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
};

}  // namespace mqtt

#endif  // MQTT_MESSAGE_QUEUE_H