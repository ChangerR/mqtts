#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

#include <cstdint>
#include <string>

// 前向声明
namespace YAML {
class Node;
}

namespace mqtt {

/**
 * @brief 服务器配置
 */
struct ServerConfig
{
  std::string bind_address = "0.0.0.0";  // 绑定地址
  uint16_t port = 1883;                  // 监听端口
  int max_connections = 1000;            // 最大连接数
  int backlog = 128;                     // socket backlog
  int thread_count = 1;                  // 线程数
};

/**
 * @brief MQTT协议配置
 */
struct MQTTProtocolConfig
{
  uint32_t max_packet_size = 1024 * 1024;         // 最大包大小 (1MB)
  uint16_t keep_alive_default = 60;               // 默认保活时间(秒)
  uint16_t keep_alive_max = 3600;                 // 最大保活时间(秒)
  uint8_t max_qos = 2;                            // 最大QoS级别
  bool retain_available = true;                   // 是否支持retain消息
  bool wildcard_subscription_available = true;    // 是否支持通配符订阅
  bool subscription_identifier_available = true;  // 是否支持订阅标识符
  bool shared_subscription_available = false;     // 是否支持共享订阅
  uint16_t topic_alias_maximum = 65535;           // 主题别名最大值
  uint16_t receive_maximum = 65535;               // 接收最大值
};

/**
 * @brief 内存配置
 */
struct MemoryConfig
{
  size_t client_max_size = 1048576;
};

/**
 * @brief 日志配置
 */
struct LogConfig
{
  std::string level = "info";  // 日志级别: trace, debug, info, warn, error, critical
  std::string file_path = "";  // 日志文件路径，空字符串表示输出到控制台
  size_t max_file_size = 100 * 1024 * 1024;  // 日志文件最大大小 (100MB)
  int max_files = 10;                        // 最大日志文件数量
  bool flush_immediately = false;            // 是否立即刷新
};

/**
 * @brief 事件转发配置
 */
struct EventForwardingConfig
{
  bool enabled = false;                    // 是否启用事件转发
  std::string server_host = "";            // 目标服务器地址
  int server_port = 0;                     // 目标服务器端口
  int worker_thread_count = 2;             // 工作线程数量
  int coroutines_per_thread = 4;           // 每个线程的协程数量
  int connection_timeout_ms = 5000;        // 连接超时时间（毫秒）
  int request_timeout_ms = 10000;          // 请求超时时间（毫秒）
  int max_batch_size = 100;                // 最大批次大小
  int batch_timeout_ms = 1000;             // 批次超时时间（毫秒）
  size_t max_queue_size = 10000;           // 最大队列大小
  int queue_drop_policy = 0;               // 队列丢弃策略：0=丢弃最旧，1=丢弃最新
  bool forward_login_events = true;        // 是否转发登录事件
  bool forward_logout_events = true;       // 是否转发登出事件
  bool forward_publish_events = true;      // 是否转发发布事件
  bool include_payload = false;            // 是否包含载荷数据
  size_t max_payload_size = 1024;          // 最大载荷大小（超过则不包含）
};

/**
 * @brief 主配置结构
 */
struct Config
{
  ServerConfig server;
  MQTTProtocolConfig mqtt;
  MemoryConfig memory;
  LogConfig log;
  EventForwardingConfig event_forwarding;
};

/**
 * @brief 配置管理器
 */
class ConfigManager
{
 public:
  ConfigManager();
  ~ConfigManager();

  // 禁止拷贝构造和赋值
  ConfigManager(const ConfigManager&) = delete;
  ConfigManager& operator=(const ConfigManager&) = delete;

  /**
   * @brief 从YAML文件加载配置
   * @param config_file 配置文件路径
   * @return 0成功，非0失败
   */
  int load_from_file(const std::string& config_file);

  /**
   * @brief 验证配置有效性
   * @return 0有效，非0无效
   */
  int validate() const;

  /**
   * @brief 获取配置
   * @return 配置引用
   */
  const Config& get_config() const { return config_; }

  /**
   * @brief 获取可修改的配置
   * @return 配置引用
   */
  Config& get_mutable_config() { return config_; }

 private:
  Config config_;

  // 解析辅助方法
  void parse_server_config(const YAML::Node& node);
  void parse_mqtt_config(const YAML::Node& node);
  void parse_memory_config(const YAML::Node& node);
  void parse_log_config(const YAML::Node& node);
  void parse_event_forwarding_config(const YAML::Node& node);
};

}  // namespace mqtt

#endif  // MQTT_CONFIG_H