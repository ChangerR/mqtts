#include "mqtt_session_allocator.h"
#include <sstream>
#include <mutex>
#include <unordered_map>
#include "logger.h"

namespace mqtt {

// 全局管理器，用于缓存allocator
class GlobalSessionAllocatorCache {
public:
    static GlobalSessionAllocatorCache& get_instance() {
        static GlobalSessionAllocatorCache instance;
        return instance;
    }
    
         int get_or_create_session_allocator(const std::string& client_id, size_t memory_limit, MQTTAllocator*& allocator) {
         std::lock_guard<std::mutex> lock(mutex_);
         
         auto it = session_allocators_.find(client_id);
         if (it != session_allocators_.end()) {
             allocator = it->second;
             return MQ_SUCCESS;
         }
         
         // 创建新的session allocator
         MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
         if (!root_allocator) {
             LOG_ERROR("Root allocator not found");
             allocator = nullptr;
             return MQ_ERR_ALLOCATOR_NOT_FOUND;
         }
         
         MQTTAllocator* session_allocator = root_allocator->create_child(
             "session_" + client_id, MQTTMemoryTag::MEM_TAG_SESSION, memory_limit);
         
         if (!session_allocator) {
             LOG_ERROR("Failed to create session allocator for client: {}", client_id);
             allocator = nullptr;
             return MQ_ERR_ALLOCATOR_CREATE;
         }
         
         session_allocators_[client_id] = session_allocator;
         allocator = session_allocator;
         LOG_INFO("Created session allocator for client: {} with limit: {} bytes", 
                  client_id, memory_limit);
         return MQ_SUCCESS;
     }
    
         int get_or_create_session_queue_allocator(const std::string& client_id, MQTTAllocator*& allocator) {
         std::lock_guard<std::mutex> lock(mutex_);
         
         std::string queue_key = client_id + "_queue";
         auto it = session_allocators_.find(queue_key);
         if (it != session_allocators_.end()) {
             allocator = it->second;
             return MQ_SUCCESS;
         }
         
         // 先确保session allocator存在
         MQTTAllocator* session_allocator = get_session_allocator_unlocked(client_id);
         if (!session_allocator) {
             LOG_ERROR("Session allocator not found for client: {}", client_id);
             allocator = nullptr;
             return MQ_ERR_ALLOCATOR_NOT_FOUND;
         }
         
         // 创建队列allocator作为session allocator的子分配器
         MQTTAllocator* queue_allocator = session_allocator->create_child(
             "queue", MQTTMemoryTag::MEM_TAG_SESSION_QUEUE, 0);
         
         if (!queue_allocator) {
             LOG_ERROR("Failed to create queue allocator for client: {}", client_id);
             allocator = nullptr;
             return MQ_ERR_ALLOCATOR_CREATE;
         }
         
         session_allocators_[queue_key] = queue_allocator;
         allocator = queue_allocator;
         LOG_DEBUG("Created queue allocator for client: {}", client_id);
         return MQ_SUCCESS;
     }
    
         int get_or_create_session_worker_allocator(const std::string& client_id, MQTTAllocator*& allocator) {
         std::lock_guard<std::mutex> lock(mutex_);
         
         std::string worker_key = client_id + "_worker";
         auto it = session_allocators_.find(worker_key);
         if (it != session_allocators_.end()) {
             allocator = it->second;
             return MQ_SUCCESS;
         }
         
         // 先确保session allocator存在
         MQTTAllocator* session_allocator = get_session_allocator_unlocked(client_id);
         if (!session_allocator) {
             LOG_ERROR("Session allocator not found for client: {}", client_id);
             allocator = nullptr;
             return MQ_ERR_ALLOCATOR_NOT_FOUND;
         }
         
         // 创建worker allocator作为session allocator的子分配器
         MQTTAllocator* worker_allocator = session_allocator->create_child(
             "worker", MQTTMemoryTag::MEM_TAG_SESSION_WORKER, 0);
         
         if (!worker_allocator) {
             LOG_ERROR("Failed to create worker allocator for client: {}", client_id);
             allocator = nullptr;
             return MQ_ERR_ALLOCATOR_CREATE;
         }
         
         session_allocators_[worker_key] = worker_allocator;
         allocator = worker_allocator;
         LOG_DEBUG("Created worker allocator for client: {}", client_id);
         return MQ_SUCCESS;
     }
    
         int get_message_cache_allocator(MQTTAllocator*& allocator) {
         std::lock_guard<std::mutex> lock(mutex_);
         
         if (message_cache_allocator_) {
             allocator = message_cache_allocator_;
             return MQ_SUCCESS;
         }
         
         // 创建全局消息缓存allocator
         MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
         if (!root_allocator) {
             LOG_ERROR("Root allocator not found");
             allocator = nullptr;
             return MQ_ERR_ALLOCATOR_NOT_FOUND;
         }
         
         message_cache_allocator_ = root_allocator->create_child(
             "message_cache", MQTTMemoryTag::MEM_TAG_MESSAGE_CACHE, 0);
         
         if (!message_cache_allocator_) {
             LOG_ERROR("Failed to create message cache allocator");
             allocator = nullptr;
             return MQ_ERR_ALLOCATOR_CREATE;
         }
         
         allocator = message_cache_allocator_;
         LOG_INFO("Created message cache allocator");
         return MQ_SUCCESS;
     }
    
