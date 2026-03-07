#include "mqtt_auth_interface.h"
#include "logger.h"
#include <algorithm>

namespace mqtt {
namespace auth {

AuthManager::AuthManager(MQTTAllocator* allocator)
    : allocator_(allocator),
      cache_enabled_(false),
      cache_ttl_seconds_(300) {
}

AuthManager::~AuthManager() {
    cleanup();
}

int AuthManager::initialize() {
    LOG_INFO("Initializing AuthManager");

    mqtt::CoroLockGuard lock(&providers_mutex_);
    for (auto& entry : providers_) {
        int ret = entry.provider->initialize();
        if (ret != MQ_SUCCESS) {
            LOG_ERROR("Failed to initialize auth provider: {}, error: {}", 
                     entry.provider->get_provider_name(), ret);
            return ret;
        }
        LOG_INFO("Successfully initialized auth provider: {}", 
                entry.provider->get_provider_name());
    }
    
    LOG_INFO("AuthManager initialized successfully with {} providers", providers_.size());
    return MQ_SUCCESS;
}

void AuthManager::cleanup() {
    LOG_INFO("Cleaning up AuthManager");

    mqtt::CoroLockGuard lock(&providers_mutex_);
    for (auto& entry : providers_) {
        entry.provider->cleanup();
    }
    providers_.clear();

    mqtt::CoroLockGuard cache_lock(&cache_mutex_);
    auth_cache_.clear();
}

int AuthManager::add_provider(std::unique_ptr<IAuthProvider> provider, int priority) {
    if (!provider) {
        LOG_ERROR("Cannot add null auth provider");
        return MQ_ERR_INVALID_ARGS;
    }
    
    const char* provider_name = provider->get_provider_name();
    LOG_INFO("Adding auth provider: {} with priority: {}", provider_name, priority);

    mqtt::CoroLockGuard lock(&providers_mutex_);
    
    // 检查是否已存在同名提供者
    for (const auto& entry : providers_) {
        if (std::string(entry.provider->get_provider_name()) == provider_name) {
            LOG_ERROR("Auth provider with name '{}' already exists", provider_name);
            return MQ_ERR_INVALID_ARGS;
        }
    }
    
    providers_.emplace_back(std::move(provider), priority);
    sort_providers_by_priority();
    
    LOG_INFO("Successfully added auth provider: {}", provider_name);
    return MQ_SUCCESS;
}

int AuthManager::remove_provider(const std::string& provider_name) {
    LOG_INFO("Removing auth provider: {}", provider_name);

    mqtt::CoroLockGuard lock(&providers_mutex_);
    
    auto it = std::find_if(providers_.begin(), providers_.end(),
        [&provider_name](const ProviderEntry& entry) {
            return std::string(entry.provider->get_provider_name()) == provider_name;
        });
    
    if (it == providers_.end()) {
        LOG_WARN("Auth provider '{}' not found", provider_name);
        return MQ_ERR_NOT_FOUND_V2;
    }
    
    it->provider->cleanup();
    providers_.erase(it);
    
    LOG_INFO("Successfully removed auth provider: {}", provider_name);
    return MQ_SUCCESS;
}

AuthResult AuthManager::authenticate_user(const MQTTString& username,
                                         const MQTTString& password,
                                         const MQTTString& client_id,
                                         const MQTTString& client_ip,
                                         uint16_t client_port,
                                         UserInfo& user_info) {
    // 清理过期缓存
    cleanup_expired_cache();
    
    // 检查缓存
    if (cache_enabled_) {
        std::string cache_key = make_cache_key(username, client_id);
        AuthResult cached_result;
        if (get_from_cache(cache_key, cached_result, user_info)) {
            LOG_DEBUG("Cache hit for user authentication: {}", from_mqtt_string(username));
            return cached_result;
        }
    }
    
    mqtt::CoroLockGuard lock(&providers_mutex_);

    if (providers_.empty()) {
        LOG_WARN("No auth providers configured, denying access");
        return AuthResult::ACCESS_DENIED;
    }
    
    // 按优先级尝试各个提供者
    AuthResult last_result = AuthResult::INTERNAL_ERROR;
    for (const auto& entry : providers_) {
        if (!entry.provider->is_healthy()) {
            LOG_WARN("Auth provider '{}' is not healthy, skipping", 
                    entry.provider->get_provider_name());
            continue;
        }
        
        LOG_DEBUG("Trying auth provider: {}", entry.provider->get_provider_name());
        
        AuthResult result = entry.provider->authenticate_user(
            username, password, client_id, client_ip, client_port, user_info);
        
        if (result == AuthResult::SUCCESS) {
            LOG_INFO("User '{}' authenticated successfully using provider: {}", 
                    from_mqtt_string(username), entry.provider->get_provider_name());
            
            // 缓存成功结果
            if (cache_enabled_) {
                std::string cache_key = make_cache_key(username, client_id);
                put_to_cache(cache_key, result, user_info);
            }
            
            return result;
        }
        
        if (result == AuthResult::USER_NOT_FOUND) {
            // 用户不存在，尝试下一个提供者
            last_result = result;
            continue;
        }
        
        // 其他错误（如密码错误），不再尝试其他提供者
        LOG_WARN("User '{}' authentication failed with provider '{}': {}", 
                from_mqtt_string(username), entry.provider->get_provider_name(), 
                static_cast<int>(result));
        return result;
    }
    
    LOG_WARN("User '{}' authentication failed with all providers", from_mqtt_string(username));
    return last_result;
}

AuthResult AuthManager::check_topic_access(const UserInfo& user_info,
                                          const MQTTString& topic,
                                          Permission permission) {
    // 超级用户拥有所有权限
    if (user_info.is_super_user) {
        LOG_DEBUG("Super user '{}' granted access to topic: {}",
                 from_mqtt_string(user_info.username), from_mqtt_string(topic));
        return AuthResult::SUCCESS;
    }

    // 匿名用户默认允许访问所有主题
    // 匿名用户标识为 "<anonymous>"
    if (from_mqtt_string(user_info.username) == "<anonymous>") {
        LOG_DEBUG("Anonymous user from {}:{} granted {} access to topic: {}",
                 from_mqtt_string(user_info.client_ip), user_info.client_port,
                 static_cast<int>(permission), from_mqtt_string(topic));
        return AuthResult::SUCCESS;
    }

    mqtt::CoroLockGuard lock(&providers_mutex_);

    if (providers_.empty()) {
        LOG_WARN("No auth providers configured for topic access check");
        return AuthResult::ACCESS_DENIED;
    }
    
    // 按优先级检查各个提供者
    for (const auto& entry : providers_) {
        if (!entry.provider->is_healthy()) {
            continue;
        }
        
        AuthResult result = entry.provider->check_topic_access(user_info, topic, permission);
        
        if (result == AuthResult::SUCCESS) {
            LOG_DEBUG("User '{}' granted {} access to topic '{}' by provider: {}", 
                     from_mqtt_string(user_info.username), static_cast<int>(permission),
                     from_mqtt_string(topic), entry.provider->get_provider_name());
            return result;
        }
        
        if (result != AuthResult::USER_NOT_FOUND) {
            // 明确拒绝，不再尝试其他提供者
            LOG_DEBUG("User '{}' denied {} access to topic '{}' by provider: {}", 
                     from_mqtt_string(user_info.username), static_cast<int>(permission),
                     from_mqtt_string(topic), entry.provider->get_provider_name());
            return result;
        }
    }
    
    LOG_WARN("No provider handled topic access check for user '{}', topic '{}'", 
            from_mqtt_string(user_info.username), from_mqtt_string(topic));
    return AuthResult::ACCESS_DENIED;
}

std::map<std::string, AuthStats> AuthManager::get_all_stats() const {
    std::map<std::string, AuthStats> all_stats;

    mqtt::CoroLockGuard lock(&providers_mutex_);
    for (const auto& entry : providers_) {
        all_stats[entry.provider->get_provider_name()] = entry.provider->get_stats();
    }
    
    return all_stats;
}

void AuthManager::set_cache_enabled(bool enabled, int cache_ttl_seconds) {
    cache_enabled_ = enabled;
    cache_ttl_seconds_ = cache_ttl_seconds;

    if (!enabled) {
        mqtt::CoroLockGuard lock(&cache_mutex_);
        auth_cache_.clear();
    }
    
    LOG_INFO("Auth cache {} with TTL: {}s", enabled ? "enabled" : "disabled", cache_ttl_seconds);
}

void AuthManager::sort_providers_by_priority() {
    std::sort(providers_.begin(), providers_.end(),
        [](const ProviderEntry& a, const ProviderEntry& b) {
            return a.priority < b.priority;
        });
}

std::string AuthManager::make_cache_key(const MQTTString& username, const MQTTString& client_id) const {
    return from_mqtt_string(username) + ":" + from_mqtt_string(client_id);
}

bool AuthManager::get_from_cache(const std::string& key, AuthResult& result, UserInfo& user_info) const {
    mqtt::CoroLockGuard lock(&cache_mutex_);
    
    auto it = auth_cache_.find(key);
    if (it == auth_cache_.end()) {
        return false;
    }
    
    const CacheEntry& entry = it->second;
    if (std::chrono::steady_clock::now() > entry.expire_time) {
        return false;
    }
    
    result = entry.result;
    // 复制用户信息
    user_info.username = entry.user_info.username;
    user_info.client_id = entry.user_info.client_id;
    user_info.client_ip = entry.user_info.client_ip;
    user_info.client_port = entry.user_info.client_port;
    user_info.is_super_user = entry.user_info.is_super_user;
    
    return true;
}

void AuthManager::put_to_cache(const std::string& key, AuthResult result, const UserInfo& user_info) {
    if (!cache_enabled_) {
        return;
    }

    mqtt::CoroLockGuard lock(&cache_mutex_);

    CacheEntry entry(allocator_);
    entry.result = result;
    entry.user_info.username = user_info.username;
    entry.user_info.client_id = user_info.client_id;
    entry.user_info.client_ip = user_info.client_ip;
    entry.user_info.client_port = user_info.client_port;
    entry.user_info.is_super_user = user_info.is_super_user;
    entry.expire_time = std::chrono::steady_clock::now() + 
                       std::chrono::seconds(cache_ttl_seconds_);
    
    auth_cache_.emplace(key, std::move(entry));
}

void AuthManager::cleanup_expired_cache() {
    if (!cache_enabled_) {
        return;
    }

    mqtt::CoroLockGuard lock(&cache_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto it = auth_cache_.begin();
    while (it != auth_cache_.end()) {
        if (now > it->second.expire_time) {
            it = auth_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace auth
} // namespace mqtt