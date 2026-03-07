#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include "mqtt_allocator.h"
#include "mqtt_define.h"
#include "mqtt_stl_allocator.h"
#include "mqtt_coroutine_utils.h"

namespace mqtt {
namespace auth {

/**
 * @brief 认证结果
 */
enum class AuthResult {
  SUCCESS = 0,           // 认证成功
  INVALID_CREDENTIALS,   // 无效凭据
  ACCESS_DENIED,         // 访问被拒绝
  USER_NOT_FOUND,        // 用户不存在
  TOPIC_ACCESS_DENIED,   // 主题访问被拒绝
  INTERNAL_ERROR,        // 内部错误
  TIMEOUT,               // 超时
  RATE_LIMITED          // 频率限制
};

/**
 * @brief 权限类型
 */
enum class Permission {
  NONE = 0,
  READ = 1,      // 订阅权限
  WRITE = 2,     // 发布权限
  READWRITE = 3  // 读写权限
};

/**
 * @brief 用户信息
 */
struct UserInfo {
  MQTTString username;
  MQTTString client_id;
  MQTTString client_ip;
  uint16_t client_port;
  bool is_super_user;
  
  UserInfo(MQTTAllocator* allocator) 
    : username(MQTTStrAllocator(allocator)),
      client_id(MQTTStrAllocator(allocator)),
      client_ip(MQTTStrAllocator(allocator)),
      client_port(0),
      is_super_user(false) {}
};

/**
 * @brief 主题权限信息
 */
struct TopicPermission {
  MQTTString topic_pattern;
  Permission permission;
  
  TopicPermission(MQTTAllocator* allocator) 
    : topic_pattern(MQTTStrAllocator(allocator)),
      permission(Permission::NONE) {}
};

/**
 * @brief 认证统计信息
 */
struct AuthStats {
  uint64_t total_login_attempts;
  uint64_t successful_logins;
  uint64_t failed_logins;
  uint64_t total_topic_checks;
  uint64_t topic_access_granted;
  uint64_t topic_access_denied;
  uint64_t cache_hits;
  uint64_t cache_misses;
  
  AuthStats() : total_login_attempts(0), successful_logins(0), failed_logins(0),
                total_topic_checks(0), topic_access_granted(0), topic_access_denied(0),
                cache_hits(0), cache_misses(0) {}
};

/**
 * @brief 抽象认证提供者接口
 * 
 * 这个接口定义了所有认证提供者必须实现的方法，支持：
 * 1. 用户登录认证
 * 2. 主题访问权限检查
 * 3. 用户权限管理
 * 4. 性能统计
 */
class IAuthProvider {
public:
  virtual ~IAuthProvider() = default;

  /**
   * @brief 初始化认证提供者
   * @return MQ_SUCCESS成功，其他值失败
   */
  virtual int initialize() = 0;

  /**
   * @brief 清理资源
   */
  virtual void cleanup() = 0;

  /**
   * @brief 用户登录认证
   * @param username 用户名
   * @param password 密码
   * @param client_id 客户端ID
   * @param client_ip 客户端IP
   * @param client_port 客户端端口
   * @param user_info 输出用户信息
   * @return 认证结果
   */
  virtual AuthResult authenticate_user(const MQTTString& username,
                                     const MQTTString& password,
                                     const MQTTString& client_id,
                                     const MQTTString& client_ip,
                                     uint16_t client_port,
                                     UserInfo& user_info) = 0;

  /**
   * @brief 检查主题访问权限
   * @param user_info 用户信息
   * @param topic 主题
   * @param permission 请求的权限类型
   * @return 认证结果
   */
  virtual AuthResult check_topic_access(const UserInfo& user_info,
                                       const MQTTString& topic,
                                       Permission permission) = 0;

  /**
   * @brief 检查用户是否有超级用户权限
   * @param username 用户名
   * @return true为超级用户，false为普通用户
   */
  virtual bool is_super_user(const MQTTString& username) = 0;

