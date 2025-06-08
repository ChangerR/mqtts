#include "mqtt_session_manager.h"
#include "mqtt_protocol_handler.h"
#include "mqtt_define.h"
#include "logger.h"
#include <algorithm>
#include <sstream>
#include <chrono>

// 新增错误代码定义
#define MQ_ERR_PARAM -700
#define MQ_ERR_NOT_FOUND -701
#define MQ_ERR_TIMEOUT -702

namespace mqtt {

// SafeHandlerRef实现
SafeHandlerRef::SafeHandlerRef() 
  : handler_(nullptr), mutex_(nullptr), ref_count_(nullptr), cv_(nullptr), acquired_(false) 
{
}

SafeHandlerRef::SafeHandlerRef(MQTTProtocolHandler* handler, std::mutex* mutex, std::atomic<int>* ref_count, std::condition_variable* cv)
  : handler_(handler), mutex_(mutex), ref_count_(ref_count), cv_(cv), acquired_(false)
{
  if (handler_ && mutex_ && ref_count_) {
    std::lock_guard<std::mutex> lock(*mutex_);
    if (handler_) {  // 再次检查，防止竞态条件
      ref_count_->fetch_add(1);
      acquired_ = true;
    } else {
      handler_ = nullptr;
    }
  }
}

SafeHandlerRef::~SafeHandlerRef()
{
  release();
}

SafeHandlerRef::SafeHandlerRef(SafeHandlerRef&& other) noexcept
  : handler_(other.handler_), mutex_(other.mutex_), 
    ref_count_(other.ref_count_), cv_(other.cv_), acquired_(other.acquired_)
{
  other.handler_ = nullptr;
  other.mutex_ = nullptr;
  other.ref_count_ = nullptr;
  other.cv_ = nullptr;
  other.acquired_ = false;
}

SafeHandlerRef& SafeHandlerRef::operator=(SafeHandlerRef&& other) noexcept
{
  if (this != &other) {
    release();
    
    handler_ = other.handler_;
    mutex_ = other.mutex_;
    ref_count_ = other.ref_count_;
    cv_ = other.cv_;
    acquired_ = other.acquired_;
    
    other.handler_ = nullptr;
    other.mutex_ = nullptr;
    other.ref_count_ = nullptr;
    other.cv_ = nullptr;
    other.acquired_ = false;
  }
  return *this;
}

void SafeHandlerRef::release()
{
  if (acquired_ && ref_count_ && mutex_) {
    std::lock_guard<std::mutex> lock(*mutex_);
    int old_count = ref_count_->fetch_sub(1);
    if (old_count == 1 && cv_) {
      // 最后一个引用，通知等待的线程
      cv_->notify_all();
    }
    acquired_ = false;
  }
  handler_ = nullptr;
  mutex_ = nullptr;
  ref_count_ = nullptr;
  cv_ = nullptr;
}

// SessionManager实现
SessionManager::SessionManager()
{
  LOG_INFO("SessionManager initialized with thread-safe handler access");
}

SessionManager::~SessionManager()
{
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  sessions_.clear();
  valid_handlers_.clear();
  LOG_INFO("SessionManager destroyed, all sessions cleared");
}

int SessionManager::register_session(const MQTTString& client_id, MQTTProtocolHandler* handler)
{
  if (!handler) {
    LOG_ERROR("Cannot register session: handler is null");
    return MQ_ERR_PARAM;
  }

  std::lock_guard<std::mutex> lock(sessions_mutex_);
  
  std::string client_id_str = from_mqtt_string(client_id);
  
  // 检查是否已存在相同的client_id
  const std::unordered_map<std::string, std::unique_ptr<SessionInfo>>::iterator existing = sessions_.find(client_id_str);
  if (existing != sessions_.end()) {
    LOG_WARN("Client ID already exists, replacing old session: {}", client_id_str);
    // 标记旧session为无效
    existing->second->is_valid.store(false);
    existing->second->pending_release.store(true);
    // 从有效handler集合中移除旧的handler
    valid_handlers_.erase(existing->second->handler);
  }

  // 注册新的会话
  sessions_[client_id_str] = std::unique_ptr<SessionInfo>(new SessionInfo(handler));
  valid_handlers_.insert(handler);
  
  LOG_INFO("Session registered: {} (total sessions: {})", 
           client_id_str, sessions_.size());
  
  return MQ_SUCCESS;
}

int SessionManager::unregister_session(const MQTTString& client_id, bool wait_for_completion)
{
  std::unique_lock<std::mutex> lock(sessions_mutex_);
  
  std::string client_id_str = from_mqtt_string(client_id);
  const std::unordered_map<std::string, std::unique_ptr<SessionInfo>>::iterator it = sessions_.find(client_id_str);
  if (it == sessions_.end()) {
    LOG_WARN("Attempt to unregister non-existent session: {}", client_id_str);
    return MQ_ERR_NOT_FOUND;
  }

  SessionInfo& info = *it->second;
  
  // 从有效handler集合中移除
  valid_handlers_.erase(info.handler);
  // 标记为无效和等待释放
  info.is_valid.store(false);
  info.pending_release.store(true);
  
  if (wait_for_completion) {
    // 等待所有使用完成
    bool success = wait_for_handler_idle(info, 5000);  // 5秒超时
    if (!success) {
      LOG_WARN("Timeout waiting for handler idle for session: {}", client_id_str);
      return MQ_ERR_TIMEOUT;
    }
  }
  
  LOG_INFO("Session unregistered: {} (remaining sessions: {})", 
           client_id_str, sessions_.size() - 1);
  
  return MQ_SUCCESS;
}

SafeHandlerRef SessionManager::get_safe_handler(const MQTTString& client_id)
{
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  
  std::string client_id_str = from_mqtt_string(client_id);
  const std::unordered_map<std::string, std::unique_ptr<SessionInfo>>::iterator it = sessions_.find(client_id_str);
  
  if (it != sessions_.end() && is_session_active(*it->second)) {
    SessionInfo& info = *it->second;
    return SafeHandlerRef(info.handler, &info.handler_mutex, &info.ref_count, &info.handler_cv);
  }
  
  return SafeHandlerRef();  // 返回空引用
}

bool SessionManager::prepare_handler_release(MQTTProtocolHandler* handler, int timeout_ms)
{
  std::unique_lock<std::mutex> lock(sessions_mutex_);
  
  // 查找对应的session
  for (const std::pair<const std::string, std::unique_ptr<SessionInfo>>& session : sessions_) {
    SessionInfo& info = *session.second;
    if (info.handler == handler) {
      // 标记为等待释放
      info.pending_release.store(true);
      info.is_valid.store(false);
      valid_handlers_.erase(handler);
      
      if (timeout_ms > 0) {
        // 等待handler空闲
        return wait_for_handler_idle(info, timeout_ms);
      }
      return true;
    }
  }
  
  // 没有找到对应的handler
  return true;
}

size_t SessionManager::get_session_count() const
{
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  
  // 只计算有效的会话
  size_t count = 0;
  for (const std::pair<const std::string, std::unique_ptr<SessionInfo>>& session : sessions_) {
    if (is_session_active(*session.second)) {
      count++;
    }
  }
  return count;
}

std::vector<MQTTString> SessionManager::get_all_client_ids() const
{
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  
  std::vector<MQTTString> client_ids;
  client_ids.reserve(sessions_.size());
  
  for (const std::pair<const std::string, std::unique_ptr<SessionInfo>>& session : sessions_) {
    if (is_session_active(*session.second)) {
      client_ids.push_back(to_mqtt_string(session.first, nullptr));
    }
  }
  
  return client_ids;
}

int SessionManager::forward_publish(const MQTTString& topic, const PublishPacket& packet, 
                                  const MQTTString& sender_client_id)
{
  // 获取所有session的拷贝以避免长时间持锁
  std::vector<std::pair<std::string, SafeHandlerRef>> active_sessions;
  
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::string sender_id_str = from_mqtt_string(sender_client_id);
    
    active_sessions.reserve(sessions_.size());
    for (const std::pair<const std::string, std::unique_ptr<SessionInfo>>& session : sessions_) {
      const std::string& client_id_str = session.first;
      SessionInfo& info = *session.second;
      
      // 跳过发送者本身
      if (client_id_str == sender_id_str) {
        continue;
      }
      
      // 获取安全的handler引用
      if (is_session_active(info)) {
        SafeHandlerRef safe_ref(info.handler, &info.handler_mutex, &info.ref_count, &info.handler_cv);
        if (safe_ref.is_valid()) {
          active_sessions.emplace_back(client_id_str, std::move(safe_ref));
        }
      }
    }
  }
  
