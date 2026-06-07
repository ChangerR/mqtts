#include "mqtt_auth_interface.h"
#include "logger.h"
#include <algorithm>
#include <mutex>

namespace mqtt {
namespace auth {

namespace
{

void split_topic_levels_local(const std::string& value, std::vector<std::string>& levels)
{
    size_t start_pos = 0;
    size_t slash_pos = 0;

    levels.clear();
    while (start_pos <= value.length()) {
        slash_pos = value.find('/', start_pos);
        if (slash_pos == std::string::npos) {
            levels.push_back(value.substr(start_pos));
            start_pos = value.length() + 1;
        } else {
            levels.push_back(value.substr(start_pos, slash_pos - start_pos));
            start_pos = slash_pos + 1;
        }
    }
}

bool match_topic_pattern_local(const std::string& topic, const std::string& pattern)
{
    bool matched = false;
    std::vector<std::string> topic_levels;
    std::vector<std::string> pattern_levels;
    size_t topic_index = 0;
    size_t pattern_index = 0;

    split_topic_levels_local(topic, topic_levels);
    split_topic_levels_local(pattern, pattern_levels);

    while (!matched && topic_index < topic_levels.size() && pattern_index < pattern_levels.size()) {
        if (pattern_levels[pattern_index] == "#") {
            matched = (pattern_index == pattern_levels.size() - 1);
            topic_index = topic_levels.size();
            pattern_index = pattern_levels.size();
        } else if (pattern_levels[pattern_index] == "+" ||
                   pattern_levels[pattern_index] == topic_levels[topic_index]) {
            ++topic_index;
            ++pattern_index;
        } else {
            topic_index = topic_levels.size();
            pattern_index = pattern_levels.size();
        }
    }

    if (!matched) {
        if (topic_index == topic_levels.size() && pattern_index == pattern_levels.size()) {
            matched = true;
        } else if (pattern_index < pattern_levels.size() &&
                   pattern_levels[pattern_index] == "#" &&
                   pattern_index == pattern_levels.size() - 1) {
            matched = true;
        }
    }

    return matched;
}

}  // namespace

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
    
    std::unique_lock<std::mutex> lock(providers_mutex_);
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
    
    std::unique_lock<std::mutex> lock(providers_mutex_);
    for (auto& entry : providers_) {
        entry.provider->cleanup();
    }
    providers_.clear();
    
