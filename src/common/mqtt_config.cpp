#include "mqtt_config.h"
#include <yaml-cpp/yaml.h>
#include "logger.h"

namespace mqtt {

ConfigManager::ConfigManager() = default;

ConfigManager::~ConfigManager() = default;

int ConfigManager::load_from_file(const std::string& config_file)
{
  int __mq_ret = 0;
  do {
    try {
      YAML::Node root = YAML::LoadFile(config_file);
  
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
  
      if (root["websocket"]) {
        LOG_WARN("检测到 websocket 配置段。当前版本使用 server 监听端口处理 WebSocket，"
                 "请将宿主机 WebSocket 端口转发到容器 server.port。");
      }
  
      // 验证配置
      int ret = validate();
      if (ret != 0) {
        LOG_ERROR("配置验证失败");
        __mq_ret = ret;
        break;
      }
  
      LOG_INFO("配置文件加载成功: {}", config_file);
      __mq_ret = 0;
      break;
  
    } catch (const YAML::Exception& e) {
      LOG_ERROR("YAML解析错误: {}", e.what());
      __mq_ret = -1;
      break;
    } catch (const std::exception& e) {
      LOG_ERROR("配置文件加载失败: {}", e.what());
      __mq_ret = -1;
      break;
    }
  } while (false);

  return __mq_ret;
}

int ConfigManager::validate() const
{
  int __mq_ret = 0;
  do {
    // 验证服务器配置
    if (config_.server.port == 0 || config_.server.port > 65535) {
      LOG_ERROR("无效的端口号: {}", config_.server.port);
      __mq_ret = -1;
      break;
    }
  
    if (config_.server.max_connections <= 0) {
      LOG_ERROR("无效的最大连接数: {}", config_.server.max_connections);
      __mq_ret = -1;
      break;
    }
  
    if (config_.server.thread_count <= 0) {
      LOG_ERROR("无效的线程数: {}", config_.server.thread_count);
      __mq_ret = -1;
      break;
    }
  
    // 验证MQTT配置
    if (config_.mqtt.max_packet_size == 0) {
      LOG_ERROR("最大包大小不能为0");
      __mq_ret = -1;
      break;
    }
  
    if (config_.mqtt.max_qos > 2) {
      LOG_ERROR("无效的最大QoS级别: {}", config_.mqtt.max_qos);
      __mq_ret = -1;
      break;
    }
  
    // 验证内存配置
    if (config_.memory.client_max_size == 0) {
      LOG_ERROR("客户端内存大小不能为0");
      __mq_ret = -1;
      break;
    }
    __mq_ret = 0;
    break;
  } while (false);

  return __mq_ret;
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
    config_.mqtt.max_packet_size = node["max_packet_size"].as<uint32_t>();
  }

  if (node["keep_alive_default"]) {
    config_.mqtt.keep_alive_default = node["keep_alive_default"].as<uint16_t>();
  }

  if (node["keep_alive_max"]) {
    config_.mqtt.keep_alive_max = node["keep_alive_max"].as<uint16_t>();
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
    config_.mqtt.topic_alias_maximum = node["topic_alias_maximum"].as<uint16_t>();
  }

  if (node["receive_maximum"]) {
    config_.mqtt.receive_maximum = node["receive_maximum"].as<uint16_t>();
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