  // 在不持锁的情况下转发消息
  int forwarded_count = 0;
  for (std::pair<std::string, SafeHandlerRef>& session : active_sessions) {
    try {
      SafeHandlerRef& handler_ref = session.second;
      if (!handler_ref.is_valid()) {
        continue;
      }
      
      // 检查是否订阅了该主题
      const MQTTVector<MQTTString>& subscriptions = handler_ref->get_subscriptions();
      bool subscribed = false;
      for (const MQTTString& subscription : subscriptions) {
        if (is_topic_match(topic, subscription)) {
          subscribed = true;
          break;
        }
      }
      
      if (subscribed) {
        // TODO: 实现实际的转发逻辑
        // handler_ref->send_publish_message(packet);
        forwarded_count++;
        LOG_DEBUG("Would forward message to client: {}", session.first);
      }
    } catch (...) {
      LOG_WARN("Failed to forward message to client: {}", session.first);
    }
  }
  
  LOG_DEBUG("Forwarded PUBLISH message to {} clients for topic: {}", 
            forwarded_count, from_mqtt_string(topic));
  
  return forwarded_count;
}

int SessionManager::broadcast_publish(const PublishPacket& packet, const MQTTString& exclude_client_id)
{
  // 获取所有session的拷贝以避免长时间持锁
  std::vector<std::pair<std::string, SafeHandlerRef>> active_sessions;
  
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::string exclude_id_str = from_mqtt_string(exclude_client_id);
    
    active_sessions.reserve(sessions_.size());
    for (const std::pair<const std::string, std::unique_ptr<SessionInfo>>& session : sessions_) {
      const std::string& client_id_str = session.first;
      SessionInfo& info = *session.second;
      
      // 跳过要排除的客户端
      if (client_id_str == exclude_id_str) {
        continue;
      }
      
      // 获取安全的handler引用
      if (is_session_active(info)) {
        SafeHandlerRef safe_ref(info.handler, &info.handler_mutex, &info.ref_count, &info.handler_cv);
        if (safe_ref.is_valid()) {
          active_sessions.emplace_back(client_id_str, std::move(safe_ref));
        }
      }
    }
  }
  
  // 在不持锁的情况下广播消息
  int broadcast_count = 0;
  for (std::pair<std::string, SafeHandlerRef>& session : active_sessions) {
    try {
      SafeHandlerRef& handler_ref = session.second;
      if (handler_ref.is_valid()) {
        // TODO: 实现实际的广播逻辑
        // handler_ref->send_publish_message(packet);
        broadcast_count++;
        LOG_DEBUG("Would broadcast message to client: {}", session.first);
      }
    } catch (...) {
      LOG_WARN("Failed to broadcast message to client: {}", session.first);
    }
  }
  
  LOG_DEBUG("Broadcasted PUBLISH message to {} clients", broadcast_count);
  
  return broadcast_count;
}

