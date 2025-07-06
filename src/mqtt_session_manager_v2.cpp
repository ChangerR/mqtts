#include "mqtt_session_manager_v2.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_set>
#include "logger.h"
#include "mqtt_define.h"
#include "mqtt_protocol_handler.h"
#include "mqtt_session_allocator.h"

// 新增错误代码定义 (已在mqtt_define.h中定义，这里注释掉避免重复定义)
// #define MQ_ERR_INVALID_ARGS -800
// #define MQ_ERR_TIMEOUT -801
#define MQ_ERR_TIMEOUT_V2 -802
#define MQ_ERR_THREAD_MISMATCH -803

namespace mqtt {

//==============================================================================
// ThreadLocalSessionManager实现
//==============================================================================

ThreadLocalSessionManager::ThreadLocalSessionManager(std::thread::id thread_id)
    : thread_id_(thread_id), has_new_messages_(false),
      session_allocator_(nullptr), queue_allocator_(nullptr), worker_allocator_(nullptr)
{
  LOG_INFO("ThreadLocalSessionManager created for thread: {}",
           std::hash<std::thread::id>{}(thread_id_));

  // 初始化分配器
  initialize_allocators();
  
  // 使用分配器创建容器
  sessions_ = make_session_unordered_map<std::string, std::unique_ptr<SessionInfo>>(session_allocator_);
  pending_messages_ = make_session_queue<PendingMessage>(queue_allocator_);

  // 创建默认的Worker池（4个Worker）
  worker_pool_.reset(new SendWorkerPool(4, 1000));
  worker_pool_->set_session_manager(this);
  worker_pool_->start();
}

ThreadLocalSessionManager::~ThreadLocalSessionManager()
{
  // 首先停止Worker池
  if (worker_pool_) {
    worker_pool_->stop();
    worker_pool_.reset();
  }

  CoroLockGuard sessions_lock(&sessions_mutex_);
  std::lock_guard<std::mutex> queue_lock(queue_mutex_);

  // 标记所有session为无效，等待引用计数归零
  for (const std::pair<const std::string, std::unique_ptr<SessionInfo>>& session : sessions_) {
    SessionInfo* info = session.second.get();
    info->is_valid.store(false);
    info->pending_removal.store(true);

    // 协程安全的等待所有引用释放
    if (!info->wait_for_zero_refs(5000)) {
      LOG_WARN("Timeout waiting for references to session: {}", session.first);
    }
  }

  sessions_.clear();

  // 清空待发送队列
  while (!pending_messages_.empty()) {
    pending_messages_.pop();
  }

  // 通知所有等待的协程退出
  new_message_cond_.broadcast();

  // 清理分配器
  cleanup_allocators();

  LOG_INFO("ThreadLocalSessionManager destroyed for thread: {}",
           std::hash<std::thread::id>{}(thread_id_));
}

int ThreadLocalSessionManager::register_handler(const MQTTString& client_id,
                                                MQTTProtocolHandler* handler)
{
  if (!handler) {
    LOG_ERROR("Cannot register handler: handler is null");
    return MQ_ERR_INVALID_ARGS;
  }

  CoroLockGuard lock(&sessions_mutex_);

  std::string client_id_str = from_mqtt_string(client_id);

  // 检查是否已存在相同的client_id
  SessionUnorderedMap<std::string, std::unique_ptr<SessionInfo>>::iterator existing =
      sessions_.find(client_id_str);
  if (existing != sessions_.end()) {
    LOG_WARN("Client ID already exists in thread, replacing: {}", client_id_str);

    // 安全移除旧session
    SessionInfo* old_info = existing->second.get();
    old_info->is_valid.store(false);
    old_info->pending_removal.store(true);

    // 协程安全的等待所有对旧session的引用释放
    if (!old_info->wait_for_zero_refs(3000)) {
      LOG_WARN("Timeout waiting for old session references: {}", client_id_str);
    }
  }

  // 创建新的SessionInfo使用session allocator
  sessions_[client_id_str] = make_session_unique<SessionInfo>(session_allocator_, handler);

  LOG_INFO("Handler registered in thread: {} (total sessions: {})", client_id_str,
           sessions_.size());

  return MQ_SUCCESS;
}

int ThreadLocalSessionManager::unregister_handler(const MQTTString& client_id)
{
  std::string client_id_str = from_mqtt_string(client_id);

  CoroLockGuard lock(&sessions_mutex_);

  SessionUnorderedMap<std::string, std::unique_ptr<SessionInfo>>::iterator it =
      sessions_.find(client_id_str);

  if (it == sessions_.end()) {
    LOG_WARN("Attempt to unregister non-existent handler: {}", client_id_str);
    return MQ_ERR_ALLOCATOR_NOT_FOUND;
  }

  // 安全移除session
  safe_remove_session(client_id_str, it->second.get());
  sessions_.erase(it);

  LOG_INFO("Handler unregistered from thread: {} (remaining sessions: {})", client_id_str,
           sessions_.size());

  return MQ_SUCCESS;
}

void ThreadLocalSessionManager::safe_remove_session(const std::string& client_id, SessionInfo* info)
{
  // 标记为无效和等待移除
  info->is_valid.store(false);
  info->pending_removal.store(true);

  // 协程安全的等待所有引用释放
  if (!info->wait_for_zero_refs(3000)) {
    LOG_WARN("Timeout waiting for session references during removal: {}", client_id);
  }

  LOG_DEBUG("Session safely removed: {}", client_id);
}

SafeHandlerRef ThreadLocalSessionManager::get_safe_handler(const MQTTString& client_id)
{
  CoroLockGuard lock(&sessions_mutex_);

  std::string client_id_str = from_mqtt_string(client_id);
  SessionUnorderedMap<std::string, std::unique_ptr<SessionInfo>>::iterator it =
      sessions_.find(client_id_str);

  if (it != sessions_.end() && it->second->is_valid.load() && !it->second->pending_removal.load()) {
    return SafeHandlerRef(it->second.get());
  }

  return SafeHandlerRef();  // 返回空引用
}

void ThreadLocalSessionManager::enqueue_message(const PendingMessage& message)
{
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_messages_.push(message);
    has_new_messages_.store(true);
  }

