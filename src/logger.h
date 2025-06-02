#pragma once
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include "singleton.h"

class Logger
{
 public:
  Logger()
  {
    log_ = spdlog::stdout_color_mt("console");
    log_->set_level(spdlog::level::trace);
    log_->set_pattern("%^[%D %T.%e] [%t] [%l] [%@,%!] %v%$");
  }

  ~Logger() { log_ = nullptr; }

 public:
  std::shared_ptr<spdlog::logger> log_;
};

typedef Singleton<Logger> SingletonLogger;
#define LOG (SingletonLogger::instance().log_)
#define LOG_TRACE(...) SPDLOG_LOGGER_TRACE(LOG, __VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(LOG, __VA_ARGS__)
#define LOG_INFO(...) SPDLOG_LOGGER_INFO(LOG, __VA_ARGS__)
#define LOG_WARN(...) SPDLOG_LOGGER_WARN(LOG, __VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_LOGGER_ERROR(LOG, __VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(LOG, __VA_ARGS__)

#define LOG_HEXDUMP(data, len)                                                         \
  do {                                                                                 \
    std::string output;                                                                \
    output.reserve(len * 4 + (len / 16 + 1) * 80);                                     \
    for (size_t i = 0; i < len; i += 16) {                                             \
      char line[80];                                                                   \
      std::string hex;                                                                 \
      std::string ascii;                                                               \
      hex.reserve(16 * 3);                                                             \
      ascii.reserve(16);                                                               \
      for (size_t j = 0; j < 16 && (i + j) < len; j++) {                               \
        uint8_t byte = static_cast<uint8_t>(data[i + j]);                              \
        char hex_byte[4];                                                              \
        snprintf(hex_byte, sizeof(hex_byte), "%02x ", byte);                           \
        hex += hex_byte;                                                               \
        ascii += (byte >= 32 && byte <= 126) ? static_cast<char>(byte) : '.';          \
      }                                                                                \
      if (hex.length() < 16 * 3) {                                                     \
        hex.append(16 * 3 - hex.length(), ' ');                                        \
      }                                                                                \
      snprintf(line, sizeof(line), "%06zx: %s |%s|\n", i, hex.c_str(), ascii.c_str()); \
      output += line;                                                                  \
    }                                                                                  \
    LOG_INFO("\n{}", output);                                                          \
  } while (0)
