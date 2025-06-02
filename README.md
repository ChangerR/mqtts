# MQTTS

MQTTS 是一个基于 C++ 实现的 MQTT 协议解析器,支持 MQTT v5.0 协议。

## 功能特性

- 支持 MQTT v5.0 协议解析
- 支持 CONNECT、PUBLISH、SUBSCRIBE 等主要消息类型
- 自定义内存分配器,优化内存使用
- 详细的日志记录功能(基于spdlog)
- 单元测试覆盖主要功能

## 构建说明

### 依赖要求

- CMake 3.10+
- C++11 或更高版本
- spdlog 日志库

### 构建步骤

1. 克隆代码仓库