  // 通知等待的协程有新消息到达
  new_message_cond_.signal();

  LOG_DEBUG("Message enqueued for client: {} (queue size: {})",
            from_mqtt_string(message.get_target_client_id()), get_pending_message_count());
}

void ThreadLocalSessionManager::enqueue_shared_message(const SharedMessageContentPtr& content,
                                                       const MQTTString& target_client_id)
{
  if (!content.is_valid()) {
    LOG_ERROR("Cannot enqueue invalid shared message content");
    return;
  }

  PendingMessageInfo message_info(content, target_client_id);
  PendingMessage message(message_info);

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_messages_.push(message);
    has_new_messages_.store(true);
  }

  // 通知等待的协程有新消息到达
  new_message_cond_.signal();

  LOG_DEBUG("Shared message enqueued for client: {} (queue size: {})",
            from_mqtt_string(target_client_id), get_pending_message_count());
}

void ThreadLocalSessionManager::enqueue_shared_messages(
    const SharedMessageContentPtr& content, const std::vector<MQTTString>& target_client_ids)
{
  if (!content.is_valid()) {
    LOG_ERROR("Cannot enqueue invalid shared message content");
    return;
  }

  if (target_client_ids.empty()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    for (const MQTTString& client_id : target_client_ids) {
      PendingMessageInfo message_info(content, client_id);
      PendingMessage message(message_info);
      pending_messages_.push(message);
    }

    has_new_messages_.store(true);
  }

  // 通知等待的协程有新消息到达
  new_message_cond_.signal();

  LOG_DEBUG("Shared messages enqueued for {} clients (queue size: {})", target_client_ids.size(),
            get_pending_message_count());
}

int ThreadLocalSessionManager::process_pending_messages(int max_process_count, int timeout_ms)
{
  // 如果队列为空，等待新消息
  if (get_pending_message_count() == 0) {
    if (!wait_for_new_message(timeout_ms)) {
      return -1;  // 超时
    }
  }

  return internal_process_messages(max_process_count);
}

int ThreadLocalSessionManager::process_pending_messages_nowait(int max_process_count)
{
  if (!worker_pool_ || !worker_pool_->is_running()) {
    LOG_WARN("Worker pool not available, falling back to direct processing");
    return internal_process_messages(max_process_count);
  }

  if (max_process_count < 0) {
    max_process_count = 0;  // 0表示处理所有
  }

  int dispatched_count = 0;

  while (dispatched_count != max_process_count) {
    PendingMessage message(PublishPacket{}, MQTTString(), MQTTString());
    bool has_message = false;

    // 从队列中取出一个消息
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (!pending_messages_.empty()) {
        message = pending_messages_.front();
        pending_messages_.pop();
        has_message = true;

        // 如果队列空了，重置新消息标志
        if (pending_messages_.empty()) {
          has_new_messages_.store(false);
        }
      }
    }

    if (!has_message) {
      break;  // 队列为空
    }

    // 直接从PendingMessage创建Worker任务
    WorkerSendTask task(message);

    int submit_result = worker_pool_->submit_task(task);
    if (submit_result == MQ_SUCCESS) {
      dispatched_count++;
      LOG_DEBUG("Message dispatched to worker pool for client: {}",
                from_mqtt_string(message.target_client_id));
    } else {
      LOG_WARN("Failed to dispatch message to worker pool for client: {}, error: {}",
               from_mqtt_string(message.target_client_id), submit_result);

      // Worker池满了或出错，将消息放回队列头部
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        SessionQueue<PendingMessage> temp_queue = make_session_queue<PendingMessage>(queue_allocator_);
        temp_queue.push(message);
        while (!pending_messages_.empty()) {
          temp_queue.push(pending_messages_.front());
          pending_messages_.pop();
        }
        pending_messages_ = std::move(temp_queue);
        has_new_messages_.store(true);
      }
      break;
    }
  }

  if (dispatched_count > 0) {
    LOG_DEBUG("Dispatched {} messages to worker pool in thread", dispatched_count);
  }

  return dispatched_count;
}

bool ThreadLocalSessionManager::wait_for_new_message(int timeout_ms)
{
  if (has_new_messages_.load()) {
    return true;  // 已经有新消息
  }

  // 使用协程信号量等待新消息
  int result = new_message_cond_.wait(timeout_ms);
  return (result == 0 && has_new_messages_.load());
}

int ThreadLocalSessionManager::internal_process_messages(int max_process_count)
{
  if (max_process_count < 0) {
    max_process_count = 0;  // 0表示处理所有
  }

  int processed_count = 0;

  while (processed_count != max_process_count) {
    PendingMessage message(PublishPacket{}, MQTTString(), MQTTString());
    bool has_message = false;

    // 从队列中取出一个消息
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (!pending_messages_.empty()) {
        message = pending_messages_.front();
        pending_messages_.pop();
        has_message = true;

        // 如果队列空了，重置新消息标志
        if (pending_messages_.empty()) {
          has_new_messages_.store(false);
        }
      }
    }

    if (!has_message) {
      break;  // 队列为空
    }

    // 安全处理消息 - 使用SafeHandlerRef避免Use-After-Free
    try {
      SafeHandlerRef safe_handler = get_safe_handler(message.target_client_id);
      if (safe_handler.is_valid() && is_handler_valid(safe_handler.get())) {
        // 获取session专属锁，实现细粒度并发控制
        std::string client_id_str = from_mqtt_string(message.target_client_id);
        SessionInfo* session_info = nullptr;

        {
          CoroLockGuard session_lookup_lock(&sessions_mutex_);
          SessionUnorderedMap<std::string, std::unique_ptr<SessionInfo>>::iterator it =
              sessions_.find(client_id_str);
          if (it != sessions_.end() && it->second->is_valid.load()) {
            session_info = it->second.get();
          }
        }

        if (session_info) {
          // 使用session专属锁，避免阻塞其他session的处理
          CoroLockGuard handler_lock(&session_info->session_mutex);

          // TODO: 将消息信息传递给 MQTT handler 进行实际发送
          // handler负责packet_id管理和PublishPacket生成，这里只传递消息基本信息
          // safe_handler->send_message(message.get_topic(), message.get_payload(),
          //                           message.get_qos(), message.get_retain(),
          //                           message.is_dup(), message.get_properties());
          processed_count++;

          LOG_DEBUG("Message processed for client: {} (topic: {}, payload size: {})", client_id_str,
                    from_mqtt_string(message.get_topic()), message.get_payload_size());
        }
      } else {
        LOG_WARN("Handler not found or invalid for client: {}",
                 from_mqtt_string(message.target_client_id));
      }
    } catch (const std::exception& e) {
      LOG_ERROR("Exception processing message for client {}: {}",
                from_mqtt_string(message.target_client_id), e.what());
    }
    // SafeHandlerRef析构时自动释放引用计数
  }

  if (processed_count > 0) {
    LOG_DEBUG("Processed {} pending messages in thread", processed_count);
  }

  return processed_count;
}

