#include "mqtt_auth_factory.h"
#include "mqtt_auth_http.h"
#include "mqtt_auth_sqlite.h"
#include "mqtt_auth_redis.h"
#include "logger.h"

namespace mqtt {
namespace auth {
int configure_auth(const mqtt::AuthConfig& config, MQTTAllocator* allocator,
                   std::unique_ptr<AuthManager>& manager)
{
  manager.reset();
  if (!config.enabled) return MQ_SUCCESS;
  if (config.allow_anonymous) {
    LOG_ERROR("Authenticated listeners do not permit anonymous fallback");
    return MQ_ERR_INVALID_ARGS;
  }
  std::unique_ptr<AuthManager> next(new AuthManager(allocator));
  size_t count = 0;
  for (const auto& entry : config.providers) {
    if (!entry.enabled) continue;
    std::unique_ptr<IAuthProvider> provider;
    if (entry.type == "http") {
      provider.reset(new HttpAuthProvider(entry.settings));
    }
#ifdef HAVE_SQLITE3
    else if (entry.type == "sqlite") {
      auth::SQLiteAuthConfig settings;
      settings.db_path = config.sqlite.db_path;
      settings.connection_pool_size = config.sqlite.connection_pool_size;
      settings.max_retry_count = config.sqlite.max_retry_count;
      settings.retry_delay_ms = config.sqlite.retry_delay_ms;
      settings.query_timeout_ms = config.sqlite.query_timeout_ms;
      settings.enable_wal_mode = config.sqlite.enable_wal_mode;
      settings.enable_foreign_keys = config.sqlite.enable_foreign_keys;
      settings.cache_size_kb = config.sqlite.cache_size_kb;
      provider.reset(new SQLiteAuthProvider(settings, allocator));
    }
#endif
#ifdef HAVE_HIREDIS
    else if (entry.type == "redis") {
      auth::RedisAuthConfig settings;
      settings.host = config.redis.host;
      settings.port = config.redis.port;
      settings.password = config.redis.password;
      settings.database = config.redis.database;
      settings.connection_pool_size = config.redis.connection_pool_size;
      settings.max_retry_count = config.redis.max_retry_count;
      settings.retry_delay_ms = config.redis.retry_delay_ms;
      settings.connect_timeout_ms = config.redis.connect_timeout_ms;
      settings.command_timeout_ms = config.redis.command_timeout_ms;
      settings.keepalive_interval_s = config.redis.keepalive_interval_s;
      settings.key_prefix = config.redis.key_prefix;
      settings.cache_ttl_seconds = config.redis.cache_ttl_seconds;
      provider.reset(new RedisAuthProvider(settings, allocator));
    }
#endif
    if (!provider || next->add_provider(std::move(provider), entry.priority) != MQ_SUCCESS) {
      LOG_ERROR("Invalid or unavailable authentication provider: {}", entry.type);
      return MQ_ERR_INVALID_ARGS;
    }
    ++count;
  }
  if (count == 0) return MQ_ERR_INVALID_ARGS;
  next->set_cache_enabled(config.cache_enabled, config.cache_ttl_seconds);
  int result = next->initialize();
  if (result == MQ_SUCCESS) manager = std::move(next);
  return result;
}
}
}
