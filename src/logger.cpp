#include "logger.h"
#include <iostream>
#include "mqtt_config.h"

int Logger::configure(const mqtt::LogConfig& config)
{
  try {
    // 设置日志级别
    spdlog::level::level_enum level = string_to_level(config.level);

    // 重置现有的logger
    spdlog::drop("console");
    log_ = nullptr;

    // 创建sinks向量
    std::vector<spdlog::sink_ptr> sinks;

    // 始终添加控制台sink
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("%^[%D %T.%e] [%t] [%l] [%@,%!] %v%$");
    sinks.push_back(console_sink);

    // 如果配置了文件路径，添加文件sink
    if (!config.file_path.empty()) {
      auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          config.file_path, config.max_file_size, config.max_files);
      file_sink->set_pattern("[%D %T.%e] [%t] [%l] [%@,%!] %v");
      sinks.push_back(file_sink);
    }

    // 创建多sink logger
    log_ = std::make_shared<spdlog::logger>("multi_sink", sinks.begin(), sinks.end());
    log_->set_level(level);

    // 设置刷新策略
    if (config.flush_immediately) {
      log_->flush_on(spdlog::level::trace);  // 每条日志都刷新
    } else {
      log_->flush_on(spdlog::level::warn);  // 警告及以上级别才刷新
    }

    // 注册logger
    spdlog::register_logger(log_);

    // 设置为默认logger
    spdlog::set_default_logger(log_);

    std::cout << "日志系统配置完成: 级别=" << config.level;
    std::cout << ", 控制台输出=是";
    if (!config.file_path.empty()) {
      std::cout << ", 文件=" << config.file_path
                << ", 最大大小=" << config.max_file_size / (1024 * 1024) << "MB"
                << ", 最大文件数=" << config.max_files;
    } else {
      std::cout << ", 文件输出=否";
    }
    std::cout << ", 立即刷新=" << (config.flush_immediately ? "是" : "否") << std::endl;

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "配置日志系统失败: " << e.what() << std::endl;
    return -1;
  }
}

spdlog::level::level_enum Logger::string_to_level(const std::string& level_str)
{
  if (level_str == "trace")
    return spdlog::level::trace;
  if (level_str == "debug")
    return spdlog::level::debug;
  if (level_str == "info")
    return spdlog::level::info;
  if (level_str == "warn" || level_str == "warning")
    return spdlog::level::warn;
  if (level_str == "error")
    return spdlog::level::err;
  if (level_str == "critical")
    return spdlog::level::critical;
  if (level_str == "off")
    return spdlog::level::off;

  // 默认返回info级别
  std::cerr << "未知的日志级别: " << level_str << "，使用默认级别info" << std::endl;
  return spdlog::level::info;
}