size_t ThreadLocalSessionManager::get_handler_count() const
{
  CoroLockGuard lock(&sessions_mutex_);

  // 只计算有效的session
  size_t count = 0;
  for (const std::pair<const std::string, std::unique_ptr<SessionInfo>>& session : sessions_) {
    if (session.second->is_valid.load() && is_handler_valid(session.second->handler)) {
      count++;
    }
  }
  return count;
}

size_t ThreadLocalSessionManager::get_pending_message_count() const
{
  std::lock_guard<std::mutex> lock(queue_mutex_);
  return pending_messages_.size();
}

std::vector<MQTTString> ThreadLocalSessionManager::get_all_client_ids() const
{
  CoroLockGuard lock(&sessions_mutex_);

  std::vector<MQTTString> client_ids;
  client_ids.reserve(sessions_.size());

  for (const std::pair<const std::string, std::unique_ptr<SessionInfo>>& session : sessions_) {
    if (session.second->is_valid.load() && is_handler_valid(session.second->handler)) {
      client_ids.push_back(to_mqtt_string(session.first, nullptr));
    }
  }

  return client_ids;
}

int ThreadLocalSessionManager::cleanup_invalid_handlers()
{
  CoroLockGuard lock(&sessions_mutex_);

  int cleaned_count = 0;
  SessionUnorderedMap<std::string, std::unique_ptr<SessionInfo>>::iterator it = sessions_.begin();

  while (it != sessions_.end()) {
    if (!is_handler_valid(it->second->handler)) {
      LOG_INFO("Cleaning up invalid handler: {}", it->first);

      // 安全移除session
      safe_remove_session(it->first, it->second.get());
      it = sessions_.erase(it);
      cleaned_count++;
    } else {
      ++it;
    }
  }

  if (cleaned_count > 0) {
    LOG_INFO("Cleaned up {} invalid handlers in thread (remaining: {})", cleaned_count,
             sessions_.size());
  }

  return cleaned_count;
}

int ThreadLocalSessionManager::configure_worker_pool(size_t worker_count, size_t max_queue_size)
{
  if (worker_pool_ && worker_pool_->is_running()) {
    worker_pool_->stop();
  }

  worker_pool_.reset(new SendWorkerPool(worker_count, max_queue_size));
  worker_pool_->set_session_manager(this);

  int result = worker_pool_->start();
  if (result != MQ_SUCCESS) {
    LOG_ERROR("Failed to start worker pool: {}", result);
    worker_pool_.reset();
    return result;
  }

  LOG_INFO("Worker pool configured with {} workers, max queue size: {}", worker_count,
           max_queue_size);
  return MQ_SUCCESS;
}

SendWorkerPool::Statistics ThreadLocalSessionManager::get_worker_statistics() const
{
  if (worker_pool_) {
    return worker_pool_->get_statistics();
  }

  SendWorkerPool::Statistics empty_stats = {};
  return empty_stats;
}

void ThreadLocalSessionManager::initialize_allocators()
{
  // 为线程创建唯一的allocator标识
  std::stringstream ss;
  ss << "thread_" << std::hash<std::thread::id>{}(thread_id_);
  std::string thread_id_str = ss.str();
  
  // 创建主会话分配器
  int ret = SessionAllocatorManager::get_session_allocator(thread_id_str, 0, session_allocator_);
  if (ret != MQ_SUCCESS) {
    LOG_ERROR("Failed to initialize session allocator for thread: {}, error: {}", thread_id_str, mqtt_error_string(ret));
    return;
  }
  
  // 创建队列分配器
  ret = SessionAllocatorManager::get_session_queue_allocator(thread_id_str, queue_allocator_);
  if (ret != MQ_SUCCESS) {
    LOG_ERROR("Failed to initialize queue allocator for thread: {}, error: {}", thread_id_str, mqtt_error_string(ret));
    return;
  }
  
  // 创建worker分配器
  ret = SessionAllocatorManager::get_session_worker_allocator(thread_id_str, worker_allocator_);
  if (ret != MQ_SUCCESS) {
    LOG_ERROR("Failed to initialize worker allocator for thread: {}, error: {}", thread_id_str, mqtt_error_string(ret));
    return;
  }
  
  LOG_INFO("Initialized allocators for thread: {}", thread_id_str);
}

void ThreadLocalSessionManager::cleanup_allocators()
{
  // 清理线程相关的所有分配器
  std::stringstream ss;
  ss << "thread_" << std::hash<std::thread::id>{}(thread_id_);
  std::string thread_id_str = ss.str();
  
  int ret = SessionAllocatorManager::cleanup_session_allocators(thread_id_str);
  if (ret != MQ_SUCCESS) {
    LOG_WARN("Failed to cleanup session allocators for thread: {}, error: {}", thread_id_str, mqtt_error_string(ret));
  }
  
  session_allocator_ = nullptr;
  queue_allocator_ = nullptr;
  worker_allocator_ = nullptr;
  
  LOG_INFO("Cleaned up allocators for thread: {}", thread_id_str);
}

