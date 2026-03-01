#include "logger.h"
#include <iostream>
#include <pthread.h>
#include "co_routine.h"
#include "mqtt_config.h"
#include "version.h"

namespace mqtt_log_ctx {
const char* const kDefaultTraceId = "-";

namespace {
pthread_key_t g_trace_id_key = 0;
pthread_once_t g_trace_id_once = PTHREAD_ONCE_INIT;
const std::string g_default_trace_id(kDefaultTraceId);

void make_trace_id_key()
{
  (void)pthread_key_create(&g_trace_id_key, NULL);
}

const std::string* get_trace_id_ptr()
{
  pthread_once(&g_trace_id_once, make_trace_id_key);
  return static_cast<const std::string*>(co_getspecific(g_trace_id_key));
}
}  // namespace

void ensure_trace_id()
{
  // Intentionally no-op:
  // trace id lifecycle is explicitly managed in MQTTServer::client_routine.
}

void bind_trace_id(const std::string& trace_id)
{
  pthread_once(&g_trace_id_once, make_trace_id_key);
  const std::string& non_empty_trace_id = trace_id.empty() ? g_default_trace_id : trace_id;
  (void)co_setspecific(g_trace_id_key, &non_empty_trace_id);
}

void clear_trace_id()
{
  pthread_once(&g_trace_id_once, make_trace_id_key);
  (void)co_setspecific(g_trace_id_key, NULL);
}

const std::string& current_trace_id()
{
  const std::string* trace_id = get_trace_id_ptr();
  if (trace_id == NULL || trace_id->empty()) {
    return g_default_trace_id;
  }
  return *trace_id;
}
}  // namespace mqtt_log_ctx

namespace {
class TraceIdFlagFormatter final : public spdlog::custom_flag_formatter
{
 public:
  void format(const spdlog::details::log_msg&, const std::tm&,
              spdlog::memory_buf_t& dest) override
  {
    const std::string& trace_id = mqtt_log_ctx::current_trace_id();
    dest.append(trace_id.data(), trace_id.data() + trace_id.size());
  }

  std::unique_ptr<spdlog::custom_flag_formatter> clone() const override
  {
    return std::unique_ptr<spdlog::custom_flag_formatter>(new TraceIdFlagFormatter());
  }
};

std::unique_ptr<spdlog::formatter> make_formatter(const std::string& pattern)
{
  std::unique_ptr<spdlog::pattern_formatter> formatter(new spdlog::pattern_formatter());
  formatter->add_flag<TraceIdFlagFormatter>('*');
  formatter->set_pattern(pattern);
  return std::unique_ptr<spdlog::formatter>(formatter.release());
}
}  // namespace

Logger::Logger()
{
  log_ = spdlog::stdout_color_mt("console");
  log_->set_level(spdlog::level::trace);
  log_->set_formatter(make_formatter("%^[%D %T.%e] [%t] [%l] [%@,%!] [%*] %v%$"));
}

Logger::~Logger()
{
  log_ = nullptr;
}

int Logger::configure(const mqtt::LogConfig& config)
{
  int __mq_ret = 0;
  do {
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
      console_sink->set_formatter(make_formatter("%^[%D %T.%e] [%t] [%l] [%@,%!] [%*] %v%$"));
      sinks.push_back(console_sink);
  
      // 如果配置了文件路径，添加文件sink
      if (!config.file_path.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config.file_path, config.max_file_size, config.max_files);
        file_sink->set_formatter(make_formatter("[%D %T.%e] [%t] [%l] [%@,%!] [%*] %v"));
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
  
      // 打印版本信息
      LOG_INFO("MQTTS启动\n{}", get_version_string());
  
      std::string file_info;
      if (!config.file_path.empty()) {
        file_info = fmt::format(", 文件={}, 最大大小={}MB, 最大文件数={}", config.file_path,
                                config.max_file_size / (1024 * 1024), config.max_files);
      } else {
        file_info = ", 文件输出=否";
      }
  
      LOG_INFO("日志系统配置完成: 级别={}, 控制台输出=是{}, 立即刷新={}", config.level, file_info,
               config.flush_immediately ? "是" : "否");
  
      __mq_ret = 0;
      break;
    } catch (const std::exception& e) {
      std::cerr << "配置日志系统失败: " << e.what() << std::endl;
      __mq_ret = -1;
      break;
    }
  } while (false);

  return __mq_ret;
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
