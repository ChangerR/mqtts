#include "mqtt_config.h"
#include "logger.h"
#include <yaml-cpp/yaml.h>

namespace mqtt {

ConfigManager::ConfigManager() = default;

ConfigManager::~ConfigManager() = default;

int ConfigManager::load_from_file(const std::string& config_file) {
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

int ConfigManager::validate() const {
  // 验证服务器配置
  if (config_.server.port == 0 || config_.server.port > 65535) {
    LOG_ERROR("无效的端口号: {}", config_.server.port);
    return -1;
  }
  
  if (config_.server.max_connections <= 0) {
    LOG_ERROR("无效的最大连接数: {}", config_.server.max_connections);
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
  if (config_.memory.initial_pool_size > config_.memory.max_pool_size) {
    LOG_ERROR("初始内存池大小不能大于最大内存池大小");
    return -1;
  }
  
  if (config_.memory.block_size == 0) {
    LOG_ERROR("内存块大小不能为0");
    return -1;
  }
  
  return 0;
}

void ConfigManager::parse_server_config(const YAML::Node& node) {
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
}

void ConfigManager::parse_mqtt_config(const YAML::Node& node) {
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
    config_.mqtt.wildcard_subscription_available = node["wildcard_subscription_available"].as<bool>();
  }
  
  if (node["subscription_identifier_available"]) {
    config_.mqtt.subscription_identifier_available = node["subscription_identifier_available"].as<bool>();
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

void ConfigManager::parse_memory_config(const YAML::Node& node) {
  if (node["initial_pool_size"]) {
    config_.memory.initial_pool_size = node["initial_pool_size"].as<size_t>();
  }
  
  if (node["max_pool_size"]) {
    config_.memory.max_pool_size = node["max_pool_size"].as<size_t>();
  }
  
  if (node["block_size"]) {
    config_.memory.block_size = node["block_size"].as<size_t>();
  }
  
  if (node["use_tcmalloc"]) {
    config_.memory.use_tcmalloc = node["use_tcmalloc"].as<bool>();
  }
  
  if (node["buffer_initial_size"]) {
    config_.memory.buffer_initial_size = node["buffer_initial_size"].as<size_t>();
  }
  
  if (node["buffer_max_size"]) {
    config_.memory.buffer_max_size = node["buffer_max_size"].as<size_t>();
  }
}

void ConfigManager::parse_log_config(const YAML::Node& node) {
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

}  // namespace mqtt 