int ThreadLocalSessionManager::get_memory_usage(size_t& memory_usage) const
{
  memory_usage = 0;
  
  if (session_allocator_) {
    memory_usage += session_allocator_->get_total_memory_usage();
  }
  
  if (queue_allocator_) {
    memory_usage += queue_allocator_->get_total_memory_usage();
  }
  
  if (worker_allocator_) {
    memory_usage += worker_allocator_->get_total_memory_usage();
  }
  
  return MQ_SUCCESS;
}

int ThreadLocalSessionManager::is_memory_limit_exceeded(bool& limit_exceeded) const
{
  limit_exceeded = false;
  
  // 检查主会话分配器是否超过限制
  if (session_allocator_) {
    size_t limit = session_allocator_->get_memory_limit();
    if (limit > 0) {
      limit_exceeded = (session_allocator_->get_total_memory_usage() > limit);
    }
  }
  
  return MQ_SUCCESS;
}

int ThreadLocalSessionManager::set_memory_limit(size_t limit)
{
  // 注意：当前的MQTTAllocator不支持动态设置限制
  // 这里只是记录警告，实际的限制需要在创建时设置
  LOG_WARN("Dynamic memory limit setting not supported. Limit should be set during creation.");
  return MQ_ERR_INVALID_ARGS;
}

int ThreadLocalSessionManager::get_memory_statistics(MemoryStats& stats) const
{
  stats = {};
  
  if (session_allocator_) {
    stats.session_usage = session_allocator_->get_memory_usage();
    stats.limit = session_allocator_->get_memory_limit();
  }
  
  if (queue_allocator_) {
    stats.queue_usage = queue_allocator_->get_memory_usage();
  }
  
  if (worker_allocator_) {
    stats.worker_usage = worker_allocator_->get_memory_usage();
  }
  
  int ret = get_memory_usage(stats.total_usage);
  if (ret != MQ_SUCCESS) {
    return ret;
  }
  
  ret = is_memory_limit_exceeded(stats.limit_exceeded);
  if (ret != MQ_SUCCESS) {
    return ret;
  }
  
  return MQ_SUCCESS;
}



bool ThreadLocalSessionManager::is_handler_valid(MQTTProtocolHandler* handler) const
{
  return handler && handler->is_connected();
}

//==============================================================================
// GlobalSessionManager实现 - 高性能无锁优化版本
//==============================================================================

// 线程本地缓存定义
thread_local ThreadLocalSessionManager* GlobalSessionManager::cached_thread_manager_ = nullptr;

GlobalSessionManager::GlobalSessionManager() : state_(ManagerState::INITIALIZING)
{
  // 初始化全局分配器
  global_allocator_ = MQTTMemoryManager::get_instance().get_root_allocator();
  
  // 初始化消息内容缓存管理器使用专用allocator
  MQTTAllocator* cache_allocator = nullptr;
  int ret = SessionAllocatorManager::get_message_cache_allocator(cache_allocator);
  if (ret != MQ_SUCCESS) {
    LOG_ERROR("Failed to get message cache allocator: {}", mqtt_error_string(ret));
  }
  message_cache_.reset(new MessageContentCache());
  
  // 为主题匹配树创建专用的allocator
  MQTTAllocator* topic_tree_allocator = global_allocator_->create_child("topic_tree", MQTTMemoryTag::MEM_TAG_TOPIC_TREE, 0);
  
  // 初始化高性能主题匹配树
  topic_tree_.reset(new ConcurrentTopicTree(topic_tree_allocator));
  
  LOG_INFO("GlobalSessionManager initialized with high-performance lock-free architecture and topic tree using allocator");
}

GlobalSessionManager::~GlobalSessionManager()
{
  // 使用写锁清理所有数据
  WriteLockGuard managers_lock(managers_mutex_);
  WriteLockGuard client_lock(client_index_mutex_);

  thread_managers_.clear();
  thread_manager_array_.clear();
  client_to_manager_.clear();

  // 清理消息缓存
  message_cache_.reset();
  
  // 清理主题匹配树
  topic_tree_.reset();

  LOG_INFO("GlobalSessionManager destroyed, all managers cleared");
}

void GlobalSessionManager::pre_register_threads(size_t thread_count, size_t reserve_client_count)
{
  if (state_.load() != ManagerState::INITIALIZING) {
    LOG_ERROR("pre_register_threads can only be called in INITIALIZING state");
    return;
  }

  WriteLockGuard managers_lock(managers_mutex_);
  WriteLockGuard client_lock(client_index_mutex_);

  // 预分配容量以避免重新分配
  thread_managers_.reserve(thread_count);
  thread_manager_array_.reserve(thread_count);
  client_to_manager_.reserve(reserve_client_count);

  state_.store(ManagerState::REGISTERING);

  LOG_INFO("Pre-registered capacity for {} threads and {} clients", thread_count,
           reserve_client_count);
}

ThreadLocalSessionManager* GlobalSessionManager::register_thread_manager(std::thread::id thread_id)
{
  if (state_.load() != ManagerState::REGISTERING) {
    LOG_ERROR("register_thread_manager can only be called in REGISTERING state");
    return nullptr;
  }

  WriteLockGuard lock(managers_mutex_);

  // 检查是否已经注册
  std::unordered_map<std::thread::id, std::unique_ptr<ThreadLocalSessionManager>>::iterator it =
      thread_managers_.find(thread_id);
  if (it != thread_managers_.end()) {
    LOG_WARN("Thread manager already registered for thread: {}",
             std::hash<std::thread::id>{}(thread_id));
    return it->second.get();
  }

  // 创建新的线程管理器
  std::unique_ptr<ThreadLocalSessionManager> manager(new ThreadLocalSessionManager(thread_id));
  ThreadLocalSessionManager* manager_ptr = manager.get();

  thread_managers_[thread_id] = std::move(manager);
  thread_manager_array_.push_back(manager_ptr);

  // 设置线程本地缓存
  cached_thread_manager_ = manager_ptr;

  LOG_INFO("Registered thread manager for thread: {} (total: {})",
           std::hash<std::thread::id>{}(thread_id), thread_managers_.size());

  return manager_ptr;
}