         int cleanup_session_allocators(const std::string& client_id) {
         std::lock_guard<std::mutex> lock(mutex_);
         
         // 清理session相关的所有allocator
         std::vector<std::string> keys_to_remove;
         for (const auto& pair : session_allocators_) {
             if (pair.first == client_id || 
                 pair.first.find(client_id + "_") == 0) {
                 keys_to_remove.push_back(pair.first);
             }
         }
         
         int cleaned_count = 0;
         for (const std::string& key : keys_to_remove) {
             auto it = session_allocators_.find(key);
             if (it != session_allocators_.end()) {
                 MQTTAllocator* allocator = it->second;
                 
                 // 从父allocator中移除
                 if (allocator->get_parent()) {
                     std::string child_id = allocator->get_id();
                     size_t pos = child_id.find_last_of('_');
                     if (pos != std::string::npos) {
                         child_id = child_id.substr(pos + 1);
                     }
                     allocator->get_parent()->remove_child(child_id);
                 }
                 
                 session_allocators_.erase(it);
                 cleaned_count++;
                 LOG_DEBUG("Removed allocator: {}", key);
             }
         }
         
         LOG_INFO("Cleaned up {} session allocators for client: {}", cleaned_count, client_id);
         return MQ_SUCCESS;
     }
    
         int get_session_memory_usage(const std::string& client_id, size_t& memory_usage) {
         std::lock_guard<std::mutex> lock(mutex_);
         
         MQTTAllocator* allocator = get_session_allocator_unlocked(client_id);
         if (!allocator) {
             memory_usage = 0;
             return MQ_ERR_ALLOCATOR_NOT_FOUND;
         }
         
         memory_usage = allocator->get_total_memory_usage();
         return MQ_SUCCESS;
     }
    
         int is_session_memory_limit_exceeded(const std::string& client_id, bool& limit_exceeded) {
         std::lock_guard<std::mutex> lock(mutex_);
         
         MQTTAllocator* allocator = get_session_allocator_unlocked(client_id);
         if (!allocator) {
             limit_exceeded = false;
             return MQ_ERR_ALLOCATOR_NOT_FOUND;
         }
         
         size_t limit = allocator->get_memory_limit();
         if (limit == 0) {
             limit_exceeded = false; // 无限制
         } else {
             limit_exceeded = (allocator->get_total_memory_usage() > limit);
         }
         
         return MQ_SUCCESS;
     }
    
private:
    std::mutex mutex_;
    std::unordered_map<std::string, MQTTAllocator*> session_allocators_;
    MQTTAllocator* message_cache_allocator_ = nullptr;
    
    MQTTAllocator* get_session_allocator_unlocked(const std::string& client_id) {
        auto it = session_allocators_.find(client_id);
        return (it != session_allocators_.end()) ? it->second : nullptr;
    }
};

// SessionAllocatorManager 实现
int SessionAllocatorManager::get_session_allocator(const std::string& client_id, size_t memory_limit, MQTTAllocator*& allocator) {
    return GlobalSessionAllocatorCache::get_instance().get_or_create_session_allocator(client_id, memory_limit, allocator);
}

int SessionAllocatorManager::get_session_queue_allocator(const std::string& client_id, MQTTAllocator*& allocator) {
    return GlobalSessionAllocatorCache::get_instance().get_or_create_session_queue_allocator(client_id, allocator);
}

int SessionAllocatorManager::get_session_worker_allocator(const std::string& client_id, MQTTAllocator*& allocator) {
    return GlobalSessionAllocatorCache::get_instance().get_or_create_session_worker_allocator(client_id, allocator);
}

int SessionAllocatorManager::get_message_cache_allocator(MQTTAllocator*& allocator) {
    return GlobalSessionAllocatorCache::get_instance().get_message_cache_allocator(allocator);
}

int SessionAllocatorManager::cleanup_session_allocators(const std::string& client_id) {
    return GlobalSessionAllocatorCache::get_instance().cleanup_session_allocators(client_id);
}

int SessionAllocatorManager::get_session_memory_usage(const std::string& client_id, size_t& memory_usage) {
    return GlobalSessionAllocatorCache::get_instance().get_session_memory_usage(client_id, memory_usage);
}

int SessionAllocatorManager::is_session_memory_limit_exceeded(const std::string& client_id, bool& limit_exceeded) {
    return GlobalSessionAllocatorCache::get_instance().is_session_memory_limit_exceeded(client_id, limit_exceeded);
}

std::string SessionAllocatorManager::make_session_allocator_id(const std::string& client_id, const std::string& suffix) {
    return client_id + "_" + suffix;
}

} // namespace mqtt