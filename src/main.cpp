#include <unistd.h>
#include <fstream>
#include <iostream>
#include <thread>
#include "logger.h"
#include "mqtt_config.h"
#include "mqtt_server.h"
#include "version.h"

void run_mqtt_server(const mqtt::Config& config)
{
  std::vector<std::thread> threads;

  MQTTServer server(config.server, config.memory);

  LOG_INFO("Starting MQTT server on {}:{}", config.server.bind_address, config.server.port);

  int ret = server.start();
  if (ret != MQ_SUCCESS) {
    LOG_ERROR("Failed to start MQTT server, error code: {}", ret);
    return;
  }

  if (config.server.thread_count > 1) {
    for (int i = 0; i < config.server.thread_count; i++) {
      threads.push_back(std::thread([&] {
        LOG_INFO("MQTT server thread started");
        server.run();
        LOG_INFO("MQTT server thread stopped");
      }));
    }
  } else {
    server.run();
  }

  for (auto& thread : threads) {
    thread.join();
  }
  server.stop();

  LOG_INFO("MQTT server thread stopped");
}

bool file_exists(const std::string& filename)
{
  std::ifstream file(filename);
  return file.good();
}

void print_usage(const char* program)
{
  std::cout << "用法: " << program << " [选项] <配置文件>\n"
            << "选项:\n"
            << "  -h, --help     显示帮助信息\n"
            << "  -v, --version  显示版本信息\n";
}

int main(int argc, char* argv[])
{
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  // 解析命令行参数
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "-v" || arg == "--version") {
      std::cout << get_version_string() << std::endl;
      return 0;
    }
  }

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

  // 配置日志系统
  int log_ret = SingletonLogger::instance().configure(config.log);
  if (log_ret != 0) {
    std::cerr << "日志系统配置失败" << std::endl;
    return 1;
  }

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
  LOG_INFO("  线程数: {}", config.server.thread_count);
  LOG_INFO("  最大包大小: {} bytes", config.mqtt.max_packet_size);
  LOG_INFO("  保活时间: {} seconds", config.mqtt.keep_alive_default);
  LOG_INFO("  客户端内存大小: {} MB", config.memory.client_max_size / (1024 * 1024));

  run_mqtt_server(config);

  LOG_INFO("MQTT服务器已停止");
  return 0;
}