void GlobalSessionManager::finalize_thread_registration()
{
  if (state_.load() != ManagerState::REGISTERING) {
    LOG_ERROR("finalize_thread_registration can only be called in REGISTERING state");
    return;
  }

  state_.store(ManagerState::RUNNING);

  LOG_INFO("Thread registration finalized, switched to RUNNING mode with {} threads",
           thread_managers_.size());
}

ThreadLocalSessionManager* GlobalSessionManager::get_thread_manager()
{
  // 优先使用线程本地缓存，避免锁和哈希查找
  if (cached_thread_manager_) {
    return cached_thread_manager_;
  }

  // 如果缓存为空，进行一次查找并缓存结果
  std::thread::id current_thread_id = std::this_thread::get_id();

  if (state_.load() == ManagerState::RUNNING) {
    // 运行时使用读锁，并发性能更好
    ReadLockGuard lock(managers_mutex_);
    std::unordered_map<std::thread::id, std::unique_ptr<ThreadLocalSessionManager>>::iterator it =
        thread_managers_.find(current_thread_id);
    if (it != thread_managers_.end()) {
      cached_thread_manager_ = it->second.get();
      return cached_thread_manager_;
    }
  } else {
    // 注册阶段使用写锁，自动创建
    return register_thread_manager(current_thread_id);
  }

  LOG_WARN("Thread manager not found for thread: {}",
           std::hash<std::thread::id>{}(current_thread_id));
  return nullptr;
}

int GlobalSessionManager::register_session(const MQTTString& client_id,
                                           MQTTProtocolHandler* handler)
{
  if (!handler) {
    LOG_ERROR("Cannot register session: handler is null");
    return MQ_ERR_INVALID_ARGS;
  }

  std::string client_id_str = from_mqtt_string(client_id);
  ThreadLocalSessionManager* thread_manager = get_thread_manager();

  if (!thread_manager) {
    LOG_ERROR("No thread manager available for session registration");
    return MQ_ERR_THREAD_MISMATCH;
  }

  // 检查客户端是否有预设的内存限制
  size_t memory_limit = 0;
  {
    std::lock_guard<std::mutex> lock(memory_limits_mutex_);
    auto it = client_memory_limits_.find(client_id_str);
    if (it != client_memory_limits_.end()) {
      memory_limit = it->second;
    }
  }

  // 为客户端创建专用的session allocator
  if (memory_limit > 0) {
    MQTTAllocator* client_allocator = nullptr;
    int alloc_ret = SessionAllocatorManager::get_session_allocator(client_id_str, memory_limit, client_allocator);
    if (alloc_ret != MQ_SUCCESS) {
      LOG_ERROR("Failed to create session allocator for client: {}, error: {}", client_id_str, mqtt_error_string(alloc_ret));
      return alloc_ret;
    }
    LOG_DEBUG("Created session allocator for client {} with limit: {} bytes", client_id_str, memory_limit);
  }

  // 在线程管理器中注册handler
  int result = thread_manager->register_handler(client_id, handler);
  if (result != MQ_SUCCESS) {
    return result;
  }

  // 更新客户端索引（使用写锁，但这个操作相对较少）
  update_client_index(client_id_str, thread_manager);

  LOG_INFO("Session registered globally: {}", client_id_str);
  return MQ_SUCCESS;
}

int GlobalSessionManager::unregister_session(const MQTTString& client_id)
{
  std::string client_id_str = from_mqtt_string(client_id);

  // 直接获取当前线程的管理器，避免全局查找开销
  ThreadLocalSessionManager* thread_manager = get_thread_manager();
  if (!thread_manager) {
    LOG_ERROR("No thread manager available for session unregistration");
    return MQ_ERR_THREAD_MISMATCH;
  }

  // 从线程管理器中注销
  int result = thread_manager->unregister_handler(client_id);

  // 只有注销成功才从客户端索引中移除
  if (result == MQ_SUCCESS) {
    remove_client_index(client_id_str);
    
    // 清理客户端的专用allocator
    int cleanup_ret = SessionAllocatorManager::cleanup_session_allocators(client_id_str);
    if (cleanup_ret != MQ_SUCCESS) {
      LOG_WARN("Failed to cleanup session allocators for client: {}, error: {}", client_id_str, mqtt_error_string(cleanup_ret));
    }
    
    LOG_INFO("Session unregistered globally: {}", client_id_str);
  }

  return result;
}

ThreadLocalSessionManager* GlobalSessionManager::find_client_manager(const MQTTString& client_id)
{
  std::string client_id_str = from_mqtt_string(client_id);
  return fast_find_client_manager(client_id_str);
}

ThreadLocalSessionManager* GlobalSessionManager::fast_find_client_manager(
    const std::string& client_id_str) const
{
  // 使用读锁进行快速查找，支持高并发
  ReadLockGuard lock(client_index_mutex_);

  std::unordered_map<std::string, ThreadLocalSessionManager*>::const_iterator it =
      client_to_manager_.find(client_id_str);

  return (it != client_to_manager_.end()) ? it->second : nullptr;
}

void GlobalSessionManager::update_client_index(const std::string& client_id,
                                               ThreadLocalSessionManager* manager)
{
  WriteLockGuard lock(client_index_mutex_);
  client_to_manager_[client_id] = manager;
}

void GlobalSessionManager::remove_client_index(const std::string& client_id)
{
  WriteLockGuard lock(client_index_mutex_);
  client_to_manager_.erase(client_id);
}

int GlobalSessionManager::forward_publish(const MQTTString& target_client_id,
                                          const PublishPacket& packet,
                                          const MQTTString& sender_client_id)
{
  ThreadLocalSessionManager* thread_manager = find_client_manager(target_client_id);
  if (!thread_manager) {
    LOG_WARN("Target client not found: {}", from_mqtt_string(target_client_id));
    return MQ_ERR_ALLOCATOR_NOT_FOUND;
  }

  // 将消息加入目标线程的队列
  PendingMessage message(packet, target_client_id, sender_client_id);
  thread_manager->enqueue_message(message);

  LOG_DEBUG("Message forwarded to client: {}", from_mqtt_string(target_client_id));
  return MQ_SUCCESS;
}