    std::unique_lock<std::mutex> cache_lock(cache_mutex_);
    auth_cache_.clear();
}

int AuthManager::add_provider(std::unique_ptr<IAuthProvider> provider, int priority) {
    if (!provider) {
        LOG_ERROR("Cannot add null auth provider");
        return MQ_ERR_INVALID_ARGS;
    }
    
    const char* provider_name = provider->get_provider_name();
    LOG_INFO("Adding auth provider: {} with priority: {}", provider_name, priority);
    
    std::unique_lock<std::mutex> lock(providers_mutex_);
    
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
    
    std::unique_lock<std::mutex> lock(providers_mutex_);
    
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
    
    std::unique_lock<std::mutex> lock(providers_mutex_);
    
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

int AuthManager::authenticate_user(const MQTTString& username,
                                   const MQTTString& password,
                                   const MQTTString& client_id,
                                   const MQTTString& client_ip,
                                   uint16_t client_port,
                                   AuthResult& auth_result,
                                   ClientAuthContext& auth_context) {
    int ret = MQ_SUCCESS;
    std::vector<TopicPermission> permissions;
    IAuthProvider* matched_provider = NULL;

    auth_context.user_info.username.clear();
    auth_context.user_info.client_id.clear();
    auth_context.user_info.client_ip.clear();
    auth_context.user_info.client_port = 0;
    auth_context.user_info.is_super_user = false;
    auth_context.acl_rules.clear();
    auth_context.is_super_user = false;
    auth_context.acl_version = 0;
    auth_context.expires_at_ms = 0;
    auth_result = AuthResult::INTERNAL_ERROR;

    cleanup_expired_cache();
    {
        std::unique_lock<std::mutex> lock(providers_mutex_);

        if (providers_.empty()) {
            auth_result = AuthResult::ACCESS_DENIED;
        } else {
            for (const auto& entry : providers_) {
                if (!entry.provider->is_healthy()) {
                    continue;
                }

                auth_result = entry.provider->authenticate_user(
                    username, password, client_id, client_ip, client_port, auth_context.user_info);
                if (auth_result == AuthResult::SUCCESS) {
                    matched_provider = entry.provider.get();
                    break;
                } else if (auth_result == AuthResult::USER_NOT_FOUND) {
                    continue;
                } else {
                    break;
                }
            }

            if (auth_result == AuthResult::SUCCESS && matched_provider != NULL) {
                permissions.clear();
                ret = matched_provider->get_user_permissions(auth_context.user_info.username, permissions);
                if (ret == MQ_ERR_NOT_FOUND_V2) {
                    ret = MQ_SUCCESS;
                }
                for (size_t i = 0; MQ_SUCCESS == ret && i < permissions.size(); ++i) {
                    TopicAclRule rule(allocator_);
                    rule.topic_pattern = permissions[i].topic_pattern;
                    rule.permission = permissions[i].permission;
                    rule.max_qos = 2;
                    auth_context.acl_rules.push_back(rule);
                }
            }
        }
    }

    if (MQ_SUCCESS == ret && auth_result == AuthResult::SUCCESS) {
        auth_context.is_super_user = auth_context.user_info.is_super_user;
        auth_context.acl_version = 1;
        if (cache_enabled_ && cache_ttl_seconds_ > 0) {
            auth_context.expires_at_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()) +
                static_cast<uint64_t>(cache_ttl_seconds_) * 1000;
            put_to_cache(make_cache_key(username, client_id), auth_result, auth_context.user_info);
        }
    }

    return ret;
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
    
    std::unique_lock<std::mutex> lock(providers_mutex_);
    
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

int AuthManager::check_topic_access(const ClientAuthContext& auth_context,
                                    const MQTTString& topic,
                                    Permission permission,
                                    AuthResult& auth_result) {
    int ret = MQ_SUCCESS;
    bool matched = false;
    const std::string topic_str = from_mqtt_string(topic);

    auth_result = AuthResult::ACCESS_DENIED;
    if (auth_context.is_super_user) {
        auth_result = AuthResult::SUCCESS;
    } else if (!auth_context.acl_rules.empty()) {
        for (size_t i = 0; i < auth_context.acl_rules.size() && !matched; ++i) {
            const TopicAclRule& rule = auth_context.acl_rules[i];
            if (match_topic_pattern_local(topic_str, from_mqtt_string(rule.topic_pattern))) {
                matched = true;
                if (rule.permission == Permission::READWRITE ||
                    rule.permission == permission) {
                    auth_result = AuthResult::SUCCESS;
                }
            }
        }
    } else {
        auth_result = check_topic_access(auth_context.user_info, topic, permission);
    }

    return ret;
}

std::map<std::string, AuthStats> AuthManager::get_all_stats() const {
    std::map<std::string, AuthStats> all_stats;
    
    std::unique_lock<std::mutex> lock(providers_mutex_);
    for (const auto& entry : providers_) {
        all_stats[entry.provider->get_provider_name()] = entry.provider->get_stats();
    }
    
    return all_stats;
}

void AuthManager::set_cache_enabled(bool enabled, int cache_ttl_seconds) {
    cache_enabled_ = enabled;
    cache_ttl_seconds_ = cache_ttl_seconds;
    
    if (!enabled) {
        std::unique_lock<std::mutex> lock(cache_mutex_);
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
    std::unique_lock<std::mutex> lock(cache_mutex_);
    
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
    
    std::unique_lock<std::mutex> lock(cache_mutex_);
    
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
    
    std::unique_lock<std::mutex> lock(cache_mutex_);
    
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
