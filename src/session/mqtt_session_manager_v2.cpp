#include "mqtt_session_manager_v2.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_map>
#include "logger.h"
#include "mqtt_define.h"
#include "mqtt_protocol_handler.h"

// 错误代码已在mqtt_define.h中统一定义

namespace mqtt {

//==============================================================================
// ThreadLocalSessionManager实现
//==============================================================================

ThreadLocalSessionManager::ThreadLocalSessionManager(std::thread::id thread_id, MQTTAllocator* allocator)
    : thread_id_(thread_id),
      allocator_(allocator),
      initialized_(false),
      session_removal_timeout_ms_(3000),
      has_new_messages_(false)
{
  // 构造函数只做简单的成员变量初始化
  // 复杂的初始化逻辑移到init()方法中
}

int ThreadLocalSessionManager::init()
{
  if (initialized_) {
    return MQ_SUCCESS;  // 已经初始化过了
  }

  // Initialize allocator if not provided
  if (!allocator_) {
    std::string child_name = "session_manager_" + std::to_string(std::hash<std::thread::id>{}(thread_id_));
    allocator_ = MQTTMemoryManager::get_instance().get_root_allocator()->create_child(
        child_name, MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
    if (!allocator_) {
      LOG_ERROR("Failed to create allocator for ThreadLocalSessionManager");
      return MQ_ERR_SESSION_MANAGER_NOT_READY;
    }
  }

  LOG_INFO("ThreadLocalSessionManager initialized for thread: {} with allocator: {}",
           std::hash<std::thread::id>{}(thread_id_), allocator_->get_id());

  // Initialize worker pool
  try {
    worker_pool_.reset(new SendWorkerPool(4, 1000));
    worker_pool_->set_session_manager(this);
    worker_pool_->start();
    LOG_DEBUG("SendWorkerPool initialized for thread: {}", std::hash<std::thread::id>{}(thread_id_));
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to initialize SendWorkerPool: {}", e.what());
    return MQ_ERR_SESSION_MANAGER_NOT_READY;
  }

  initialized_ = true;
  return MQ_SUCCESS;
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

  LOG_INFO("ThreadLocalSessionManager destroyed for thread: {}",
           std::hash<std::thread::id>{}(thread_id_));
}

int ThreadLocalSessionManager::register_handler(const MQTTString& client_id,
                                                MQTTProtocolHandler* handler)
{
  if (!initialized_) {
    LOG_ERROR("ThreadLocalSessionManager not initialized");
    return MQ_ERR_SESSION_MANAGER_NOT_READY;
  }
  
  if (!handler) {
    LOG_ERROR("Cannot register handler: handler is null");
    return MQ_ERR_PARAM_V2;
  }

  CoroLockGuard lock(&sessions_mutex_);

  std::string client_id_str = from_mqtt_string(client_id);

  // 检查是否已存在相同的client_id
  std::unordered_map<std::string, std::unique_ptr<SessionInfo>>::iterator existing =
      sessions_.find(client_id_str);
  if (existing != sessions_.end()) {
    LOG_WARN("Client ID already exists in thread, replacing: {}", client_id_str);

    // 安全移除旧session：引用未归零时拒绝替换，避免 UAF
    SessionInfo* old_info = existing->second.get();
    old_info->is_valid.store(false);
    old_info->pending_removal.store(true);

    if (!old_info->wait_for_zero_refs(session_removal_timeout_ms_)) {
      LOG_WARN("Timeout waiting for old session references, keeping existing handler: {}",
               client_id_str);
      old_info->is_valid.store(true);
      old_info->pending_removal.store(false);
      return MQ_ERR_TIMEOUT_V2;
    }
  }

  // 创建新的SessionInfo
  sessions_[client_id_str] = std::unique_ptr<SessionInfo>(new SessionInfo(handler));

  LOG_INFO("Handler registered in thread: {} (total sessions: {})", client_id_str,
           sessions_.size());

  return MQ_SUCCESS;
}

int ThreadLocalSessionManager::unregister_handler(const MQTTString& client_id)
{
  std::string client_id_str = from_mqtt_string(client_id);

  CoroLockGuard lock(&sessions_mutex_);

  auto it = sessions_.find(client_id_str);

  if (it == sessions_.end()) {
    LOG_WARN("Attempt to unregister non-existent handler: {}", client_id_str);
    return MQ_ERR_NOT_FOUND_V2;
  }

  // 安全移除session：引用未归零时保留 SessionInfo，避免 UAF
  int remove_ret = safe_remove_session(client_id_str, it->second.get());
  if (remove_ret != MQ_SUCCESS) {
    LOG_WARN("Failed to unregister handler, session retained: {} (error: {})", client_id_str,
             remove_ret);
    return remove_ret;
  }

  sessions_.erase(it);

  LOG_INFO("Handler unregistered from thread: {} (remaining sessions: {})", client_id_str,
           sessions_.size());

  return MQ_SUCCESS;
}

int ThreadLocalSessionManager::safe_remove_session(const std::string& client_id, SessionInfo* info)
{
  // 标记为无效和等待移除
  info->is_valid.store(false);
  info->pending_removal.store(true);

  // 协程安全的等待所有引用释放
  if (!info->wait_for_zero_refs(session_removal_timeout_ms_)) {
    LOG_WARN("Timeout waiting for session references during removal: {}", client_id);
    info->is_valid.store(true);
    info->pending_removal.store(false);
    return MQ_ERR_TIMEOUT_V2;
  }

  LOG_DEBUG("Session safely removed: {}", client_id);
  return MQ_SUCCESS;
}

SafeHandlerRef ThreadLocalSessionManager::get_safe_handler(const MQTTString& client_id)
{
  if (!initialized_) {
    LOG_ERROR("ThreadLocalSessionManager not initialized");
    return SafeHandlerRef();  // 返回空引用
  }
  
  CoroLockGuard lock(&sessions_mutex_);

  std::string client_id_str = from_mqtt_string(client_id);
  auto it = sessions_.find(client_id_str);

  if (it != sessions_.end() && it->second->is_valid.load() && !it->second->pending_removal.load()) {
    return SafeHandlerRef(it->second.get());
  }

  return SafeHandlerRef();  // 返回空引用
}

int ThreadLocalSessionManager::enqueue_message(const PendingMessage& message)
{
  if (!initialized_) {
    LOG_ERROR("ThreadLocalSessionManager not initialized");
    return MQ_ERR_SESSION_MANAGER_NOT_READY;
  }
  
  int ret = MQ_SUCCESS;

  if (from_mqtt_string(message.get_target_client_id()).empty()) {
    LOG_ERROR("Target client ID cannot be empty");
    ret = MQ_ERR_PARAM_V2;
  } else {
    // Pre-check queue capacity to avoid potential allocation failures
    if (get_pending_message_count() >= 10000) {  // Reasonable queue limit
      LOG_ERROR("Message queue is full, cannot enqueue more messages");
      ret = MQ_ERR_QUEUE_FULL;
    } else {
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
  }

  return ret;
}

int ThreadLocalSessionManager::enqueue_shared_message(const SharedMessageContentPtr& content,
                                                      const MQTTString& target_client_id)
{
  int ret = MQ_SUCCESS;

  if (!content.is_valid()) {
    LOG_ERROR("Cannot enqueue invalid shared message content");
    ret = MQ_ERR_PARAM_V2;
  } else if (from_mqtt_string(target_client_id).empty()) {
    LOG_ERROR("Target client ID cannot be empty");
    ret = MQ_ERR_PARAM_V2;
  } else {
    // Pre-check queue capacity
    if (get_pending_message_count() >= 10000) {
      LOG_ERROR("Message queue is full, cannot enqueue shared message");
      ret = MQ_ERR_QUEUE_FULL;
    } else {
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
  }

  return ret;
}

int ThreadLocalSessionManager::enqueue_shared_messages(
    const SharedMessageContentPtr& content, const std::vector<MQTTString>& target_client_ids)
{
  int ret = MQ_SUCCESS;

  if (!content.is_valid()) {
    LOG_ERROR("Cannot enqueue invalid shared message content");
    ret = MQ_ERR_PARAM_V2;
  } else if (target_client_ids.empty()) {
    LOG_DEBUG("Target client IDs list is empty, nothing to enqueue");
    // 空列表不是错误，但也不需要处理
  } else {
    // Pre-check queue capacity for batch operation
    size_t current_queue_size = get_pending_message_count();
    if (current_queue_size + target_client_ids.size() >= 10000) {
      LOG_ERROR("Message queue would exceed capacity with batch enqueue");
      ret = MQ_ERR_QUEUE_FULL;
    } else {
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        for (const MQTTString& client_id : target_client_ids) {
          if (from_mqtt_string(client_id).empty()) {
            LOG_WARN("Skipping empty client ID in batch enqueue");
            continue;
          }

          PendingMessageInfo message_info(content, client_id);
          PendingMessage message(message_info);
          pending_messages_.push(message);
        }

        has_new_messages_.store(true);
      }

      // 通知等待的协程有新消息到达
      new_message_cond_.signal();

      LOG_DEBUG("Shared messages enqueued for {} clients (queue size: {})",
                target_client_ids.size(), get_pending_message_count());
    }
  }

  return ret;
}

int ThreadLocalSessionManager::enqueue_takeover(const std::string& client_id,
                                                MQTTProtocolHandler* newcomer,
                                                const std::shared_ptr<SessionTakeoverGate>& gate)
{
  if (!initialized_) {
    return MQ_ERR_SESSION_MANAGER_NOT_READY;
  }
  if (!gate) {
    return MQ_ERR_PARAM_V2;
  }

  SessionTakeoverRequest request;
  request.client_id = client_id;
  request.newcomer = newcomer;
  request.gate = gate;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    takeover_requests_.push(std::move(request));
  }
  return MQ_SUCCESS;
}

int ThreadLocalSessionManager::process_control_messages()
{
  if (!initialized_) {
    return 0;
  }

  int processed = 0;
  while (true) {
    SessionTakeoverRequest request;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (takeover_requests_.empty()) {
        break;
      }
      request = std::move(takeover_requests_.front());
      takeover_requests_.pop();
    }

    if (!request.gate || !request.gate->arm()) {
      if (request.gate) {
        request.gate->done.store(true, std::memory_order_release);
      }
      processed++;
      continue;
    }
    SafeHandlerRef current = get_safe_handler(to_mqtt_string(request.client_id, allocator_));
    if (!current.is_valid() || current.get() == request.newcomer) {
      request.gate->done.store(true, std::memory_order_release);
    } else {
      current->request_session_takeover(request.gate);
    }
    processed++;
  }
  return processed;
}

int ThreadLocalSessionManager::process_pending_messages(int max_process_count, int timeout_ms)
{
  // 如果队列为空，等待新消息
  if (get_pending_message_count() == 0) {
    bool has_message = false;
    int ret = wait_for_new_message(timeout_ms, has_message);
    if (MQ_FAIL(ret) || !has_message) {
      return -1;  // 超时或错误
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
        std::queue<PendingMessage> temp_queue;
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

int ThreadLocalSessionManager::wait_for_new_message(int timeout_ms, bool& has_message)
{
  int ret = MQ_SUCCESS;

  if (has_new_messages_.load()) {
    has_message = true;  // 已经有新消息
  } else {
    // 使用协程信号量等待新消息
    int result = new_message_cond_.wait(timeout_ms);
    if (result == 0) {
      has_message = has_new_messages_.load();
    } else if (result == -1) {
      has_message = false;
      ret = MQ_ERR_TIMEOUT_V2;
    } else {
      has_message = false;
      ret = MQ_ERR_INTERNAL;
    }
  }

  return ret;
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
    SafeHandlerRef safe_handler = get_safe_handler(message.target_client_id);
    if (safe_handler.is_valid() && is_handler_valid(safe_handler.get())) {
      // 获取session专属锁，实现细粒度并发控制
      std::string client_id_str = from_mqtt_string(message.target_client_id);
      SessionInfo* session_info = nullptr;

      {
        CoroLockGuard session_lookup_lock(&sessions_mutex_);
        auto it = sessions_.find(client_id_str);
        if (it != sessions_.end() && it->second && it->second->is_valid.load()) {
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
      } else {
        LOG_WARN("Session not found or invalid for client: {}", client_id_str);
      }
    } else {
      LOG_WARN("Handler not found or invalid for client: {}",
               from_mqtt_string(message.target_client_id));
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
  auto it = sessions_.begin();

  while (it != sessions_.end()) {
    if (!is_handler_valid(it->second->handler)) {
      LOG_INFO("Cleaning up invalid handler: {}", it->first);

      // 引用未归零时跳过擦除，避免 UAF
      int remove_ret = safe_remove_session(it->first, it->second.get());
      if (remove_ret != MQ_SUCCESS) {
        ++it;
        continue;
      }
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

bool ThreadLocalSessionManager::is_handler_valid(MQTTProtocolHandler* handler) const
{
  return handler && handler->is_connected();
}

//==============================================================================
// GlobalSessionManager实现 - 高性能无锁优化版本
//==============================================================================

// 线程本地缓存定义
thread_local GlobalSessionManager* GlobalSessionManager::cached_manager_owner_ = nullptr;
thread_local ThreadLocalSessionManager* GlobalSessionManager::cached_thread_manager_ = nullptr;

GlobalSessionManager::GlobalSessionManager() : state_(ManagerState::INITIALIZING), event_forwarding_service_(nullptr), server_id_(MQTTStrAllocator(nullptr))
{
  // 初始化全局分配器
  MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
  global_allocator_ = root_allocator->get_child("global_session_manager");
  if (!global_allocator_) {
    global_allocator_ = root_allocator->create_child("global_session_manager",
                                                     MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
  }
  if (!global_allocator_) {
    // Fallback: avoid null dereference in tests that create/destroy managers repeatedly.
    global_allocator_ = root_allocator;
    LOG_WARN("Failed to create/find global_session_manager allocator, falling back to root allocator");
  }

  // TODO: 简化容器初始化，去除复杂的STL分配器
  // 容器使用默认分配器，后期可以优化

  // 初始化消息内容缓存管理器
  message_cache_.reset(new MessageContentCache());

  // 为主题匹配树创建专用的allocator
  MQTTAllocator* topic_tree_allocator = global_allocator_->get_child("topic_tree");
  if (!topic_tree_allocator) {
    topic_tree_allocator =
        global_allocator_->create_child("topic_tree", MQTTMemoryTag::MEM_TAG_TOPIC_TREE, 0);
  }
  if (!topic_tree_allocator) {
    topic_tree_allocator = global_allocator_;
    LOG_WARN("Failed to create/find topic_tree allocator, using global allocator");
  }

  // 初始化高性能主题匹配树
  topic_tree_.reset(new ConcurrentTopicTree(topic_tree_allocator));

  // 初始化服务器ID为空，由外部设置
  server_id_ = MQTTString(MQTTStrAllocator(global_allocator_));

  LOG_INFO(
      "GlobalSessionManager initialized with high-performance lock-free architecture and topic "
      "tree using allocator: {}", global_allocator_->get_id());
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

  if (global_allocator_ && global_allocator_->get_parent()) {
    global_allocator_->get_parent()->remove_child(global_allocator_->get_id());
    global_allocator_ = nullptr;
  }

  // 清理当前线程上的缓存，避免跨实例悬空指针
  if (cached_manager_owner_ == this) {
    cached_manager_owner_ = nullptr;
    cached_thread_manager_ = nullptr;
  }

  LOG_INFO("GlobalSessionManager destroyed, all managers cleared");
}

int GlobalSessionManager::pre_register_threads(size_t thread_count, size_t reserve_client_count)
{
  if (state_.load() != ManagerState::INITIALIZING) {
    LOG_ERROR("pre_register_threads can only be called in INITIALIZING state");
    return MQ_ERR_INVALID_STATE;
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

  return MQ_SUCCESS;
}

ThreadLocalSessionManager* GlobalSessionManager::register_thread_manager(std::thread::id thread_id)
{
  if (state_.load() != ManagerState::REGISTERING) {
    LOG_ERROR("register_thread_manager can only be called in REGISTERING state");
    return nullptr;
  }

  WriteLockGuard lock(managers_mutex_);

  // 检查是否已经注册
  auto it = thread_managers_.find(thread_id);
  if (it != thread_managers_.end()) {
    LOG_WARN("Thread manager already registered for thread: {}",
             std::hash<std::thread::id>{}(thread_id));
    return it->second.get();
  }

  // 为每个线程创建专用的allocator
  MQTTAllocator* thread_allocator = global_allocator_->create_child("thread_" + std::to_string(std::hash<std::thread::id>{}(thread_id)), MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);

  // 创建新的线程管理器
  std::unique_ptr<ThreadLocalSessionManager> manager(new ThreadLocalSessionManager(thread_id, thread_allocator));
  ThreadLocalSessionManager* manager_ptr = manager.get();
  
  // 初始化线程管理器
  int init_result = manager_ptr->init();
  if (init_result != MQ_SUCCESS) {
    LOG_ERROR("Failed to initialize ThreadLocalSessionManager for thread: {}", 
              std::hash<std::thread::id>{}(thread_id));
    return nullptr;
  }

  thread_managers_[thread_id] = std::move(manager);
  thread_manager_array_.push_back(manager_ptr);

  // 设置线程本地缓存
  cached_manager_owner_ = this;
  cached_thread_manager_ = manager_ptr;

  LOG_INFO("Registered thread manager for thread: {} (total: {})",
           std::hash<std::thread::id>{}(thread_id), thread_managers_.size());

  return manager_ptr;
}

int GlobalSessionManager::finalize_thread_registration()
{
  if (state_.load() != ManagerState::REGISTERING) {
    LOG_ERROR("finalize_thread_registration can only be called in REGISTERING state");
    return MQ_ERR_INVALID_STATE;
  }

  state_.store(ManagerState::RUNNING);

  LOG_INFO("Thread registration finalized, switched to RUNNING mode with {} threads",
           thread_managers_.size());

  return MQ_SUCCESS;
}

ProtocolCounters* GlobalSessionManager::current_thread_counters()
{
  if (cached_manager_owner_ == this && cached_thread_manager_) {
    return &cached_thread_manager_->protocol_counters();
  }
  if (state_.load() != ManagerState::RUNNING) {
    return nullptr;
  }
  ThreadLocalSessionManager* local = get_thread_manager();
  return local ? &local->protocol_counters() : nullptr;
}

void GlobalSessionManager::note_publish_received(uint64_t payload_bytes)
{
  ProtocolCounters* counters = current_thread_counters();
  if (!counters) {
    return;
  }
  counters->publish_received.fetch_add(1, std::memory_order_relaxed);
  counters->publish_received_bytes.fetch_add(payload_bytes, std::memory_order_relaxed);
}

void GlobalSessionManager::note_publish_sent(uint64_t payload_bytes)
{
  ProtocolCounters* counters = current_thread_counters();
  if (!counters) {
    return;
  }
  counters->publish_sent.fetch_add(1, std::memory_order_relaxed);
  counters->publish_sent_bytes.fetch_add(payload_bytes, std::memory_order_relaxed);
}

void GlobalSessionManager::note_bytes_received(uint64_t bytes)
{
  ProtocolCounters* counters = current_thread_counters();
  if (!counters) {
    return;
  }
  counters->bytes_received.fetch_add(bytes, std::memory_order_relaxed);
}

void GlobalSessionManager::note_bytes_sent(uint64_t bytes)
{
  ProtocolCounters* counters = current_thread_counters();
  if (!counters) {
    return;
  }
  counters->bytes_sent.fetch_add(bytes, std::memory_order_relaxed);
}

void GlobalSessionManager::note_connect_timeout()
{
  ProtocolCounters* counters = current_thread_counters();
  if (counters) {
    counters->connect_timeouts.fetch_add(1, std::memory_order_relaxed);
  }
}

void GlobalSessionManager::note_keepalive_timeout()
{
  ProtocolCounters* counters = current_thread_counters();
  if (counters) {
    counters->keepalive_timeouts.fetch_add(1, std::memory_order_relaxed);
  }
}

void GlobalSessionManager::note_session_takeover()
{
  ProtocolCounters* counters = current_thread_counters();
  if (counters) {
    counters->session_takeovers.fetch_add(1, std::memory_order_relaxed);
  }
}

void GlobalSessionManager::note_malformed_disconnect()
{
  ProtocolCounters* counters = current_thread_counters();
  if (counters) {
    counters->malformed_packet_disconnects.fetch_add(1, std::memory_order_relaxed);
  }
}

void GlobalSessionManager::note_outbound_packet_size_drop()
{
  ProtocolCounters* counters = current_thread_counters();
  if (counters) {
    counters->outbound_packet_size_drops.fetch_add(1, std::memory_order_relaxed);
  }
}

void GlobalSessionManager::note_inflight_window_delay()
{
  ProtocolCounters* counters = current_thread_counters();
  if (counters) {
    counters->inflight_window_delays.fetch_add(1, std::memory_order_relaxed);
  }
}

void GlobalSessionManager::collect_protocol_counters(ProtocolCounterSnapshot& out) const
{
  out = ProtocolCounterSnapshot();
  std::vector<const ProtocolCounters*> blocks;
  {
    ReadLockGuard lock(managers_mutex_);
    blocks.reserve(thread_manager_array_.size());
    for (ThreadLocalSessionManager* manager : thread_manager_array_) {
      if (manager) {
        blocks.push_back(&manager->protocol_counters());
      }
    }
  }
  for (const ProtocolCounters* block : blocks) {
    out.add(*block);
  }
}

ThreadLocalSessionManager* GlobalSessionManager::get_thread_manager()
{
  // 优先使用线程本地缓存，避免锁和哈希查找
  if (cached_manager_owner_ == this && cached_thread_manager_) {
    return cached_thread_manager_;
  }

  if (cached_manager_owner_ != this) {
    cached_thread_manager_ = nullptr;
    cached_manager_owner_ = this;
  }

  // 如果缓存为空，进行一次查找并缓存结果
  std::thread::id current_thread_id = std::this_thread::get_id();

  if (state_.load() == ManagerState::RUNNING) {
    // 运行时使用读锁，并发性能更好
    ReadLockGuard lock(managers_mutex_);
    auto it = thread_managers_.find(current_thread_id);
    if (it != thread_managers_.end()) {
      cached_thread_manager_ = it->second.get();
      cached_manager_owner_ = this;
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
    return MQ_ERR_PARAM_V2;
  }

  std::string client_id_str = from_mqtt_string(client_id);
  ThreadLocalSessionManager* thread_manager = get_thread_manager();

  if (!thread_manager) {
    LOG_ERROR("No thread manager available for session registration");
    return MQ_ERR_SESSION_THREAD_MISMATCH;
  }

  // 先占住 Client ID，再投递接管。等待期间不能持有任何 OS 锁。
  auto gate = std::make_shared<SessionTakeoverGate>();
  ThreadLocalSessionManager* existing = nullptr;
  const int timeout_ms = thread_manager->get_session_removal_timeout_ms();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (true) {
    std::shared_ptr<SessionTakeoverGate> blocker;
    {
      WriteLockGuard lock(client_index_mutex_);
      auto reserved = client_reservations_.find(client_id_str);
      // 占位要保留到索引安装完成。只等 done 会让下一个连接在发布前抢走 Client ID。
      if (reserved != client_reservations_.end() && reserved->second && reserved->second != gate) {
        blocker = reserved->second;
      } else {
        client_reservations_[client_id_str] = gate;
        auto found = client_to_manager_.find(client_id_str);
        existing = found == client_to_manager_.end() ? nullptr : found->second;
      }
    }
    if (!blocker) {
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      LOG_WARN("Timed out waiting for in-flight session takeover of {}", client_id_str);
      return MQ_ERR_TIMEOUT_V2;
    }
    int slice_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    if (slice_ms > 10) {
      slice_ms = 10;
    }
    if (slice_ms < 1) {
      slice_ms = 1;
    }
    runtime::current_runtime().wait(-1, 0, slice_ms);
  }

  bool held = true;
  auto release_reservation = [&]() {
    if (!held) {
      return;
    }
    held = false;
    WriteLockGuard lock(client_index_mutex_);
    auto reserved = client_reservations_.find(client_id_str);
    if (reserved != client_reservations_.end() && reserved->second == gate) {
      client_reservations_.erase(reserved);
    }
  };

  if (existing) {
    const int posted = existing->enqueue_takeover(client_id_str, handler, gate);
    if (posted != MQ_SUCCESS) {
      release_reservation();
      return posted;
    }
    const auto takeover_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(existing->get_session_removal_timeout_ms());
    bool finished = false;
    while (!gate->done.load(std::memory_order_acquire)) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= takeover_deadline) {
        break;
      }
      int slice_ms = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(takeover_deadline - now).count());
      if (slice_ms > 10) {
        slice_ms = 10;
      }
      if (slice_ms < 1) {
        slice_ms = 1;
      }
      runtime::current_runtime().wait(-1, 0, slice_ms);
    }
    finished = gate->done.load(std::memory_order_acquire);
    if (!finished) {
      if (gate->cancel()) {
        LOG_WARN("Timed out waiting for session takeover of {}", client_id_str);
        release_reservation();
        return MQ_ERR_TIMEOUT_V2;
      }
      while (!gate->done.load(std::memory_order_acquire)) {
        runtime::current_runtime().wait(-1, 0, 10);
      }
    }
  }

  // 在线程管理器中注册handler
  int result = thread_manager->register_handler(client_id, handler);
  if (result != MQ_SUCCESS) {
    release_reservation();
    return result;
  }

  // 占位还在自己手里时才发布索引，避免后写的连接覆盖先完成的主人。
  {
    WriteLockGuard lock(client_index_mutex_);
    auto reserved = client_reservations_.find(client_id_str);
    if (reserved != client_reservations_.end() && reserved->second == gate) {
      client_to_manager_[client_id_str] = thread_manager;
      client_reservations_.erase(reserved);
      held = false;
    }
  }
  if (held) {
    thread_manager->unregister_handler(client_id);
    release_reservation();
    return MQ_ERR_TIMEOUT_V2;
  }

  LOG_INFO("Session registered globally: {} thread {}", client_id_str,
           std::hash<std::thread::id>{}(thread_manager->get_thread_id()));
  return MQ_SUCCESS;
}

int GlobalSessionManager::unregister_session(const MQTTString& client_id)
{
  std::string client_id_str = from_mqtt_string(client_id);

  // 直接获取当前线程的管理器，避免全局查找开销
  ThreadLocalSessionManager* thread_manager = get_thread_manager();
  if (!thread_manager) {
    LOG_ERROR("No thread manager available for session unregistration");
    return MQ_ERR_SESSION_THREAD_MISMATCH;
  }

  // 从线程管理器中注销
  int result = thread_manager->unregister_handler(client_id);

  // 只有注销成功才从客户端索引中移除
  if (result == MQ_SUCCESS) {
    remove_client_index(client_id_str);
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

  auto it = client_to_manager_.find(client_id_str);

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
  auto* local = get_thread_manager();
  WriteLockGuard lock(client_index_mutex_);
  auto found = client_to_manager_.find(client_id);
  if (found != client_to_manager_.end() && found->second == local)
    client_to_manager_.erase(found);
}

int GlobalSessionManager::forward_publish(const MQTTString& target_client_id,
                                          const PublishPacket& packet,
                                          const MQTTString& sender_client_id)
{
  ThreadLocalSessionManager* thread_manager = find_client_manager(target_client_id);
  if (!thread_manager) {
    LOG_WARN("Target client not found: {}", from_mqtt_string(target_client_id));
    return MQ_ERR_NOT_FOUND_V2;
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
    return MQ_ERR_PARAM_V2;
  }

  ThreadLocalSessionManager* thread_manager = find_client_manager(target_client_id);
  if (!thread_manager) {
    LOG_WARN("Target client not found: {}", from_mqtt_string(target_client_id));
    return MQ_ERR_NOT_FOUND_V2;
  }

  // 使用共享内容加入目标线程的队列
  const int ret = thread_manager->enqueue_shared_message(content, target_client_id);
  if (ret != MQ_SUCCESS) return ret;

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
    return MQ_ERR_PARAM_V2;
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

namespace {
struct PlannedDelivery
{
  MQTTString client_id;
  uint8_t qos = 0;
  bool retain = false;
  uint8_t subscription_qos = 0;
};

std::vector<PlannedDelivery> plan_subscription_deliveries(
    const std::vector<SubscriberInfo>& subscribers, const std::string& sender_id,
    uint8_t message_qos, bool message_retain, bool implicit_no_local)
{
  std::vector<PlannedDelivery> planned;
  std::unordered_map<std::string, size_t> index;
  for (const SubscriberInfo& subscriber : subscribers) {
    const std::string id = from_mqtt_string(subscriber.client_id);
    // No Local is decided per subscription before overlapping filters are merged.
    if (id == sender_id && (implicit_no_local || subscriber.no_local))
      continue;
    const uint8_t delivery_qos = std::min(subscriber.qos, message_qos);
    const bool retain = subscriber.retain_as_published && message_retain;
    const auto found = index.find(id);
    if (found == index.end()) {
      index.emplace(id, planned.size());
      planned.push_back(
          PlannedDelivery{subscriber.client_id, delivery_qos, retain, subscriber.qos});
    } else if (subscriber.qos > planned[found->second].subscription_qos) {
      PlannedDelivery& chosen = planned[found->second];
      chosen.qos = delivery_qos;
      chosen.retain = retain;
      chosen.subscription_qos = subscriber.qos;
    }
  }
  return planned;
}
}  // namespace

int GlobalSessionManager::forward_publish_by_topic(const MQTTString& topic,
                                                   const PublishPacket& packet,
                                                   const MQTTString& sender_client_id)
{
  // Inbound topic aliases are not supported (CONNACK advertises a maximum of
  // zero). Reject unresolved/invalid topics before they can enter the journal,
  // including the WebSocket bridge's publishing path.
  if (topic.empty() || topic.find('+') != MQTTString::npos ||
      topic.find('#') != MQTTString::npos || topic.find('\0') != MQTTString::npos ||
      packet.properties.topic_alias != 0)
    return MQ_ERR_PUBLISH_TOPIC;
  std::vector<std::string> persisted_targets;
  size_t persisted_count = 0;
  if (packet.properties.has_message_expiry_interval &&
      packet.properties.message_expiry_interval == 0)
    return 0;
  const std::string sender_id = from_mqtt_string(sender_client_id);
  std::vector<SubscriberInfo> subscribers;
  const int lookup = find_topic_subscribers(topic, subscribers);
  std::vector<PlannedDelivery> planned;
  if (lookup == MQ_SUCCESS) {
    planned = plan_subscription_deliveries(subscribers, sender_id, packet.qos, packet.retain,
                                           implicit_no_local_);
  }
  if (durable_store_) {
    if (packet.qos > 1)
      return MQ_ERR_PACKET_INVALID;  // This mode advertises maximum QoS 1.
    if (packet.qos == 1 && durable_store_->has_subscriptions()) {
      try {
        MQTTAllocator wire_allocator("durable_publish", MQTTMemoryTag::MEM_TAG_SESSION_MANAGER,
                                     4 * 1024 * 1024);
        MQTTParser serializer(&wire_allocator);
        serializer.set_protocol_version_hint(5);
        PublishPacket copy(&wire_allocator);
        copy.type = PacketType::PUBLISH;
        copy.topic_name = packet.topic_name;
        copy.payload = packet.payload;
        copy.qos = 1;
        copy.packet_id = 1;
        // One stored wire is shared by every durable recipient, so it cannot
        // carry a different retain flag per subscription. Restored Retain As
        // Published still applies on the live path. This copy keeps retain clear.
        copy.retain = false;
        copy.dup = false;
        copy.properties = Properties(packet.properties, &wire_allocator);
        copy.properties.topic_alias = 0;
        MQTTBuffer wire(&wire_allocator);
        if (serializer.serialize_publish(&copy, wire) != MQ_SUCCESS)
          return MQ_ERR_INTERNAL;
        int64_t expires =
            packet.properties.message_expiry_interval
                ? DurableStore::now_ms() + int64_t(packet.properties.message_expiry_interval) * 1000
                : 0;
        auto result = durable_store_->publish(
            from_mqtt_string(topic),
            std::string(reinterpret_cast<const char*>(wire.data()), wire.size()),
            from_mqtt_string(sender_client_id), expires, implicit_no_local_);
        if (!result.ok) {
          LOG_WARN("Persistent publish refused: {}", result.error);
          return result.quota_exceeded ? MQ_ERR_PUBLISH_QUOTA : MQ_ERR_INTERNAL;
        }
        persisted_count = result.accepted_targets;
        persisted_targets = std::move(result.targets);
      } catch (const std::exception& error) {
        LOG_WARN("Persistent publish failed: {}", error.what());
        return MQ_ERR_INTERNAL;
      }
    }
  }
  int ret = lookup;
  if (MQ_FAIL(ret)) {
    LOG_ERROR("Failed to find subscribers for topic: {}", from_mqtt_string(topic));
    return 0;
  }

  if (planned.empty())
    return int(persisted_count);
  int forwarded_count = int(persisted_count);
  try {
    // Previously every recipient copied the payload using the publisher's
    // 1-MiB allocator. A single 4-KiB publish to 500 recipients could exhaust
    // it and throw out of the MQTT coroutine. One owned snapshot per publish
    // also survives the publishing connection being closed while delivery waits.
    SharedMessageContentPtr content(new SharedMessageContent(
        packet.topic_name, packet.payload, packet.qos, packet.retain, packet.dup,
        packet.properties, sender_client_id, global_allocator_));
    std::unordered_map<uint8_t, SharedMessageContentPtr> shaped;
    for (const PlannedDelivery& delivery : planned) {
      const std::string target = from_mqtt_string(delivery.client_id);
      if (std::binary_search(persisted_targets.begin(), persisted_targets.end(), target))
        continue;
      SharedMessageContentPtr outbound = content;
      if (delivery.qos != packet.qos || delivery.retain != packet.retain) {
        const uint8_t key = static_cast<uint8_t>((delivery.qos << 1) | (delivery.retain ? 1 : 0));
        auto found = shaped.find(key);
        if (found == shaped.end()) {
          // 降级或改写后的投递是一条新报文，不能沿用入站重传的 DUP。
          outbound = SharedMessageContentPtr(new SharedMessageContent(
              packet.topic_name, packet.payload, delivery.qos, delivery.retain, false,
              packet.properties, sender_client_id, global_allocator_));
          shaped.emplace(key, outbound);
        } else {
          outbound = found->second;
        }
      }
      if (forward_publish_shared(delivery.client_id, outbound) == MQ_SUCCESS)
        ++forwarded_count;
    }
  } catch (const std::bad_alloc&) {
    LOG_WARN("Insufficient memory while queuing PUBLISH fanout");
    return MQ_ERR_MEMORY_ALLOC;
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
    return MQ_ERR_PARAM_V2;
  }

  // 使用高性能主题匹配树查找订阅者
  std::vector<SubscriberInfo> subscribers;
  int ret = find_topic_subscribers(topic, subscribers);
  if (MQ_FAIL(ret)) {
    LOG_ERROR("Failed to find subscribers for topic: {}", from_mqtt_string(topic));
    return 0;
  }

  const std::string sender_id = from_mqtt_string(content->sender_client_id);
  const std::vector<PlannedDelivery> planned = plan_subscription_deliveries(
      subscribers, sender_id, content->qos, content->retain, implicit_no_local_);
  if (planned.empty()) {
    LOG_DEBUG("No subscribers found for topic: {}", from_mqtt_string(topic));
    return 0;
  }

  int forwarded_count = 0;
  try {
    std::unordered_map<uint8_t, SharedMessageContentPtr> shaped;
    std::unordered_map<uint8_t, std::vector<MQTTString>> groups;
    for (const PlannedDelivery& delivery : planned) {
      const uint8_t key = static_cast<uint8_t>((delivery.qos << 1) | (delivery.retain ? 1 : 0));
      groups[key].push_back(delivery.client_id);
      if (shaped.find(key) == shaped.end()) {
        if (delivery.qos == content->qos && delivery.retain == content->retain) {
          shaped.emplace(key, content);
        } else {
          shaped.emplace(key, SharedMessageContentPtr(new SharedMessageContent(
                                  content->topic_name, content->payload, delivery.qos,
                                  delivery.retain, false, content->properties,
                                  content->sender_client_id, global_allocator_)));
        }
      }
    }
    for (const auto& group : groups) {
      forwarded_count += batch_forward_publish(group.second, shaped[group.first]);
    }
  } catch (const std::bad_alloc&) {
    LOG_WARN("Insufficient memory while queuing shared PUBLISH fanout");
    return MQ_ERR_MEMORY_ALLOC;
  }

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
      SafeHandlerRef safe_handler = manager->get_safe_handler(client_id);
      if (safe_handler.is_valid()) {
        MQTTProtocolHandler* handler = safe_handler.get();
        if (handler) {
          MQTTVector<MQTTString> subscriptions;
          int result = handler->get_subscriptions(subscriptions);
          if (result == MQ_SUCCESS) {
            for (const MQTTString& subscription : subscriptions) {
              if (is_topic_match(topic, subscription)) {
                subscribers.push_back(client_id);
                break;
              }
            }
          } else {
            LOG_WARN("Failed to get subscriptions for client {}: {}", from_mqtt_string(client_id),
                     result);
          }
        }
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
    managers_to_clean.assign(thread_manager_array_.begin(), thread_manager_array_.end());
  }

  int total_cleaned = 0;
  for (ThreadLocalSessionManager* manager : managers_to_clean) {
    total_cleaned += manager->cleanup_invalid_handlers();
  }

  // 清理客户端索引中的无效条目
  {
    WriteLockGuard client_lock(client_index_mutex_);

    auto it = client_to_manager_.begin();
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

  // A root '+' or '#' does not match a topic whose first character is '$'.
  if (!topic.empty() && topic[0] == '$' && (filter[0] == '#' || filter[0] == '+')) {
    return false;
  }

  // 如果过滤器是 '#'，匹配所有非 '$' 主题
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

int GlobalSessionManager::cleanup_message_cache(int max_age_seconds)
{
  if (!message_cache_) {
    LOG_ERROR("Message cache not initialized");
    return MQ_ERR_INTERNAL;
  }

  message_cache_->cleanup_expired_content(max_age_seconds);
  LOG_DEBUG("Message cache cleanup completed, max age: {} seconds", max_age_seconds);

  return MQ_SUCCESS;
}

int GlobalSessionManager::get_message_cache_size(size_t& cache_size) const
{
  cache_size = 0;

  if (!message_cache_) {
    LOG_ERROR("Message cache not initialized");
    return MQ_ERR_INTERNAL;
  }

  cache_size = message_cache_->get_cache_size();
  return MQ_SUCCESS;
}

// ============================================================================
// 新的主题匹配树相关方法实现
// ============================================================================

int GlobalSessionManager::subscribe_topic(const MQTTString& topic_filter,
                                          const MQTTString& client_id, uint8_t qos)
{
  SubscriptionOptions options;
  options.qos = qos;
  return subscribe_topic(topic_filter, client_id, options);
}

int GlobalSessionManager::subscribe_topic(const MQTTString& topic_filter,
                                          const MQTTString& client_id,
                                          const SubscriptionOptions& options)
{
  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_INTERNAL;
  }

  int result = topic_tree_->subscribe(topic_filter, client_id, options);
  if (result == MQ_SUCCESS) {
    LOG_DEBUG("Client {} subscribed to topic filter {} with QoS {}", from_mqtt_string(client_id),
              from_mqtt_string(topic_filter), options.qos);
  } else {
    LOG_ERROR("Failed to subscribe client {} to topic filter {}: error {}",
              from_mqtt_string(client_id), from_mqtt_string(topic_filter), result);
  }

  return result;
}

int GlobalSessionManager::unsubscribe_topic(const MQTTString& topic_filter,
                                            const MQTTString& client_id)
{
  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_INTERNAL;
  }

  int result = topic_tree_->unsubscribe(topic_filter, client_id);
  if (result == MQ_SUCCESS) {
    LOG_DEBUG("Client {} unsubscribed from topic filter {}", from_mqtt_string(client_id),
              from_mqtt_string(topic_filter));
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
  LOG_INFO("Unsubscribed client {} from {} topics", from_mqtt_string(client_id),
           unsubscribed_count);

  return unsubscribed_count;
}

int GlobalSessionManager::get_client_subscriptions(const MQTTString& client_id,
                                                   std::vector<MQTTString>& subscriptions) const
{
  subscriptions.clear();

  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_INTERNAL;
  }

  TopicTreeLevelVector owned_subscriptions{TopicTreeAllocator<MQTTString>(global_allocator_)};
  int ret = topic_tree_->get_client_subscriptions(client_id, owned_subscriptions);
  if (MQ_FAIL(ret)) {
    LOG_ERROR("Failed to get client subscriptions: {}", ret);
    return ret;
  }

  subscriptions.reserve(owned_subscriptions.size());
  for (const auto& subscription : owned_subscriptions) {
    subscriptions.push_back(subscription);
  }

  return MQ_SUCCESS;
}

int GlobalSessionManager::find_topic_subscribers(const MQTTString& topic,
                                                 std::vector<SubscriberInfo>& subscribers) const
{
  subscribers.clear();

  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_INTERNAL;
  }

  TopicMatchResult result(global_allocator_);
  int ret = topic_tree_->find_subscribers(topic, result);
  if (MQ_FAIL(ret)) {
    LOG_ERROR("Failed to find topic subscribers: {}", ret);
    return ret;
  }

  // 转换分配器类型
  subscribers.reserve(result.subscribers.size());
  for (const auto& subscriber : result.subscribers) {
    subscribers.push_back(subscriber);
  }

  return MQ_SUCCESS;
}

int GlobalSessionManager::get_topic_tree_stats(size_t& subscriber_count, size_t& node_count) const
{
  subscriber_count = 0;
  node_count = 0;

  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_INTERNAL;
  }

  int ret = topic_tree_->get_total_subscribers(subscriber_count);
  if (MQ_FAIL(ret)) {
    LOG_ERROR("Failed to get subscriber count: {}", ret);
    return ret;
  }

  ret = topic_tree_->get_total_nodes(node_count);
  if (MQ_FAIL(ret)) {
    LOG_ERROR("Failed to get node count: {}", ret);
    return ret;
  }

  return MQ_SUCCESS;
}

int GlobalSessionManager::cleanup_topic_tree(size_t& cleaned_count) const
{
  cleaned_count = 0;

  if (!topic_tree_) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_INTERNAL;
  }

  int ret = topic_tree_->cleanup_empty_nodes(cleaned_count);
  if (MQ_FAIL(ret)) {
    LOG_ERROR("Failed to cleanup topic tree: {}", ret);
    return ret;
  }

  if (cleaned_count > 0) {
    LOG_INFO("Cleaned up {} empty nodes from topic tree", cleaned_count);
  }

  return MQ_SUCCESS;
}

size_t GlobalSessionManager::get_thread_count() const
{
  ReadLockGuard lock(managers_mutex_);
  return thread_manager_array_.size();
}

int GlobalSessionManager::get_total_handler_count(size_t& total_count) const
{
  ReadLockGuard lock(managers_mutex_);
  
  total_count = 0;
  for (const ThreadLocalSessionManager* manager : thread_manager_array_) {
    total_count += manager->get_handler_count();
  }
  
  return MQ_SUCCESS;
}

int GlobalSessionManager::get_total_pending_message_count(size_t& total_count) const
{
  ReadLockGuard lock(managers_mutex_);
  
  total_count = 0;
  for (const ThreadLocalSessionManager* manager : thread_manager_array_) {
    total_count += manager->get_pending_message_count();
  }
  
  return MQ_SUCCESS;
}

void GlobalSessionManager::set_event_forwarding_service(events::EventForwardingService* forwarding_service)
{
  event_forwarding_service_ = forwarding_service;
  LOG_INFO("Event forwarding service set: {}", (forwarding_service ? "enabled" : "disabled"));
}

void GlobalSessionManager::trigger_login_event(const MQTTString& client_id,
                                               const MQTTString& username,
                                               const MQTTString& protocol_version,
                                               int keep_alive,
                                               bool clean_session,
                                               const MQTTString& client_ip,
                                               int client_port)
{
  if (!event_forwarding_service_ || !event_forwarding_service_->isEnabled()) {
    return;  // Event forwarding is disabled
  }
  
  // Create login event data
  events::LoginEventData login_data;
  login_data.metadata = events::createEventMetadata(
      to_mqtt_string("mqtt_server", global_allocator_), 
      client_id, 
      client_ip, 
      client_port);
  login_data.username = username;
  login_data.protocol_version = protocol_version;
  login_data.keep_alive = keep_alive;
  login_data.clean_session = clean_session;
  
  // Create and forward event
  auto event = events::createLoginEvent(login_data);
  if (!event_forwarding_service_->forwardEvent(event)) {
    LOG_DEBUG("Failed to forward login event for client: {}", from_mqtt_string(client_id));
  }
}

void GlobalSessionManager::trigger_logout_event(const MQTTString& client_id,
                                                const MQTTString& reason,
                                                int session_duration_seconds,
                                                int64_t bytes_sent,
                                                int64_t bytes_received,
                                                int messages_sent,
                                                int messages_received,
                                                const MQTTString& client_ip,
                                                int client_port)
{
  if (!event_forwarding_service_ || !event_forwarding_service_->isEnabled()) {
    return;  // Event forwarding is disabled
  }
  
  // Create logout event data
  events::LogoutEventData logout_data;
  logout_data.metadata = events::createEventMetadata(
      to_mqtt_string("mqtt_server", global_allocator_), 
      client_id, 
      client_ip, 
      client_port);
  logout_data.reason = reason;
  logout_data.session_duration_seconds = session_duration_seconds;
  logout_data.bytes_sent = bytes_sent;
  logout_data.bytes_received = bytes_received;
  logout_data.messages_sent = messages_sent;
  logout_data.messages_received = messages_received;
  
  // Create and forward event
  auto event = events::createLogoutEvent(logout_data);
  if (!event_forwarding_service_->forwardEvent(event)) {
    LOG_DEBUG("Failed to forward logout event for client: {}", from_mqtt_string(client_id));
  }
}

void GlobalSessionManager::trigger_publish_event(const MQTTString& client_id,
                                                 const MQTTString& topic,
                                                 int qos,
                                                 bool retain,
                                                 bool duplicate,
                                                 int payload_size,
                                                 const MQTTVector<uint8_t>& payload,
                                                 const MQTTString& client_ip,
                                                 int client_port)
{
  if (!event_forwarding_service_ || !event_forwarding_service_->isEnabled()) {
    return;  // Event forwarding is disabled
  }
  
  // Create publish event data
  events::PublishEventData publish_data;
  publish_data.metadata = events::createEventMetadata(
      to_mqtt_string("mqtt_server", global_allocator_), 
      client_id, 
      client_ip, 
      client_port);
  publish_data.topic = topic;
  publish_data.qos = qos;
  publish_data.retain = retain;
  publish_data.duplicate = duplicate;
  publish_data.payload_size = payload_size;
  
  // Only include payload if it's small (to save bandwidth)
  if (payload.size() <= 1024) {  // 1KB limit
    publish_data.payload = payload;
  }
  
  // Create and forward event
  auto event = events::createPublishEvent(publish_data);
  if (!event_forwarding_service_->forwardEvent(event)) {
    LOG_DEBUG("Failed to forward publish event for client: {}, topic: {}", 
              from_mqtt_string(client_id), from_mqtt_string(topic));
  }
}

//==============================================================================
// 路由客户端相关方法实现
//==============================================================================

void GlobalSessionManager::set_router_client(std::unique_ptr<MQTTRouterRpcClient> router_client)
{
  router_client_ = std::move(router_client);
  if (router_client_.get() != NULL) {
    (void)router_client_->set_cluster_config(cluster_config_);
  }
  LOG_INFO("Router client set for GlobalSessionManager");
}

int GlobalSessionManager::set_cluster_config(const ClusterConfig& cluster_config)
{
  int ret = MQ_SUCCESS;
  cluster_config_ = cluster_config;

  if (router_client_.get() != NULL) {
    ret = router_client_->set_cluster_config(cluster_config_);
  }

  if (MQ_SUCCESS == ret && !cluster_config_.server_id.empty()) {
    server_id_ = to_mqtt_string(cluster_config_.server_id, global_allocator_);
  }

  if (MQ_SUCCESS == ret) {
    if (cluster_config_.cluster_enabled) {
      if (!forwarding_client_) {
        ForwardingConnectionPool::Config pool_config;
        pool_config.pool_size_per_target = cluster_config_.forwarding_pool_size;
        pool_config.idle_timeout_ms      = cluster_config_.forwarding_idle_timeout_ms;
        forwarding_pool_.reset(new ForwardingConnectionPool(global_allocator_, pool_config));
        forwarding_client_.reset(
            new MQTTForwardingRpcClient(global_allocator_, forwarding_pool_.get()));
      }
      if (forwarding_service_) {
        (void)forwarding_service_->stop();
        forwarding_service_.reset(nullptr);
      }
      forwarding_service_.reset(new MQTTForwardingService(global_allocator_,
                                                          this,
                                                          cluster_config_.forwarding_host,
                                                          cluster_config_.forwarding_port));
      ret = forwarding_service_->start();
      if (MQ_FAIL(ret)) {
        forwarding_service_.reset(nullptr);
        forwarding_client_.reset(nullptr);
        forwarding_pool_.reset(nullptr);
      }
    } else {
      if (forwarding_service_) {
        (void)forwarding_service_->stop();
        forwarding_service_.reset(nullptr);
      }
      forwarding_client_.reset(nullptr);
      forwarding_pool_.reset(nullptr);
    }
  }

  return ret;
}

MQTTRouterRpcClient* GlobalSessionManager::get_router_client() const
{
  return router_client_.get();
}

void GlobalSessionManager::set_server_id(const MQTTString& server_id)
{
  server_id_ = server_id;
  LOG_INFO("Server ID set to: {}", from_mqtt_string(server_id));
}

const MQTTString& GlobalSessionManager::get_server_id() const
{
  return server_id_;
}

int GlobalSessionManager::register_with_router()
{
  int ret = MQ_SUCCESS;
  bool is_connected = false;
  MQTTRouterRpcClient::NodeAuthRequest request(global_allocator_);
  MQTTRouterRpcClient::NodeAuthResponse response(global_allocator_);

  if (!cluster_config_.cluster_enabled) {
    ret = MQ_SUCCESS;
  } else if (router_client_.get() == NULL) {
    ret = MQ_ERR_INVALID_STATE;
  } else if (MQ_FAIL(router_client_->get_connection_state(is_connected))) {
  } else if (!is_connected) {
    ret = MQ_ERR_ROUTER_NOT_CONNECTED;
  } else {
    request.server_id = to_mqtt_string(cluster_config_.server_id, global_allocator_);
    request.token = to_mqtt_string(cluster_config_.server_token, global_allocator_);
    request.timestamp_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    request.nonce = to_mqtt_string(
        cluster_config_.server_id + "-" + std::to_string(request.timestamp_ms), global_allocator_);
    request.host = to_mqtt_string(cluster_config_.forwarding_host, global_allocator_);
    request.port = cluster_config_.router_port;
    request.forwarding_port = cluster_config_.forwarding_port;
    ret = router_client_->authenticate_node(request, response);
  }

  return ret;
}

int GlobalSessionManager::heartbeat_with_router()
{
  int ret = MQ_SUCCESS;

  if (!cluster_config_.cluster_enabled) {
    ret = MQ_SUCCESS;
  } else if (router_client_.get() == NULL) {
    ret = MQ_ERR_INVALID_STATE;
  } else {
    ret = router_client_->heartbeat();
  }

  return ret;
}

int GlobalSessionManager::notify_router_client_connect(const MQTTString& client_id,
                                                      const MQTTString& username,
                                                      uint32_t protocol_version,
                                                      uint32_t keep_alive,
                                                      bool clean_session)
{
  int ret = MQ_SUCCESS;
  MQTTRouterRpcClient::ClientConnectRequest request(global_allocator_);

  if (router_client_.get() == NULL) {
    ret = MQ_SUCCESS;
  } else {
    request.server_id = server_id_;
    request.client_id = client_id;
    request.username = username;
    request.protocol_version = protocol_version;
    request.keep_alive = keep_alive;
    request.clean_session = clean_session;
    ret = router_client_->client_connect_async(request);
  }

  return ret;
}

int GlobalSessionManager::notify_router_client_disconnect(const MQTTString& client_id,
                                                         const MQTTString& disconnect_reason)
{
  int ret = MQ_SUCCESS;
  MQTTRouterRpcClient::ClientDisconnectRequest request(global_allocator_);

  if (router_client_.get() == NULL) {
    ret = MQ_SUCCESS;
  } else {
    request.server_id = server_id_;
    request.client_id = client_id;
    request.disconnect_reason = disconnect_reason;
    ret = router_client_->client_disconnect_async(request);
  }

  return ret;
}

int GlobalSessionManager::subscribe_topic_with_router(const MQTTString& topic_filter, const MQTTString& client_id, uint8_t qos)
{
  SubscriptionOptions options;
  options.qos = qos;
  return subscribe_topic_with_router(topic_filter, client_id, options);
}

int GlobalSessionManager::subscribe_topic_with_router(const MQTTString& topic_filter,
                                                     const MQTTString& client_id,
                                                     const SubscriptionOptions& options)
{
  return subscribe_topic_cluster(topic_filter, client_id, options);
}

int GlobalSessionManager::subscribe_topic_cluster(const MQTTString& topic_filter,
                                                  const MQTTString& client_id,
                                                  uint8_t qos)
{
  SubscriptionOptions options;
  options.qos = qos;
  return subscribe_topic_cluster(topic_filter, client_id, options);
}

int GlobalSessionManager::subscribe_topic_cluster(const MQTTString& topic_filter,
                                                  const MQTTString& client_id,
                                                  const SubscriptionOptions& options)
{
  int ret = MQ_SUCCESS;
  MQTTRouterRpcClient::SubscribeRequest request(global_allocator_);
  bool strict_cluster_mode = false;

  // Do not replace an existing local filter until router admission succeeds.
  // A failed re-SUBSCRIBE must retain its previous QoS and routing entry.
  // Router RPC stays QoS-only; cluster delivery does not carry No Local / RAP.
  if (router_client_.get() != NULL) {
    strict_cluster_mode = cluster_config_.cluster_enabled;
    request.server_id = server_id_;
    request.client_id = client_id;
    request.topic_filter = topic_filter;
    request.qos = options.qos;
    if (strict_cluster_mode) {
      ret = router_client_->subscribe(request);
    } else {
      ret = router_client_->subscribe_async(request);
    }
    if (MQ_SUCCESS != ret)
      return ret;
  }
  return subscribe_topic(topic_filter, client_id, options);
}

int GlobalSessionManager::unsubscribe_topic_with_router(const MQTTString& topic_filter, const MQTTString& client_id)
{
  int ret = MQ_SUCCESS;
  ret = unsubscribe_topic_cluster(topic_filter, client_id);
  return ret;
}

int GlobalSessionManager::unsubscribe_topic_cluster(const MQTTString& topic_filter, const MQTTString& client_id)
{
  int ret = MQ_SUCCESS;
  MQTTRouterRpcClient::UnsubscribeRequest request(global_allocator_);
  bool strict_cluster_mode = false;

  if (router_client_.get() == NULL) {
    ret = unsubscribe_topic(topic_filter, client_id);
  } else {
    strict_cluster_mode = cluster_config_.cluster_enabled;
    request.server_id = server_id_;
    request.client_id = client_id;
    request.topic_filter = topic_filter;
    if (strict_cluster_mode) {
      std::vector<SubscriberInfo> subscribers;
      ret = find_topic_subscribers(topic_filter, subscribers);
      SubscriptionOptions restored;
      if (MQ_SUCCESS == ret) {
        for (size_t i = 0; i < subscribers.size(); ++i) {
          if (subscribers[i].client_id == client_id) {
            restored.qos = subscribers[i].qos;
            restored.no_local = subscribers[i].no_local;
            restored.retain_as_published = subscribers[i].retain_as_published;
            restored.retain_handling = subscribers[i].retain_handling;
            break;
          }
        }
        ret = unsubscribe_topic(topic_filter, client_id);
        if (MQ_SUCCESS == ret) {
          ret = router_client_->unsubscribe(request);
          if (MQ_SUCCESS != ret) {
            int restore_ret = subscribe_topic(topic_filter, client_id, restored);
            if (MQ_SUCCESS != restore_ret) {
              LOG_WARN("Failed to restore local subscription after router unsubscribe failure, topic={}, client_id={}, restore_ret={}",
                       from_mqtt_string(topic_filter), from_mqtt_string(client_id), restore_ret);
            }
          }
        }
      }
    } else {
      ret = router_client_->unsubscribe_async(request);
      if (MQ_SUCCESS == ret) {
        ret = unsubscribe_topic(topic_filter, client_id);
      }
    }
  }

  return ret;
}

int GlobalSessionManager::forward_publish_via_router(const MQTTString& topic, const PublishPacket& packet,
                                                    const MQTTString& sender_client_id)
{
  int ret = MQ_SUCCESS;
  int delivered_count = 0;
  ret = forward_publish_cluster(topic, packet, sender_client_id, delivered_count);
  return ret;
}

int GlobalSessionManager::forward_publish_cluster(const MQTTString& topic,
                                                  const PublishPacket& packet,
                                                  const MQTTString& sender_client_id,
                                                  int& delivered_count)
{
  int ret = MQ_SUCCESS;
  MQTTRouterRpcClient::RoutePublishRequest request(global_allocator_);
  MQTTRouterRpcClient::RouteTargetVector targets{
      MQTTSTLAllocator<MQTTRouterRpcClient::RouteTarget>(global_allocator_)};
  bool router_enabled = false;
  bool strict_cluster_mode = false;

  delivered_count = forward_publish_by_topic(topic, packet, sender_client_id);
  router_enabled = (router_client_.get() != NULL);
  strict_cluster_mode = cluster_config_.cluster_enabled;
  if (!router_enabled) {
    ret = delivered_count;
  } else {
    request.topic = topic;
    request.qos = packet.qos;
    request.retain = packet.retain;
    request.publisher_server_id = server_id_;
    request.publisher_client_id = sender_client_id;
    request.message_id = 0;
    request.trace_id = to_mqtt_string(from_mqtt_string(server_id_) + ":" + from_mqtt_string(sender_client_id),
                                      global_allocator_);
    if (!packet.payload.empty()) {
      request.payload = packet.payload;
    }
    ret = router_client_->route_publish(request, targets);
    if (MQ_ERR_ROUTER_ROUTE_EMPTY == ret) {
      ret = delivered_count;
    } else if (MQ_SUCCESS == ret) {
      for (size_t i = 0; MQ_SUCCESS == ret && i < targets.size(); ++i) {
        int remote_delivered_count = 0;
        if (forwarding_client_.get() == NULL) {
          if (strict_cluster_mode) {
            ret = MQ_ERR_FORWARD_TARGET_INVALID;
          }
        } else {
          ret = forwarding_client_->forward_publish(from_mqtt_string(targets[i].host),
                                                    targets[i].forwarding_port,
                                                    server_id_,
                                                    sender_client_id,
                                                    topic,
                                                    packet.payload,
                                                    packet.qos,
                                                    packet.retain,
                                                    request.message_id,
                                                    request.trace_id,
                                                    remote_delivered_count);
          if (MQ_SUCCESS == ret) {
            delivered_count += remote_delivered_count;
          }
        }
      }
      if (MQ_SUCCESS == ret) {
        ret = delivered_count;
      }
    }
  }

  return ret;
}

bool GlobalSessionManager::is_router_available() const
{
  return router_client_ && router_client_->is_connected();
}

}  // namespace mqtt