int GlobalSessionManager::forward_publish_shared(const MQTTString& target_client_id,
                                                 const SharedMessageContentPtr& content)
{
  if (!content.is_valid()) {
    LOG_ERROR("Cannot forward invalid shared message content");
    return MQ_ERR_INVALID_ARGS;
  }

  ThreadLocalSessionManager* thread_manager = find_client_manager(target_client_id);
  if (!thread_manager) {
    LOG_WARN("Target client not found: {}", from_mqtt_string(target_client_id));
    return MQ_ERR_ALLOCATOR_NOT_FOUND;
  }

  // 使用共享内容加入目标线程的队列
  thread_manager->enqueue_shared_message(content, target_client_id);

  LOG_DEBUG("Shared message forwarded to client: {}", from_mqtt_string(target_client_id));
  return MQ_SUCCESS;
}

SharedMessageContentPtr GlobalSessionManager::get_or_create_shared_content(
    const PublishPacket& packet, const MQTTString& sender_client_id)
{
  if (!message_cache_) {
    LOG_ERROR("Message cache not initialized");
    return SharedMessageContentPtr();
  }

  return message_cache_->get_or_create_content_from_packet(packet, sender_client_id);
}

int GlobalSessionManager::batch_forward_publish(const std::vector<MQTTString>& target_client_ids,
                                                const SharedMessageContentPtr& content)
{
  if (!content.is_valid()) {
    LOG_ERROR("Cannot forward invalid shared message content");
    return MQ_ERR_INVALID_ARGS;
  }

  if (target_client_ids.empty()) {
    return 0;
  }

  // 将客户端按线程分组
  std::unordered_map<ThreadLocalSessionManager*, std::vector<MQTTString>> manager_groups;

  for (const MQTTString& client_id : target_client_ids) {
    ThreadLocalSessionManager* thread_manager = find_client_manager(client_id);
    if (thread_manager) {
      manager_groups[thread_manager].push_back(client_id);
    } else {
      LOG_WARN("Target client not found: {}", from_mqtt_string(client_id));
    }
  }

  int forwarded_count = 0;

  // 批量转发到每个线程
  for (const auto& group : manager_groups) {
    ThreadLocalSessionManager* manager = group.first;
    const std::vector<MQTTString>& client_ids = group.second;

    manager->enqueue_shared_messages(content, client_ids);
    forwarded_count += client_ids.size();
  }

  LOG_DEBUG("Batch forwarded shared message to {} clients across {} threads", forwarded_count,
            manager_groups.size());
  return forwarded_count;
}

int GlobalSessionManager::forward_publish_by_topic(const MQTTString& topic,
                                                   const PublishPacket& packet,
                                                   const MQTTString& sender_client_id)
{
  // 使用高性能主题匹配树查找订阅者
  std::vector<SubscriberInfo> subscribers = find_topic_subscribers(topic);
  int forwarded_count = 0;

  std::string sender_id = from_mqtt_string(sender_client_id);
  for (const SubscriberInfo& subscriber : subscribers) {
    // 避免回环
    if (from_mqtt_string(subscriber.client_id) == sender_id) {
      continue;
    }

    if (forward_publish(subscriber.client_id, packet, sender_client_id) == MQ_SUCCESS) {
      forwarded_count++;
    }
  }

  LOG_DEBUG("Forwarded PUBLISH message to {} subscribers for topic: {}", forwarded_count,
            from_mqtt_string(topic));

  return forwarded_count;
}

int GlobalSessionManager::forward_publish_by_topic_shared(const MQTTString& topic,
                                                          const SharedMessageContentPtr& content)
{
  if (!content.is_valid()) {
    LOG_ERROR("Cannot forward invalid shared message content");
    return MQ_ERR_INVALID_ARGS;
  }

  // 使用高性能主题匹配树查找订阅者
  std::vector<SubscriberInfo> subscribers = find_topic_subscribers(topic);
  if (subscribers.empty()) {
    LOG_DEBUG("No subscribers found for topic: {}", from_mqtt_string(topic));
    return 0;
  }

  // 过滤掉发送者自己（避免回环）
  std::vector<MQTTString> filtered_subscribers;
  std::string sender_id = from_mqtt_string(content->sender_client_id);
  
  for (const SubscriberInfo& subscriber : subscribers) {
    if (from_mqtt_string(subscriber.client_id) != sender_id) {
      filtered_subscribers.push_back(subscriber.client_id);
    }
  }

  if (filtered_subscribers.empty()) {
    LOG_DEBUG("No valid subscribers found for topic: {} (after filtering sender)",
              from_mqtt_string(topic));
    return 0;
  }

  // 使用批量转发优化
  int forwarded_count = batch_forward_publish(filtered_subscribers, content);

  LOG_DEBUG("Forwarded shared PUBLISH message to {} subscribers for topic: {}", forwarded_count,
            from_mqtt_string(topic));

  return forwarded_count;
}

size_t GlobalSessionManager::get_total_session_count() const
{
  ReadLockGuard lock(managers_mutex_);

  size_t total_count = 0;
  for (const ThreadLocalSessionManager* manager : thread_manager_array_) {
    total_count += manager->get_handler_count();
  }

  return total_count;
}

std::vector<MQTTString> GlobalSessionManager::get_all_client_ids() const
{
  ReadLockGuard lock(managers_mutex_);

  std::vector<MQTTString> all_client_ids;

  for (const ThreadLocalSessionManager* manager : thread_manager_array_) {
    std::vector<MQTTString> thread_clients = manager->get_all_client_ids();
    all_client_ids.insert(all_client_ids.end(), thread_clients.begin(), thread_clients.end());
  }

  return all_client_ids;
}

bool GlobalSessionManager::is_topic_match(const MQTTString& topic,
                                          const MQTTString& topic_filter) const
{
  return match_topic_filter(topic, topic_filter);
}

