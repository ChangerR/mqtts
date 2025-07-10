#include "mqtt_config.h"
#include <yaml-cpp/yaml.h>
#include "logger.h"

namespace mqtt {

ConfigManager::ConfigManager() = default;

ConfigManager::~ConfigManager() = default;

int ConfigManager::load_from_file(const std::string& config_file)
{
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

int ConfigManager::validate() const
{
  // 验证服务器配置
  if (config_.server.port == 0 || config_.server.port > 65535) {
    LOG_ERROR("无效的端口号: {}", config_.server.port);
    return -1;
  }

  if (config_.server.max_connections <= 0) {
    LOG_ERROR("无效的最大连接数: {}", config_.server.max_connections);
    return -1;
  }

  if (config_.server.thread_count <= 0) {
    LOG_ERROR("无效的线程数: {}", config_.server.thread_count);
    return -1;
  }

  // 验证MQTT配置
  if (config_.mqtt.max_packet_size == 0) {
    LOG_ERROR("最大包大小不能为0");
    return -1;
  }

  if (config_.mqtt.max_qos > 2) {
    LOG_ERROR("无效的最大QoS级别: {}", config_.mqtt.max_qos);
    return -1;
  }

  // 验证内存配置
  if (config_.memory.client_max_size == 0) {
    LOG_ERROR("客户端内存大小不能为0");
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

}  // namespace mqtt