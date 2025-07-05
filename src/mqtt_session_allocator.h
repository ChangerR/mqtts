#ifndef MQTT_SESSION_ALLOCATOR_H
#define MQTT_SESSION_ALLOCATOR_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>
#include "mqtt_allocator.h"
#include "mqtt_stl_allocator.h"
#include "mqtt_memory_tags.h"

namespace mqtt {

/**
 * @brief Session-specific allocator manager
 * 为每个session创建独立的allocator hierarchy
 */
class SessionAllocatorManager {
public:
    /**
     * @brief 获取或创建session专用allocator
     * @param client_id 客户端ID
     * @param memory_limit 内存限制（字节），0表示无限制
     * @return session专用allocator
     */
    static MQTTAllocator* get_session_allocator(const std::string& client_id, size_t memory_limit = 0);
    
    /**
     * @brief 获取或创建session队列专用allocator
     * @param client_id 客户端ID
     * @return session队列专用allocator
     */
    static MQTTAllocator* get_session_queue_allocator(const std::string& client_id);
    
    /**
     * @brief 获取或创建session worker专用allocator
     * @param client_id 客户端ID
     * @return session worker专用allocator
     */
    static MQTTAllocator* get_session_worker_allocator(const std::string& client_id);
    
    /**
     * @brief 获取全局消息缓存allocator
     * @return 消息缓存allocator
     */
    static MQTTAllocator* get_message_cache_allocator();
    
    /**
     * @brief 清理session相关的所有allocator
     * @param client_id 客户端ID
     */
    static void cleanup_session_allocators(const std::string& client_id);
    
    /**
     * @brief 获取session内存使用统计
     * @param client_id 客户端ID
     * @return 内存使用量（字节）
     */
    static size_t get_session_memory_usage(const std::string& client_id);
    
    /**
     * @brief 检查session是否超过内存限制
     * @param client_id 客户端ID
     * @return true表示超过限制
     */
    static bool is_session_memory_limit_exceeded(const std::string& client_id);

private:
    // 防止实例化
    SessionAllocatorManager() = delete;
    ~SessionAllocatorManager() = delete;
    
    // 内部辅助函数
    static std::string make_session_allocator_id(const std::string& client_id, const std::string& suffix);
};

// Session专用的容器类型定义
template<typename T>
using SessionVector = std::vector<T, MQTTSTLAllocator<T>>;

template<typename K, typename V>
using SessionUnorderedMap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, 
                                               MQTTSTLAllocator<std::pair<const K, V>>>;

template<typename T>
using SessionQueue = std::queue<T, std::deque<T, MQTTSTLAllocator<T>>>;

// Session专用的字符串和智能指针类型
using SessionString = std::basic_string<char, std::char_traits<char>, MQTTSTLAllocator<char>>;

/**
 * @brief 使用session allocator的智能指针创建函数
 */
template<typename T, typename... Args>
std::unique_ptr<T> make_session_unique(MQTTAllocator* allocator, Args&&... args) {
    void* ptr = allocator->allocate(sizeof(T));
    if (!ptr) {
        throw std::bad_alloc();
    }
    
    try {
        T* obj = new(ptr) T(std::forward<Args>(args)...);
        return std::unique_ptr<T>(obj, [allocator](T* p) {
            if (p) {
                p->~T();
                allocator->deallocate(p, sizeof(T));
            }
        });
    } catch (...) {
        allocator->deallocate(ptr, sizeof(T));
        throw;
    }
}

/**
 * @brief 使用session allocator的共享指针创建函数
 */
template<typename T, typename... Args>
std::shared_ptr<T> make_session_shared(MQTTAllocator* allocator, Args&&... args) {
    void* ptr = allocator->allocate(sizeof(T));
    if (!ptr) {
        throw std::bad_alloc();
    }
    
    try {
        T* obj = new(ptr) T(std::forward<Args>(args)...);
        return std::shared_ptr<T>(obj, [allocator](T* p) {
            if (p) {
                p->~T();
                allocator->deallocate(p, sizeof(T));
            }
        });
    } catch (...) {
        allocator->deallocate(ptr, sizeof(T));
        throw;
    }
}

/**
 * @brief Session专用的容器工厂函数
 */
template<typename T>
SessionVector<T> make_session_vector(MQTTAllocator* allocator) {
    return SessionVector<T>(MQTTSTLAllocator<T>(allocator));
}

template<typename K, typename V>
SessionUnorderedMap<K, V> make_session_unordered_map(MQTTAllocator* allocator) {
    return SessionUnorderedMap<K, V>(0, std::hash<K>(), std::equal_to<K>(), 
                                     MQTTSTLAllocator<std::pair<const K, V>>(allocator));
}

template<typename T>
SessionQueue<T> make_session_queue(MQTTAllocator* allocator) {
    return SessionQueue<T>(std::deque<T, MQTTSTLAllocator<T>>(MQTTSTLAllocator<T>(allocator)));
}

SessionString make_session_string(MQTTAllocator* allocator, const std::string& str) {
    return SessionString(str.begin(), str.end(), MQTTSTLAllocator<char>(allocator));
}

} // namespace mqtt

#endif // MQTT_SESSION_ALLOCATOR_H