bool SessionManager::is_topic_match(const MQTTString& topic, const MQTTString& topic_filter) const
{
  return match_topic_filter(topic, topic_filter);
}

std::vector<MQTTString> SessionManager::get_subscribers(const MQTTString& topic) const
{
  std::vector<MQTTString> subscribers;
  
  // 获取所有session的拷贝以避免长时间持锁
  std::vector<std::pair<std::string, SafeHandlerRef>> active_sessions;
  
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    active_sessions.reserve(sessions_.size());
    
    for (const std::pair<const std::string, std::unique_ptr<SessionInfo>>& session : sessions_) {
      SessionInfo& info = *session.second;
      if (is_session_active(info)) {
        SafeHandlerRef safe_ref(info.handler, &info.handler_mutex, &info.ref_count, &info.handler_cv);
        if (safe_ref.is_valid()) {
          active_sessions.emplace_back(session.first, std::move(safe_ref));
        }
      }
    }
  }
  
  // 在不持锁的情况下检查订阅
  for (std::pair<std::string, SafeHandlerRef>& session : active_sessions) {
    try {
      SafeHandlerRef& handler_ref = session.second;
      if (!handler_ref.is_valid()) {
        continue;
      }
      
      const MQTTVector<MQTTString>& subscriptions = handler_ref->get_subscriptions();
      for (const MQTTString& subscription : subscriptions) {
        if (is_topic_match(topic, subscription)) {
          subscribers.push_back(to_mqtt_string(session.first, nullptr));
          break;
        }
      }
    } catch (...) {
      // 忽略异常，继续处理其他session
    }
  }
  
  return subscribers;
}

