#include "mqtt_config.h"
#include <yaml-cpp/yaml.h>
#include <string>
#include "logger.h"

namespace mqtt {
namespace {

bool key_in(const std::vector<const char*>& keys, const std::string& key)
{
  for (const char* item : keys) {
    if (key == item) {
      return true;
    }
  }
  return false;
}

const std::vector<const char*> kServerKeys = {"bind_address", "port", "max_connections", "backlog",
                                              "thread_count"};
const std::vector<const char*> kMqttKeys = {
    "max_packet_size",
    "keep_alive_max",
    "connect_timeout_seconds",
    "implicit_no_local",
    "max_qos",
    "retain_available",
    "wildcard_subscription_available",
    "subscription_identifier_available",
    "shared_subscription_available",
    "topic_alias_maximum",
    "receive_maximum",
    "allow_mqtt3x"};
const std::vector<const char*> kMqttDeprecated = {"keep_alive_default"};
const std::vector<const char*> kMemoryKeys = {"client_max_size"};
const std::vector<const char*> kRetainKeys = {"max_messages", "max_payload_bytes", "max_total_bytes",
                                              "max_deliver_per_subscribe"};
const std::vector<const char*> kLogKeys = {"level", "file_path", "max_file_size", "max_files",
                                           "flush_immediately"};
const std::vector<const char*> kMonitoringKeys = {"enabled", "interval_seconds", "verbose_output",
                                                  "json_output_file"};
const std::vector<const char*> kPersistenceKeys = {
    "enabled",
    "path",
    "partitions",
    "segment_bytes",
    "max_disk_bytes",
    "checkpoint_interval_ms",
    "max_session_expiry_seconds",
    "max_sessions",
    "max_subscriptions_per_session",
    "max_messages",
    "max_messages_per_session",
    "max_bytes_per_session",
    "overflow_policy",
    "max_bytes",
    "max_requests",
    "max_request_bytes",
    "max_inflight"};
const std::vector<const char*> kAuthKeys = {"enabled", "allow_anonymous", "cache_enabled",
                                            "cache_ttl_seconds", "providers", "sqlite", "redis"};
const std::vector<const char*> kSqliteKeys = {"db_path",           "connection_pool_size",
                                              "max_retry_count",   "retry_delay_ms",
                                              "query_timeout_ms",  "enable_wal_mode",
                                              "enable_foreign_keys", "cache_size_kb"};
const std::vector<const char*> kRedisKeys = {
    "host",           "port",         "password",        "database",
    "connection_pool_size", "max_retry_count", "retry_delay_ms", "connect_timeout_ms",
    "command_timeout_ms", "keepalive_interval_s", "key_prefix", "cache_ttl_seconds"};
const std::vector<const char*> kProviderKeys = {"type", "priority", "enabled", "settings"};

}  // namespace

ConfigManager::ConfigManager() = default;

ConfigManager::~ConfigManager() = default;

void ConfigManager::note_warning(const std::string& message)
{
  warnings_.push_back(message);
  LOG_WARN("{}", message);
}

void ConfigManager::note_error(const std::string& message)
{
  last_error_ = message;
  LOG_ERROR("{}", message);
}

void ConfigManager::warn_leaves(const YAML::Node& node, const std::string& path, const char* reason)
{
  if (!node || node.IsNull()) {
    return;
  }
  if (node.IsMap()) {
    if (node.size() == 0) {
      note_warning("配置键 " + path + " " + reason);
      return;
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
      std::string key = it->first.as<std::string>();
      warn_leaves(it->second, path.empty() ? key : path + "." + key, reason);
    }
    return;
  }
  if (node.IsSequence()) {
    if (node.size() == 0) {
      note_warning("配置键 " + path + " " + reason);
      return;
    }
    int index = 0;
    for (const auto& item : node) {
      warn_leaves(item, path + "[" + std::to_string(index++) + "]", reason);
    }
    return;
  }
  note_warning("配置键 " + path + " " + reason);
}

void ConfigManager::audit_map(const YAML::Node& node, const std::string& path,
                              const std::vector<const char*>& effective,
                              const std::vector<const char*>& deprecated)
{
  if (!node || node.IsNull()) {
    return;
  }
  if (!node.IsMap()) {
    note_warning("配置键 " + path + " 应为映射，已忽略");
    return;
  }
  for (auto it = node.begin(); it != node.end(); ++it) {
    const std::string key = it->first.as<std::string>();
    const std::string child = path.empty() ? key : path + "." + key;
    if (key_in(deprecated, key)) {
      note_warning("配置键 " + child + " 已废弃并忽略");
    } else if (!key_in(effective, key)) {
      warn_leaves(it->second, child, "未识别，已忽略");
    }
  }
}

void ConfigManager::audit_auth(const YAML::Node& node)
{
  if (!node || node.IsNull()) {
    return;
  }
  if (!node.IsMap()) {
    note_warning("配置键 auth 应为映射，已忽略");
    return;
  }
  for (auto it = node.begin(); it != node.end(); ++it) {
    const std::string key = it->first.as<std::string>();
    if (key == "sqlite") {
      audit_map(it->second, "auth.sqlite", kSqliteKeys, {});
    } else if (key == "redis") {
      audit_map(it->second, "auth.redis", kRedisKeys, {});
    } else if (key == "providers") {
      if (!it->second.IsSequence()) {
        note_warning("配置键 auth.providers 应为列表，已忽略");
        continue;
      }
      int index = 0;
      for (const auto& provider : it->second) {
        const std::string path = "auth.providers[" + std::to_string(index++) + "]";
        if (!provider.IsMap()) {
          note_warning("配置键 " + path + " 应为映射，已忽略");
          continue;
        }
        for (auto field = provider.begin(); field != provider.end(); ++field) {
          const std::string field_key = field->first.as<std::string>();
          if (field_key == "settings") {
            continue;
          }
          if (!key_in(kProviderKeys, field_key)) {
            warn_leaves(field->second, path + "." + field_key, "未识别，已忽略");
          }
        }
      }
    } else if (!key_in(kAuthKeys, key)) {
      warn_leaves(it->second, "auth." + key, "未识别，已忽略");
    }
  }
}

void ConfigManager::audit_config(const YAML::Node& root)
{
  if (!root || !root.IsMap()) {
    return;
  }
  for (auto it = root.begin(); it != root.end(); ++it) {
    const std::string key = it->first.as<std::string>();
    if (key == "server") {
      audit_map(it->second, "server", kServerKeys, {});
    } else if (key == "mqtt") {
      audit_map(it->second, "mqtt", kMqttKeys, kMqttDeprecated);
    } else if (key == "memory") {
      audit_map(it->second, "memory", kMemoryKeys, {});
    } else if (key == "retain") {
      audit_map(it->second, "retain", kRetainKeys, {});
    } else if (key == "log") {
      audit_map(it->second, "log", kLogKeys, {});
    } else if (key == "monitoring") {
      audit_map(it->second, "monitoring", kMonitoringKeys, {});
    } else if (key == "persistence") {
      audit_map(it->second, "persistence", kPersistenceKeys, {});
    } else if (key == "auth") {
      audit_auth(it->second);
    } else if (key == "websocket") {
      warn_leaves(it->second, "websocket",
                  "已忽略：WebSocket 复用 server.port，本段不生效");
    } else if (key == "event_forwarding") {
      warn_leaves(it->second, "event_forwarding", "本版本不生效：事件转发未接线");
    } else {
      warn_leaves(it->second, key, "未识别，已忽略");
    }
  }
}

int ConfigManager::load_from_file(const std::string& config_file)
{
  warnings_.clear();
  last_error_.clear();
  try {
    YAML::Node root = YAML::LoadFile(config_file);
    audit_config(root);

    // 解析各个配置段
    if (root["server"]) {
      parse_server_config(root["server"]);
    }

    if (root["mqtt"]) {
      parse_mqtt_config(root["mqtt"]);
    }

    if (root["memory"]) {
      parse_memory_config(root["memory"]);
    }

    if (root["retain"]) {
      parse_retain_config(root["retain"]);
    }

    if (root["log"]) {
      parse_log_config(root["log"]);
    }

    if (root["event_forwarding"]) {
      parse_event_forwarding_config(root["event_forwarding"]);
    }

    if (root["monitoring"]) {
      parse_monitoring_config(root["monitoring"]);
    }

    if (root["auth"]) {
      parse_auth_config(root["auth"]);
    }

    if (root["persistence"]) {
      const auto p = root["persistence"];
      if (p["enabled"])
        config_.persistence.enabled = p["enabled"].as<bool>();
      if (p["path"])
        config_.persistence.path = p["path"].as<std::string>();
      if (p["partitions"])
        config_.persistence.partitions = p["partitions"].as<size_t>();
      if (p["segment_bytes"])
        config_.persistence.segment_bytes = p["segment_bytes"].as<size_t>();
      if (p["max_disk_bytes"])
        config_.persistence.max_disk_bytes = p["max_disk_bytes"].as<uint64_t>();
      if (p["checkpoint_interval_ms"])
        config_.persistence.checkpoint_interval_ms = p["checkpoint_interval_ms"].as<uint32_t>();
      if (p["max_session_expiry_seconds"])
        config_.persistence.max_session_expiry_seconds =
            p["max_session_expiry_seconds"].as<uint32_t>();
      if (p["max_sessions"])
        config_.persistence.max_sessions = p["max_sessions"].as<size_t>();
      if (p["max_subscriptions_per_session"])
        config_.persistence.max_subscriptions_per_session =
            p["max_subscriptions_per_session"].as<size_t>();
      if (p["max_messages"])
        config_.persistence.max_messages = p["max_messages"].as<size_t>();
      if (p["max_messages_per_session"])
        config_.persistence.max_messages_per_session = p["max_messages_per_session"].as<size_t>();
      if (p["max_bytes_per_session"])
        config_.persistence.max_bytes_per_session = p["max_bytes_per_session"].as<size_t>();
      if (p["overflow_policy"])
        config_.persistence.overflow_policy = p["overflow_policy"].as<std::string>();
      if (p["max_bytes"])
        config_.persistence.max_bytes = p["max_bytes"].as<size_t>();
      if (p["max_requests"])
        config_.persistence.max_requests = p["max_requests"].as<size_t>();
      if (p["max_request_bytes"])
        config_.persistence.max_request_bytes = p["max_request_bytes"].as<size_t>();
      if (p["max_inflight"])
        config_.persistence.max_inflight = p["max_inflight"].as<size_t>();
    }
    if (config_.retain.max_payload_bytes == 0) {
      config_.retain.max_payload_bytes = config_.mqtt.max_packet_size;
    }
    if (!last_error_.empty()) {
      LOG_ERROR("配置验证失败");
      return -1;
    }
    // 验证配置
    int ret = validate();
    if (ret != 0) {
      LOG_ERROR("配置验证失败");
      return ret;
    }

    LOG_INFO("配置文件加载成功: {}", config_file);
    return 0;

  } catch (const YAML::Exception& e) {
    LOG_ERROR("YAML解析错误: {}", e.what());
    return -1;
  } catch (const std::exception& e) {
    LOG_ERROR("配置文件加载失败: {}", e.what());
    return -1;
  }
}

int ConfigManager::validate()
{
  const auto& p = config_.persistence;
  if (p.enabled &&
      (p.path.empty() || !p.partitions || p.partitions > 32 || p.segment_bytes < 4096 ||
       p.max_disk_bytes / (p.partitions * 4) < p.segment_bytes ||
       p.checkpoint_interval_ms < 100 || !p.max_session_expiry_seconds || !p.max_sessions ||
       !p.max_subscriptions_per_session || !p.max_messages || !p.max_messages_per_session ||
       !p.max_bytes || !p.max_bytes_per_session ||
       (p.overflow_policy != "reject" && p.overflow_policy != "isolate") ||
       !p.max_requests || p.max_request_bytes < 4 * 1024 * 1024 ||
       !p.max_inflight || p.max_inflight > 65535)) {
    note_error("Invalid bounded persistence configuration");
    return -1;
  }
  // 验证服务器配置
  if (config_.server.port == 0 || config_.server.port > 65535) {
    note_error("无效的端口号: " + std::to_string(config_.server.port));
    return -1;
  }

  if (config_.server.max_connections <= 0) {
    note_error("无效的最大连接数: " + std::to_string(config_.server.max_connections));
    return -1;
  }

  if (config_.server.thread_count <= 0) {
    note_error("无效的线程数: " + std::to_string(config_.server.thread_count));
    return -1;
  }

  // 验证MQTT配置
  if (config_.mqtt.max_packet_size == 0) {
    note_error("最大包大小不能为0");
    return -1;
  }

  if (config_.mqtt.max_qos > 2) {
    note_error("无效的最大QoS级别: " + std::to_string(config_.mqtt.max_qos));
    return -1;
  }

  if (config_.mqtt.shared_subscription_available) {
    note_error("mqtt.shared_subscription_available 本版本不支持，不能为 true");
    return -1;
  }

  if (config_.mqtt.subscription_identifier_available) {
    note_error("mqtt.subscription_identifier_available 本版本不支持，不能为 true");
    return -1;
  }

  if (config_.mqtt.receive_maximum < 1) {
    note_error("mqtt.receive_maximum 必须大于等于 1");
    return -1;
  }

  // 读缓冲和解析出的报文各占一份完整报文，另留 128KiB 给连接状态（别名表、会话属性、遗嘱等）。
  constexpr uint64_t kConnectionStateHeadroom = 128 * 1024;
  const uint64_t required_client_memory =
      2 * static_cast<uint64_t>(config_.mqtt.max_packet_size) + kConnectionStateHeadroom;
  if (static_cast<uint64_t>(config_.memory.client_max_size) < required_client_memory) {
    note_error("memory.client_max_size (" + std::to_string(config_.memory.client_max_size) +
               ") 至少要为 2 × mqtt.max_packet_size + 128KiB (" +
               std::to_string(required_client_memory) + ")");
    return -1;
  }

  if (config_.mqtt.connect_timeout_seconds < 1) {
    note_error("mqtt.connect_timeout_seconds 必须大于等于 1");
    return -1;
  }

  // 验证内存配置
  if (config_.memory.client_max_size == 0) {
    note_error("客户端内存大小不能为0");
    return -1;
  }
  return 0;
}

void ConfigManager::parse_server_config(const YAML::Node& node)
{
  if (node["bind_address"]) {
    config_.server.bind_address = node["bind_address"].as<std::string>();
  }

  if (node["port"]) {
    config_.server.port = node["port"].as<uint16_t>();
  }

  if (node["max_connections"]) {
    config_.server.max_connections = node["max_connections"].as<int>();
  }

  if (node["backlog"]) {
    config_.server.backlog = node["backlog"].as<int>();
  }

  if (node["thread_count"]) {
    config_.server.thread_count = node["thread_count"].as<int>();
  }
}

void ConfigManager::parse_mqtt_config(const YAML::Node& node)
{
  if (node["max_packet_size"]) {
    const uint64_t value = node["max_packet_size"].as<uint64_t>();
    if (value == 0 || value > 0xffffffffu) {
      note_error("mqtt.max_packet_size 必须在 1 到 4294967295 之间");
      config_.mqtt.max_packet_size = 0;
    } else {
      config_.mqtt.max_packet_size = static_cast<uint32_t>(value);
    }
  }

  // keep_alive_default 已废弃，审计阶段已经 WARN，这里不再写入。

  if (node["keep_alive_max"]) {
    const uint64_t value = node["keep_alive_max"].as<uint64_t>();
    if (value > 65535) {
      note_error("mqtt.keep_alive_max 不得超过 65535");
    } else {
      config_.mqtt.keep_alive_max = static_cast<uint16_t>(value);
    }
  }

  if (node["connect_timeout_seconds"]) {
    config_.mqtt.connect_timeout_seconds = node["connect_timeout_seconds"].as<uint32_t>();
  }

  if (node["implicit_no_local"]) {
    config_.mqtt.implicit_no_local = node["implicit_no_local"].as<bool>();
  }

  if (node["max_qos"]) {
    config_.mqtt.max_qos = node["max_qos"].as<uint8_t>();
  }

  if (node["retain_available"]) {
    config_.mqtt.retain_available = node["retain_available"].as<bool>();
  }

  if (node["wildcard_subscription_available"]) {
    config_.mqtt.wildcard_subscription_available =
        node["wildcard_subscription_available"].as<bool>();
  }

  if (node["subscription_identifier_available"]) {
    config_.mqtt.subscription_identifier_available =
        node["subscription_identifier_available"].as<bool>();
  }

  if (node["shared_subscription_available"]) {
    config_.mqtt.shared_subscription_available = node["shared_subscription_available"].as<bool>();
  }

  if (node["topic_alias_maximum"]) {
    const uint64_t value = node["topic_alias_maximum"].as<uint64_t>();
    if (value > 65535) {
      note_error("mqtt.topic_alias_maximum 不得超过 65535");
    } else {
      config_.mqtt.topic_alias_maximum = static_cast<uint16_t>(value);
    }
  }

  if (node["receive_maximum"]) {
    const uint64_t value = node["receive_maximum"].as<uint64_t>();
    if (value < 1 || value > 65535) {
      note_error("mqtt.receive_maximum 必须在 1 到 65535 之间");
      config_.mqtt.receive_maximum = 0;
    } else {
      config_.mqtt.receive_maximum = static_cast<uint16_t>(value);
    }
  }

  if (node["allow_mqtt3x"]) {
    config_.mqtt.allow_mqtt3x = node["allow_mqtt3x"].as<bool>();
  }
}

void ConfigManager::parse_memory_config(const YAML::Node& node)
{
  if (node["client_max_size"]) {
    config_.memory.client_max_size = node["client_max_size"].as<size_t>();
  }
}

void ConfigManager::parse_retain_config(const YAML::Node& node)
{
  if (node["max_messages"]) {
    config_.retain.max_messages = node["max_messages"].as<size_t>();
  }
  if (node["max_payload_bytes"]) {
    config_.retain.max_payload_bytes = node["max_payload_bytes"].as<size_t>();
  }
  if (node["max_total_bytes"]) {
    config_.retain.max_total_bytes = node["max_total_bytes"].as<size_t>();
  }
  if (node["max_deliver_per_subscribe"]) {
    config_.retain.max_deliver_per_subscribe = node["max_deliver_per_subscribe"].as<size_t>();
  }
}

void ConfigManager::parse_log_config(const YAML::Node& node)
{
  if (node["level"]) {
    config_.log.level = node["level"].as<std::string>();
  }

  if (node["file_path"]) {
    config_.log.file_path = node["file_path"].as<std::string>();
  }

  if (node["max_file_size"]) {
    config_.log.max_file_size = node["max_file_size"].as<size_t>();
  }

  if (node["max_files"]) {
    config_.log.max_files = node["max_files"].as<int>();
  }

  if (node["flush_immediately"]) {
    config_.log.flush_immediately = node["flush_immediately"].as<bool>();
  }
}

void ConfigManager::parse_event_forwarding_config(const YAML::Node& node)
{
  if (node["enabled"]) {
    config_.event_forwarding.enabled = node["enabled"].as<bool>();
  }

  if (node["server_host"]) {
    config_.event_forwarding.server_host = node["server_host"].as<std::string>();
  }

  if (node["server_port"]) {
    config_.event_forwarding.server_port = node["server_port"].as<int>();
  }

  if (node["worker_thread_count"]) {
    config_.event_forwarding.worker_thread_count = node["worker_thread_count"].as<int>();
  }

  if (node["coroutines_per_thread"]) {
    config_.event_forwarding.coroutines_per_thread = node["coroutines_per_thread"].as<int>();
  }

  if (node["connection_timeout_ms"]) {
    config_.event_forwarding.connection_timeout_ms = node["connection_timeout_ms"].as<int>();
  }

  if (node["request_timeout_ms"]) {
    config_.event_forwarding.request_timeout_ms = node["request_timeout_ms"].as<int>();
  }

  if (node["max_batch_size"]) {
    config_.event_forwarding.max_batch_size = node["max_batch_size"].as<int>();
  }

  if (node["batch_timeout_ms"]) {
    config_.event_forwarding.batch_timeout_ms = node["batch_timeout_ms"].as<int>();
  }

  if (node["max_queue_size"]) {
    config_.event_forwarding.max_queue_size = node["max_queue_size"].as<size_t>();
  }

  if (node["queue_drop_policy"]) {
    config_.event_forwarding.queue_drop_policy = node["queue_drop_policy"].as<int>();
  }

  if (node["forward_login_events"]) {
    config_.event_forwarding.forward_login_events = node["forward_login_events"].as<bool>();
  }

  if (node["forward_logout_events"]) {
    config_.event_forwarding.forward_logout_events = node["forward_logout_events"].as<bool>();
  }

  if (node["forward_publish_events"]) {
    config_.event_forwarding.forward_publish_events = node["forward_publish_events"].as<bool>();
  }

  if (node["include_payload"]) {
    config_.event_forwarding.include_payload = node["include_payload"].as<bool>();
  }

  if (node["max_payload_size"]) {
    config_.event_forwarding.max_payload_size = node["max_payload_size"].as<size_t>();
  }
}

void ConfigManager::parse_monitoring_config(const YAML::Node& node)
{
  if (node["enabled"]) {
    config_.monitoring.enabled = node["enabled"].as<bool>();
  }

  if (node["interval_seconds"]) {
    config_.monitoring.interval_seconds = node["interval_seconds"].as<int>();
  }

  if (node["verbose_output"]) {
    config_.monitoring.verbose_output = node["verbose_output"].as<bool>();
  }

  if (node["json_output_file"]) {
    config_.monitoring.json_output_file = node["json_output_file"].as<std::string>();
  }
}

void ConfigManager::parse_auth_config(const YAML::Node& node)
{
  if (node["enabled"]) {
    config_.auth.enabled = node["enabled"].as<bool>();
  }

  if (node["allow_anonymous"]) {
    config_.auth.allow_anonymous = node["allow_anonymous"].as<bool>();
  }

  if (node["cache_enabled"]) {
    config_.auth.cache_enabled = node["cache_enabled"].as<bool>();
  }

  if (node["cache_ttl_seconds"]) {
    config_.auth.cache_ttl_seconds = node["cache_ttl_seconds"].as<int>();
  }

  // 解析认证提供者列表
  if (node["providers"]) {
    for (const auto& provider_node : node["providers"]) {
      AuthProviderConfig provider_config;
      
      if (provider_node["type"]) {
        provider_config.type = provider_node["type"].as<std::string>();
      }
      
      if (provider_node["priority"]) {
        provider_config.priority = provider_node["priority"].as<int>();
      }
      
      if (provider_node["enabled"]) {
        provider_config.enabled = provider_node["enabled"].as<bool>();
      }
      
      // 解析提供者特定设置
      if (provider_node["settings"]) {
        for (auto it = provider_node["settings"].begin(); it != provider_node["settings"].end(); ++it) {
          provider_config.settings[it->first.as<std::string>()] = it->second.as<std::string>();
        }
      }
      
      config_.auth.providers.push_back(provider_config);
    }
  }

  // 解析SQLite认证配置
  if (node["sqlite"]) {
    parse_sqlite_auth_config(node["sqlite"]);
  }

  // 解析Redis认证配置
  if (node["redis"]) {
    parse_redis_auth_config(node["redis"]);
  }
}

void ConfigManager::parse_sqlite_auth_config(const YAML::Node& node)
{
  if (node["db_path"]) {
    config_.auth.sqlite.db_path = node["db_path"].as<std::string>();
  }

  if (node["connection_pool_size"]) {
    config_.auth.sqlite.connection_pool_size = node["connection_pool_size"].as<int>();
  }

  if (node["max_retry_count"]) {
    config_.auth.sqlite.max_retry_count = node["max_retry_count"].as<int>();
  }

  if (node["retry_delay_ms"]) {
    config_.auth.sqlite.retry_delay_ms = node["retry_delay_ms"].as<int>();
  }

  if (node["query_timeout_ms"]) {
    config_.auth.sqlite.query_timeout_ms = node["query_timeout_ms"].as<int>();
  }

  if (node["enable_wal_mode"]) {
    config_.auth.sqlite.enable_wal_mode = node["enable_wal_mode"].as<bool>();
  }

  if (node["enable_foreign_keys"]) {
    config_.auth.sqlite.enable_foreign_keys = node["enable_foreign_keys"].as<bool>();
  }

  if (node["cache_size_kb"]) {
    config_.auth.sqlite.cache_size_kb = node["cache_size_kb"].as<int>();
  }
}

void ConfigManager::parse_redis_auth_config(const YAML::Node& node)
{
  if (node["host"]) {
    config_.auth.redis.host = node["host"].as<std::string>();
  }

  if (node["port"]) {
    config_.auth.redis.port = node["port"].as<int>();
  }

  if (node["password"]) {
    config_.auth.redis.password = node["password"].as<std::string>();
  }

  if (node["database"]) {
    config_.auth.redis.database = node["database"].as<int>();
  }

  if (node["connection_pool_size"]) {
    config_.auth.redis.connection_pool_size = node["connection_pool_size"].as<int>();
  }

  if (node["max_retry_count"]) {
    config_.auth.redis.max_retry_count = node["max_retry_count"].as<int>();
  }

  if (node["retry_delay_ms"]) {
    config_.auth.redis.retry_delay_ms = node["retry_delay_ms"].as<int>();
  }

  if (node["connect_timeout_ms"]) {
    config_.auth.redis.connect_timeout_ms = node["connect_timeout_ms"].as<int>();
  }

  if (node["command_timeout_ms"]) {
    config_.auth.redis.command_timeout_ms = node["command_timeout_ms"].as<int>();
  }

  if (node["keepalive_interval_s"]) {
    config_.auth.redis.keepalive_interval_s = node["keepalive_interval_s"].as<int>();
  }

  if (node["key_prefix"]) {
    config_.auth.redis.key_prefix = node["key_prefix"].as<std::string>();
  }

  if (node["cache_ttl_seconds"]) {
    config_.auth.redis.cache_ttl_seconds = node["cache_ttl_seconds"].as<int>();
  }
}

}  // namespace mqtt
