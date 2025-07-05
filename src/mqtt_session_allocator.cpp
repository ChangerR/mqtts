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
    
    MQTTAllocator* get_or_create_session_allocator(const std::string& client_id, size_t memory_limit) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = session_allocators_.find(client_id);
        if (it != session_allocators_.end()) {
            return it->second;
        }
        
        // 创建新的session allocator
        MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        MQTTAllocator* session_allocator = root_allocator->create_child(
            "session_" + client_id, MQTTMemoryTag::MEM_TAG_SESSION, memory_limit);
        
        if (!session_allocator) {
            LOG_ERROR("Failed to create session allocator for client: {}", client_id);
            return nullptr;
        }
        
        session_allocators_[client_id] = session_allocator;
        LOG_INFO("Created session allocator for client: {} with limit: {} bytes", 
                 client_id, memory_limit);
        return session_allocator;
    }
    
    MQTTAllocator* get_or_create_session_queue_allocator(const std::string& client_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string queue_key = client_id + "_queue";
        auto it = session_allocators_.find(queue_key);
        if (it != session_allocators_.end()) {
            return it->second;
        }
        
        // 先确保session allocator存在
        MQTTAllocator* session_allocator = get_session_allocator_unlocked(client_id);
        if (!session_allocator) {
            LOG_ERROR("Session allocator not found for client: {}", client_id);
            return nullptr;
        }
        
        // 创建队列allocator作为session allocator的子分配器
        MQTTAllocator* queue_allocator = session_allocator->create_child(
            "queue", MQTTMemoryTag::MEM_TAG_SESSION_QUEUE, 0);
        
        if (!queue_allocator) {
            LOG_ERROR("Failed to create queue allocator for client: {}", client_id);
            return nullptr;
        }
        
        session_allocators_[queue_key] = queue_allocator;
        LOG_DEBUG("Created queue allocator for client: {}", client_id);
        return queue_allocator;
    }
    
    MQTTAllocator* get_or_create_session_worker_allocator(const std::string& client_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string worker_key = client_id + "_worker";
        auto it = session_allocators_.find(worker_key);
        if (it != session_allocators_.end()) {
            return it->second;
        }
        
        // 先确保session allocator存在
        MQTTAllocator* session_allocator = get_session_allocator_unlocked(client_id);
        if (!session_allocator) {
            LOG_ERROR("Session allocator not found for client: {}", client_id);
            return nullptr;
        }
        
        // 创建worker allocator作为session allocator的子分配器
        MQTTAllocator* worker_allocator = session_allocator->create_child(
            "worker", MQTTMemoryTag::MEM_TAG_SESSION_WORKER, 0);
        
        if (!worker_allocator) {
            LOG_ERROR("Failed to create worker allocator for client: {}", client_id);
            return nullptr;
        }
        
        session_allocators_[worker_key] = worker_allocator;
        LOG_DEBUG("Created worker allocator for client: {}", client_id);
        return worker_allocator;
    }
    
    MQTTAllocator* get_message_cache_allocator() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (message_cache_allocator_) {
            return message_cache_allocator_;
        }
        
        // 创建全局消息缓存allocator
        MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        message_cache_allocator_ = root_allocator->create_child(
            "message_cache", MQTTMemoryTag::MEM_TAG_MESSAGE_CACHE, 0);
        
        if (!message_cache_allocator_) {
            LOG_ERROR("Failed to create message cache allocator");
            return nullptr;
        }
        
        LOG_INFO("Created message cache allocator");
        return message_cache_allocator_;
    }
    
    void cleanup_session_allocators(const std::string& client_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 清理session相关的所有allocator
        std::vector<std::string> keys_to_remove;
        for (const auto& pair : session_allocators_) {
            if (pair.first == client_id || 
                pair.first.find(client_id + "_") == 0) {
                keys_to_remove.push_back(pair.first);
            }
        }
        
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
                LOG_DEBUG("Removed allocator: {}", key);
            }
        }
        
        LOG_INFO("Cleaned up session allocators for client: {}", client_id);
    }
    
    size_t get_session_memory_usage(const std::string& client_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        MQTTAllocator* allocator = get_session_allocator_unlocked(client_id);
        if (!allocator) {
            return 0;
        }
        
        return allocator->get_total_memory_usage();
    }
    
    bool is_session_memory_limit_exceeded(const std::string& client_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        MQTTAllocator* allocator = get_session_allocator_unlocked(client_id);
        if (!allocator) {
            return false;
        }
        
        size_t limit = allocator->get_memory_limit();
        if (limit == 0) {
            return false; // 无限制
        }
        
        return allocator->get_total_memory_usage() > limit;
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
MQTTAllocator* SessionAllocatorManager::get_session_allocator(const std::string& client_id, size_t memory_limit) {
    return GlobalSessionAllocatorCache::get_instance().get_or_create_session_allocator(client_id, memory_limit);
}

MQTTAllocator* SessionAllocatorManager::get_session_queue_allocator(const std::string& client_id) {
    return GlobalSessionAllocatorCache::get_instance().get_or_create_session_queue_allocator(client_id);
}

MQTTAllocator* SessionAllocatorManager::get_session_worker_allocator(const std::string& client_id) {
    return GlobalSessionAllocatorCache::get_instance().get_or_create_session_worker_allocator(client_id);
}

MQTTAllocator* SessionAllocatorManager::get_message_cache_allocator() {
    return GlobalSessionAllocatorCache::get_instance().get_message_cache_allocator();
}

void SessionAllocatorManager::cleanup_session_allocators(const std::string& client_id) {
    GlobalSessionAllocatorCache::get_instance().cleanup_session_allocators(client_id);
}

size_t SessionAllocatorManager::get_session_memory_usage(const std::string& client_id) {
    return GlobalSessionAllocatorCache::get_instance().get_session_memory_usage(client_id);
}

bool SessionAllocatorManager::is_session_memory_limit_exceeded(const std::string& client_id) {
    return GlobalSessionAllocatorCache::get_instance().is_session_memory_limit_exceeded(client_id);
}

std::string SessionAllocatorManager::make_session_allocator_id(const std::string& client_id, const std::string& suffix) {
    return client_id + "_" + suffix;
}

} // namespace mqtt