int SessionManager::cleanup_invalid_sessions()
{
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  
  int cleaned_count = 0;
  std::unordered_map<std::string, std::unique_ptr<SessionInfo>>::iterator it = sessions_.begin();
  
  while (it != sessions_.end()) {
    SessionInfo& info = *it->second;
    
    // 检查session是否无效且没有被使用
    if (!is_session_active(info) && info.ref_count.load() == 0) {
      LOG_INFO("Cleaning up invalid session: {}", it->first);
      it = sessions_.erase(it);
      cleaned_count++;
    } else {
      ++it;
    }
  }
  
  if (cleaned_count > 0) {
    LOG_INFO("Cleaned up {} invalid sessions (remaining: {})", 
             cleaned_count, sessions_.size());
  }
  
  return cleaned_count;
}

void SessionManager::set_message_forwarder(const MessageForwarder& forwarder)
{
  std::lock_guard<std::mutex> lock(forwarder_mutex_);
  message_forwarder_ = forwarder;
  LOG_INFO("Message forwarder callback set");
}

bool SessionManager::is_session_active(const SessionInfo& info) const
{
  // 检查会话是否有效、handler是否在有效集合中、以及是否已连接
  return info.is_valid.load() && 
         !info.pending_release.load() &&
         valid_handlers_.find(info.handler) != valid_handlers_.end() && 
         info.handler && 
         info.handler->is_connected();
}

bool SessionManager::wait_for_handler_idle(SessionInfo& info, int timeout_ms)
{
  std::unique_lock<std::mutex> lock(info.handler_mutex);
  
  std::chrono::steady_clock::time_point deadline = 
    std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  
  return info.handler_cv.wait_until(lock, deadline, [&info]() {
    return info.ref_count.load() == 0;
  });
}

bool SessionManager::match_topic_filter(const MQTTString& topic, const MQTTString& filter) const
{
  // 简化版的MQTT主题匹配实现
  // 支持通配符 '+' (单级) 和 '#' (多级)
  
  if (topic.empty() && filter.empty()) {
    return true;
  }
  
  if (filter.empty()) {
    return false;
  }
  
  // 如果过滤器是 '#'，匹配所有主题
  if (filter == MQTTString(1, '#')) {
    return true;
  }
  
  // 将主题和过滤器分割成级别
  std::vector<MQTTString> topic_levels;
  std::vector<MQTTString> filter_levels;
  
  // 分割主题
  size_t start = 0;
  size_t pos = 0;
  while ((pos = topic.find('/', start)) != MQTTString::npos) {
    topic_levels.push_back(topic.substr(start, pos - start));
    start = pos + 1;
  }
  if (start < topic.size()) {
    topic_levels.push_back(topic.substr(start));
  }
  
  // 分割过滤器
  start = 0;
  pos = 0;
  while ((pos = filter.find('/', start)) != MQTTString::npos) {
    filter_levels.push_back(filter.substr(start, pos - start));
    start = pos + 1;
  }
  if (start < filter.size()) {
    filter_levels.push_back(filter.substr(start));
  }
  
  // 执行匹配
  size_t topic_idx = 0;
  size_t filter_idx = 0;
  
  while (filter_idx < filter_levels.size() && topic_idx < topic_levels.size()) {
    const MQTTString& filter_level = filter_levels[filter_idx];
    const MQTTString& topic_level = topic_levels[topic_idx];
    
    if (filter_level == MQTTString(1, '#')) {
      // 多级通配符，匹配剩余所有级别
      return true;
    } else if (filter_level == MQTTString(1, '+')) {
      // 单级通配符，匹配当前级别
      topic_idx++;
      filter_idx++;
    } else if (filter_level == topic_level) {
      // 精确匹配
      topic_idx++;
      filter_idx++;
    } else {
      // 不匹配
      return false;
    }
  }
  
  // 检查是否都匹配完了
  if (filter_idx < filter_levels.size()) {
    // 过滤器还有剩余，只有当最后一个是'#'时才匹配
    return filter_levels[filter_idx] == MQTTString(1, '#');
  }
  
  return topic_idx == topic_levels.size();
}

}  // namespace mqtt 