std::vector<MQTTString> GlobalSessionManager::get_subscribers(const MQTTString& topic) const
{
  std::vector<MQTTString> subscribers;
  ReadLockGuard lock(managers_mutex_);

  for (ThreadLocalSessionManager* manager : thread_manager_array_) {
    std::vector<MQTTString> thread_clients = manager->get_all_client_ids();

    for (const MQTTString& client_id : thread_clients) {
      try {
        SafeHandlerRef safe_handler = manager->get_safe_handler(client_id);
        if (safe_handler.is_valid()) {
          MQTTProtocolHandler* handler = safe_handler.get();
          const MQTTVector<MQTTString>& subscriptions = handler->get_subscriptions();
          for (const MQTTString& subscription : subscriptions) {
            if (is_topic_match(topic, subscription)) {
              subscribers.push_back(client_id);
              break;
            }
          }
        }
      } catch (...) {
        // 忽略异常，继续处理其他客户端
      }
    }
  }

  return subscribers;
}

int GlobalSessionManager::cleanup_all_invalid_sessions()
{
  // 使用读锁收集需要清理的管理器
  std::vector<ThreadLocalSessionManager*> managers_to_clean;
  {
    ReadLockGuard lock(managers_mutex_);
    managers_to_clean = thread_manager_array_;
  }

  int total_cleaned = 0;
  for (ThreadLocalSessionManager* manager : managers_to_clean) {
    total_cleaned += manager->cleanup_invalid_handlers();
  }

  // 清理客户端索引中的无效条目
  {
    WriteLockGuard client_lock(client_index_mutex_);

    std::unordered_map<std::string, ThreadLocalSessionManager*>::iterator it =
        client_to_manager_.begin();
    while (it != client_to_manager_.end()) {
      // 检查对应的handler是否有效
      SafeHandlerRef safe_handler =
          it->second->get_safe_handler(to_mqtt_string(it->first, nullptr));
      if (!safe_handler.is_valid()) {
        it = client_to_manager_.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (total_cleaned > 0) {
    LOG_INFO("Cleaned up {} invalid sessions globally", total_cleaned);
  }

  return total_cleaned;
}

bool GlobalSessionManager::match_topic_filter(const MQTTString& topic,
                                              const MQTTString& filter) const
{
  // 复用原有实现的主题匹配逻辑
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

void GlobalSessionManager::cleanup_message_cache(int max_age_seconds)
{
  if (message_cache_) {
    message_cache_->cleanup_expired_content(max_age_seconds);
    LOG_DEBUG("Message cache cleanup completed, max age: {} seconds", max_age_seconds);
  }
}

size_t GlobalSessionManager::get_message_cache_size() const
{
  if (message_cache_) {
    return message_cache_->get_cache_size();
  }
  return 0;
}

// ============================================================================
// 新的主题匹配树相关方法实现
// ============================================================================

int GlobalSessionManager::subscribe_topic(const MQTTString& topic_filter, const MQTTString& client_id, uint8_t qos)
{
  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_INTERNAL;
  }
  
  int result = topic_tree_->subscribe(topic_filter, client_id, qos);
  if (result == MQ_SUCCESS) {
    LOG_DEBUG("Client {} subscribed to topic filter {} with QoS {}", 
              from_mqtt_string(client_id), from_mqtt_string(topic_filter), qos);
  } else {
    LOG_ERROR("Failed to subscribe client {} to topic filter {}: error {}", 
              from_mqtt_string(client_id), from_mqtt_string(topic_filter), result);
  }
  
  return result;
}

int GlobalSessionManager::unsubscribe_topic(const MQTTString& topic_filter, const MQTTString& client_id)
{
  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_INTERNAL;
  }
  
  int result = topic_tree_->unsubscribe(topic_filter, client_id);
  if (result == MQ_SUCCESS) {
    LOG_DEBUG("Client {} unsubscribed from topic filter {}", 
              from_mqtt_string(client_id), from_mqtt_string(topic_filter));
  } else {
    LOG_WARN("Failed to unsubscribe client {} from topic filter {}: error {}", 
             from_mqtt_string(client_id), from_mqtt_string(topic_filter), result);
  }
  
  return result;
}

int GlobalSessionManager::unsubscribe_all_topics(const MQTTString& client_id)
{
  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return 0;
  }
  
  int unsubscribed_count = topic_tree_->unsubscribe_all(client_id);
  LOG_INFO("Unsubscribed client {} from {} topics", 
           from_mqtt_string(client_id), unsubscribed_count);
  
  return unsubscribed_count;
}

std::vector<MQTTString> GlobalSessionManager::get_client_subscriptions(const MQTTString& client_id) const
{
  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return std::vector<MQTTString>();
  }
  
  return topic_tree_->get_client_subscriptions(client_id);
}

std::vector<SubscriberInfo> GlobalSessionManager::find_topic_subscribers(const MQTTString& topic) const
{
  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return std::vector<SubscriberInfo>();
  }
  
  TopicMatchResult result = topic_tree_->find_subscribers(topic);
  // 转换分配器类型
  std::vector<SubscriberInfo> converted_subscribers;
  converted_subscribers.reserve(result.subscribers.size());
  for (const auto& subscriber : result.subscribers) {
    converted_subscribers.push_back(subscriber);
  }
  return converted_subscribers;
}

std::pair<size_t, size_t> GlobalSessionManager::get_topic_tree_stats() const
{
  if (!topic_tree_) {
    return std::make_pair(0, 0);
  }
  
  return std::make_pair(topic_tree_->get_total_subscribers(), topic_tree_->get_total_nodes());
}

size_t GlobalSessionManager::cleanup_topic_tree() const
{
  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return 0;
  }
  
  size_t cleaned_nodes = topic_tree_->cleanup_empty_nodes();
  if (cleaned_nodes > 0) {
    LOG_INFO("Cleaned up {} empty nodes from topic tree", cleaned_nodes);
  }
  
  return cleaned_nodes;
}

// ============================================================================
// 新的内存管理方法实现
// ============================================================================