  /**
   * @brief 获取认证统计信息
   * @return 统计信息
   */
  virtual AuthStats get_stats() const = 0;

  /**
   * @brief 重置统计信息
   */
  virtual void reset_stats() = 0;

  /**
   * @brief 获取提供者名称
   * @return 提供者名称
   */
  virtual const char* get_provider_name() const = 0;

  /**
   * @brief 检查提供者是否健康
   * @return true健康，false不健康
   */
  virtual bool is_healthy() const = 0;
};

/**
 * @brief 认证管理器
 * 
 * 管理多个认证提供者，支持链式认证和缓存
 */
class AuthManager {
public:
  explicit AuthManager(MQTTAllocator* allocator);
  ~AuthManager();

  /**
   * @brief 初始化认证管理器
   * @return MQ_SUCCESS成功，其他值失败
   */
  int initialize();

  /**
   * @brief 清理资源
   */
  void cleanup();

  /**
   * @brief 添加认证提供者
   * @param provider 认证提供者
   * @param priority 优先级（数字越小优先级越高）
   * @return MQ_SUCCESS成功，其他值失败
   */
  int add_provider(std::unique_ptr<IAuthProvider> provider, int priority = 100);

  /**
   * @brief 移除认证提供者
   * @param provider_name 提供者名称
   * @return MQ_SUCCESS成功，其他值失败
   */
  int remove_provider(const std::string& provider_name);

  /**
   * @brief 用户登录认证
   * @param username 用户名
   * @param password 密码
   * @param client_id 客户端ID
   * @param client_ip 客户端IP
   * @param client_port 客户端端口
   * @param user_info 输出用户信息
   * @return 认证结果
   */
  AuthResult authenticate_user(const MQTTString& username,
                             const MQTTString& password,
                             const MQTTString& client_id,
                             const MQTTString& client_ip,
                             uint16_t client_port,
                             UserInfo& user_info);

  /**
   * @brief 检查主题访问权限
   * @param user_info 用户信息
   * @param topic 主题
   * @param permission 请求的权限类型
   * @return 认证结果
   */
  AuthResult check_topic_access(const UserInfo& user_info,
                               const MQTTString& topic,
                               Permission permission);

  /**
   * @brief 获取所有提供者的统计信息
   * @return 统计信息映射
   */
  std::map<std::string, AuthStats> get_all_stats() const;

  /**
   * @brief 启用/禁用缓存
   * @param enabled 是否启用
   * @param cache_ttl_seconds 缓存TTL（秒）
   */
  void set_cache_enabled(bool enabled, int cache_ttl_seconds = 300);

private:
  struct ProviderEntry {
    std::unique_ptr<IAuthProvider> provider;
    int priority;
    
    ProviderEntry(std::unique_ptr<IAuthProvider> p, int prio) 
      : provider(std::move(p)), priority(prio) {}
  };

  MQTTAllocator* allocator_;
  std::vector<ProviderEntry> providers_;
  mutable mqtt::CoroMutex providers_mutex_;
  
  // 缓存相关
  bool cache_enabled_;
  int cache_ttl_seconds_;
  
  // 简单的内存缓存实现
  struct CacheEntry {
    AuthResult result;
    UserInfo user_info;
    std::chrono::steady_clock::time_point expire_time;
    
    CacheEntry(MQTTAllocator* allocator) : user_info(allocator) {}
  };
  
  mutable std::unordered_map<std::string, CacheEntry> auth_cache_;
  mutable mqtt::CoroMutex cache_mutex_;
  
  void sort_providers_by_priority();
  std::string make_cache_key(const MQTTString& username, const MQTTString& client_id) const;
  bool get_from_cache(const std::string& key, AuthResult& result, UserInfo& user_info) const;
  void put_to_cache(const std::string& key, AuthResult result, const UserInfo& user_info);
  void cleanup_expired_cache();
};

} // namespace auth
} // namespace mqtt