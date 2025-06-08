#include <unistd.h>
#include <iostream>
#include <thread>
#include <fstream>
#include "logger.h"
#include "mqtt_server.h"
#include "mqtt_config.h"

static MQTTServer* g_server = nullptr;
static std::thread g_server_thread;

void server_thread_func(const mqtt::Config& config)
{
  g_server = new MQTTServer(config.server.bind_address.c_str(), config.server.port);
  if (!g_server) {
    LOG_ERROR("Failed to create MQTT server");
    return;
  }

  LOG_INFO("Starting MQTT server on {}:{}", config.server.bind_address, config.server.port);

  int ret = g_server->start();
  if (ret != MQ_SUCCESS) {
    LOG_ERROR("Failed to start MQTT server, error code: {}", ret);
    delete g_server;
    g_server = nullptr;
    return;
  }

  // Start running the server
  g_server->run();

  // Cleanup
  if (g_server) {
    g_server->stop();
    delete g_server;
    g_server = nullptr;
  }

  LOG_INFO("MQTT server thread stopped");
}

bool file_exists(const std::string& filename) {
  std::ifstream file(filename);
  return file.good();
}

void print_usage(const char* program_name) {
  std::cout << "用法: " << program_name << " [选项]\n";
  std::cout << "选项:\n";
  std::cout << "  -c <config_file>  指定配置文件路径 (默认: mqtts.yaml)\n";
  std::cout << "  -i <bind_address> 绑定地址 (覆盖配置文件)\n";
  std::cout << "  -p <port>         监听端口 (覆盖配置文件)\n";
  std::cout << "  -h                显示此帮助信息\n";
  std::cout << "\n示例:\n";
  std::cout << "  " << program_name << " -c /etc/mqtts.yaml\n";
  std::cout << "  " << program_name << " -i 127.0.0.1 -p 8883\n";
}

int main(int argc, char* argv[])
{
  // 默认配置
  std::string config_file = "mqtts.yaml";
  std::string override_ip;
  int override_port = -1;

  // 解析命令行参数
  int opt;
  while ((opt = getopt(argc, argv, "c:i:p:h")) != -1) {
    switch (opt) {
      case 'c':
        config_file = optarg;
        break;
      case 'i':
        override_ip = optarg;
        break;
      case 'p':
        override_port = atoi(optarg);
        break;
      case 'h':
        print_usage(argv[0]);
        return 0;
      default:
        print_usage(argv[0]);
        return 1;
    }
  }

  // 加载配置文件
  mqtt::ConfigManager config_manager;
  
  // 检查配置文件是否存在
  if (!file_exists(config_file)) {
    LOG_ERROR("配置文件不存在: {}", config_file);
    LOG_ERROR("请创建配置文件或使用命令行参数指定服务器参数");
    return 1;
  }

  // 加载配置文件
  int ret = config_manager.load_from_file(config_file);
  if (ret != 0) {
    LOG_ERROR("加载配置文件失败: {}", config_file);
    return 1;
  }

  // 获取配置
  mqtt::Config config = config_manager.get_config();
  
  // 应用命令行覆盖
  if (!override_ip.empty()) {
    config.server.bind_address = override_ip;
    LOG_INFO("使用命令行覆盖绑定地址: {}", override_ip);
  }
  
  if (override_port > 0) {
    config.server.port = static_cast<uint16_t>(override_port);
    LOG_INFO("使用命令行覆盖监听端口: {}", override_port);
  }

  // 显示当前配置
  LOG_INFO("配置加载完成:");
  LOG_INFO("  服务器地址: {}", config.server.bind_address);
  LOG_INFO("  监听端口: {}", config.server.port);
  LOG_INFO("  最大连接数: {}", config.server.max_connections);
  LOG_INFO("  最大包大小: {} bytes", config.mqtt.max_packet_size);
  LOG_INFO("  保活时间: {} seconds", config.mqtt.keep_alive_default);
  LOG_INFO("  内存池大小: {} MB", config.memory.initial_pool_size / (1024 * 1024));

  // 在独立线程中启动服务器
  g_server_thread = std::thread(server_thread_func, std::cref(config));

  // 等待服务器线程结束
  g_server_thread.join();

  LOG_INFO("MQTT服务器已停止");
  return 0;
}