int GlobalSessionManager::get_global_memory_usage(size_t& memory_usage) const
{
  memory_usage = 0;
  
  // 获取所有线程管理器的内存使用情况
  ReadLockGuard lock(managers_mutex_);
  for (const ThreadLocalSessionManager* manager : thread_manager_array_) {
    size_t manager_usage = 0;
    int ret = manager->get_memory_usage(manager_usage);
    if (ret != MQ_SUCCESS) {
      LOG_WARN("Failed to get memory usage from thread manager: {}", mqtt_error_string(ret));
      continue;
    }
    memory_usage += manager_usage;
  }
  
  // 添加消息缓存的内存使用
  if (message_cache_) {
    // 假设每个缓存项平均占用1KB
    memory_usage += get_message_cache_size() * 1024;
  }
  
  return MQ_SUCCESS;
}

int GlobalSessionManager::get_client_memory_usage(std::unordered_map<std::string, size_t>& client_usage) const
{
  client_usage.clear();
  
  // 获取所有客户端的内存使用情况
  ReadLockGuard index_lock(client_index_mutex_);
  for (const auto& pair : client_to_manager_) {
    const std::string& client_id = pair.first;
    size_t usage = 0;
    int ret = SessionAllocatorManager::get_session_memory_usage(client_id, usage);
    if (ret == MQ_SUCCESS && usage > 0) {
      client_usage[client_id] = usage;
    } else if (ret != MQ_SUCCESS && ret != MQ_ERR_ALLOCATOR_NOT_FOUND) {
      LOG_WARN("Failed to get memory usage for client {}: {}", client_id, mqtt_error_string(ret));
    }
  }
  
  return MQ_SUCCESS;
}

int GlobalSessionManager::set_client_memory_limit(const MQTTString& client_id, size_t limit)
{
  std::string client_id_str = from_mqtt_string(client_id);
  
  std::lock_guard<std::mutex> lock(memory_limits_mutex_);
  client_memory_limits_[client_id_str] = limit;
  
  // 注意：当前实现不支持动态设置现有allocator的限制
  // 新的限制将在下次创建session时生效
  LOG_INFO("Memory limit set for client {}: {} bytes (effective on next session creation)", 
           client_id_str, limit);
  
  return MQ_SUCCESS;
}

int GlobalSessionManager::is_client_memory_limit_exceeded(const MQTTString& client_id, bool& limit_exceeded)
{
  std::string client_id_str = from_mqtt_string(client_id);
  return SessionAllocatorManager::is_session_memory_limit_exceeded(client_id_str, limit_exceeded);
}

int GlobalSessionManager::get_allocator_hierarchy(std::string& hierarchy) const
{
  std::stringstream hierarchy_stream;
  hierarchy_stream << "MQTT Allocator Hierarchy:\n";
  
  // 全局分配器信息
  if (global_allocator_) {
    hierarchy_stream << "  Global Allocator: " << global_allocator_->get_id() 
                     << " (Usage: " << global_allocator_->get_total_memory_usage() << " bytes)\n";
  }
  
  // 线程分配器信息
  ReadLockGuard lock(managers_mutex_);
  for (size_t i = 0; i < thread_manager_array_.size(); ++i) {
    const ThreadLocalSessionManager* manager = thread_manager_array_[i];
    ThreadLocalSessionManager::MemoryStats stats;
    int ret = manager->get_memory_statistics(stats);
    if (ret != MQ_SUCCESS) {
      LOG_WARN("Failed to get memory statistics from thread manager {}: {}", i, mqtt_error_string(ret));
      continue;
    }
    
    hierarchy_stream << "    Thread Manager " << i << ":\n";
    hierarchy_stream << "      Session Usage: " << stats.session_usage << " bytes\n";
    hierarchy_stream << "      Queue Usage: " << stats.queue_usage << " bytes\n";
    hierarchy_stream << "      Worker Usage: " << stats.worker_usage << " bytes\n";
    hierarchy_stream << "      Total Usage: " << stats.total_usage << " bytes\n";
    hierarchy_stream << "      Limit: " << (stats.limit > 0 ? std::to_string(stats.limit) : "unlimited") << " bytes\n";
    hierarchy_stream << "      Limit Exceeded: " << (stats.limit_exceeded ? "Yes" : "No") << "\n";
  }
  
  // 客户端分配器信息
  std::unordered_map<std::string, size_t> client_usage;
  int ret = get_client_memory_usage(client_usage);
  if (ret == MQ_SUCCESS && !client_usage.empty()) {
    hierarchy_stream << "  Client Allocators:\n";
    for (const auto& pair : client_usage) {
      hierarchy_stream << "    " << pair.first << ": " << pair.second << " bytes\n";
    }
  } else if (ret != MQ_SUCCESS) {
    LOG_WARN("Failed to get client memory usage: {}", mqtt_error_string(ret));
  }
  
  hierarchy = hierarchy_stream.str();
  return MQ_SUCCESS;
}

int GlobalSessionManager::cleanup_expired_allocators(int& cleaned_count)
{
  cleaned_count = 0;
  
  // 获取所有有效的客户端ID
  std::unordered_set<std::string> valid_clients;
  {
    ReadLockGuard index_lock(client_index_mutex_);
    for (const auto& pair : client_to_manager_) {
      valid_clients.insert(pair.first);
    }
  }
  
  // 清理无效的客户端分配器
  std::unordered_map<std::string, size_t> client_usage;
  int ret = get_client_memory_usage(client_usage);
  if (ret != MQ_SUCCESS) {
    LOG_ERROR("Failed to get client memory usage for cleanup: {}", mqtt_error_string(ret));
    return ret;
  }
  
  for (const auto& pair : client_usage) {
    const std::string& client_id = pair.first;
    if (valid_clients.find(client_id) == valid_clients.end()) {
      // 客户端已经不存在，清理其分配器
      int cleanup_ret = SessionAllocatorManager::cleanup_session_allocators(client_id);
      if (cleanup_ret == MQ_SUCCESS) {
        cleaned_count++;
        LOG_DEBUG("Cleaned up allocator for expired client: {}", client_id);
      } else {
        LOG_WARN("Failed to cleanup allocator for expired client {}: {}", client_id, mqtt_error_string(cleanup_ret));
      }
    }
  }
  
  if (cleaned_count > 0) {
    LOG_INFO("Cleaned up {} expired allocators", cleaned_count);
  }
  
  return MQ_SUCCESS;
}

}  // namespace mqtt
