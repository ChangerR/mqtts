#pragma once
#include <cstdio>
#include <iostream>
#include <sstream>

// 简化的日志宏，直接输出到stderr
#define LOG_TRACE(...)
#define LOG_DEBUG(...)
#define LOG_INFO(...)             \
  do {                            \
    fprintf(stderr, "[INFO] ");   \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n");        \
  } while (0)
#define LOG_WARN(...)             \
  do {                            \
    fprintf(stderr, "[WARN] ");   \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n");        \
  } while (0)
#define LOG_ERROR(...)            \
  do {                            \
    fprintf(stderr, "[ERROR] ");  \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n");        \
  } while (0)
#define LOG_CRITICAL(...)           \
  do {                              \
    fprintf(stderr, "[CRITICAL] "); \
    fprintf(stderr, __VA_ARGS__);   \
    fprintf(stderr, "\n");          \
  } while (0)

#define LOG_HEXDUMP(data, len)                                                              \
  do {                                                                                      \
    fprintf(stderr, "[HEXDUMP]\n");                                                         \
    for (size_t i = 0; i < len; i += 16) {                                                  \
      fprintf(stderr, "%06zx: ", i);                                                        \
      for (size_t j = 0; j < 16 && (i + j) < len; j++) {                                    \
        fprintf(stderr, "%02x ", static_cast<uint8_t>(data[i + j]));                        \
      }                                                                                     \
      fprintf(stderr, " |");                                                                \
      for (size_t j = 0; j < 16 && (i + j) < len; j++) {                                    \
        uint8_t byte = static_cast<uint8_t>(data[i + j]);                                   \
        fprintf(stderr, "%c", (byte >= 32 && byte <= 126) ? static_cast<char>(byte) : '.'); \
      }                                                                                     \
      fprintf(stderr, "|\n");                                                               \
    }                                                                                       \
  } while (0)