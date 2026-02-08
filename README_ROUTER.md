# MQTT分布式路由系统

## 概述

这是一个高性能的MQTT分布式路由系统，支持多个MQTT服务器实例之间的消息路由和订阅管理。系统采用独立的路由进程设计，提供全局的主题订阅管理和消息路由功能。

## 架构特性

### 🚀 核心功能
- **分布式路由**：支持多个MQTT服务器实例的消息路由
- **全局订阅管理**：统一管理所有服务器的主题订阅
- **高性能设计**：基于协程的异步处理，支持高并发
- **数据持久化**：使用redo log + snapshot机制保证数据安全
- **故障恢复**：支持路由服务故障恢复和数据重建

### 🏗️ 系统架构
```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   MQTT Server   │    │   MQTT Server   │    │   MQTT Server   │
│    Instance 1   │    │    Instance 2   │    │    Instance N   │
│                 │    │                 │    │                 │
│  ┌───────────┐  │    │  ┌───────────┐  │    │  ┌───────────┐  │
│  │ RPC Client│◄─┼────┼─►│ RPC Client│◄─┼────┼─►│ RPC Client│  │
│  └───────────┘  │    │  └───────────┘  │    │  └───────────┘  │
└─────────────────┘    └─────────────────┘    └─────────────────┘
         │                       │                       │
         └───────────────────────┼───────────────────────┘
                                 │
                    ┌─────────────────────────┐
                    │    MQTT Router Service   │
                    │                         │
                    │  ┌─────────────────┐    │
                    │  │ Global Topic    │    │
                    │  │     Tree        │    │
                    │  └─────────────────┘    │
                    │                         │
                    │  ┌─────────────────┐    │
                    │  │  Redo Log +     │    │
                    │  │   Snapshot      │    │
                    │  └─────────────────┘    │
                    └─────────────────────────┘
```

### ⚡ 性能特性
- **无锁设计**：主题树使用写时复制（Copy-on-Write）技术
- **内存优化**：自定义内存分配器，减少内存碎片
- **协程并发**：基于libco的高性能协程支持
- **批量处理**：支持批量订阅和消息转发操作

## 快速开始

### 1. 编译系统

```bash
# 进入项目目录
cd /path/to/mqtts

# 创建构建目录
mkdir -p build && cd build

# 配置和编译
cmake .. && make -j$(nproc)

# 或者使用便捷脚本
./scripts/start-router.sh build
```

### 2. 启动路由服务

```bash
# 启动路由服务（前台运行）
./scripts/start-router.sh start

# 或者后台运行
./scripts/start-router.sh start -d

# 使用自定义配置
./scripts/start-router.sh start -c custom_router.yaml -d
```

### 3. 启动MQTT服务器实例

```bash
# 启动第一个MQTT服务器实例
./bin/mqtts mqtts_with_router.yaml

# 启动第二个实例（不同端口）
./bin/mqtts mqtts_instance2.yaml
```

### 4. 验证系统运行

```bash
# 检查路由服务状态
./scripts/start-router.sh status

# 查看路由服务日志
./scripts/start-router.sh logs

# 检查MQTT服务器日志
tail -f logs/mqtts.log
```

## 配置说明

### 路由服务配置 (mqtt_router.yaml)

```yaml
router:
  # 服务配置
  service_host: "127.0.0.1"
  service_port: 9090
  
  # 持久化配置
  redo_log_path: "./data/mqtt_router.redo"
  snapshot_path: "./data/mqtt_router.snapshot"
  snapshot_interval_seconds: 300
  redo_log_flush_interval_ms: 1000
  max_redo_log_entries: 10000
  
  # 性能配置
  worker_thread_count: 4
  coroutines_per_thread: 8
  max_memory_limit: 1073741824  # 1GB
```

### MQTT服务器配置 (mqtts_with_router.yaml)

```yaml
server:
  server_id: "mqtt_server_1"  # 每个实例需要唯一ID
  bind_address: "0.0.0.0"
  port: 1883

router:
  enabled: true               # 启用路由功能
  host: "127.0.0.1"          # 路由服务地址
  port: 9090                 # 路由服务端口
  auto_register: true        # 自动注册到路由器
```

## 管理命令

### 路由服务管理

```bash
# 启动服务
./scripts/start-router.sh start [-c config] [-d]

# 停止服务
./scripts/start-router.sh stop

# 重启服务
./scripts/start-router.sh restart

# 查看状态
./scripts/start-router.sh status

# 查看日志
./scripts/start-router.sh logs

# 清理文件
./scripts/start-router.sh clean
```

### 服务监控

```bash
# 查看路由服务进程
ps aux | grep mqtt_router

# 查看网络连接
netstat -tlnp | grep 9090

# 查看实时日志
tail -f /tmp/mqtt_router.log

# 监控系统资源
top -p $(cat /tmp/mqtt_router.pid)
```

## 功能详解

### 1. 分布式订阅管理

当客户端在任意MQTT服务器实例上订阅主题时：

1. **本地订阅**：在本地主题树中记录订阅
2. **路由注册**：通过RPC客户端向路由服务注册订阅
3. **全局同步**：路由服务在全局主题树中记录订阅
4. **持久化**：订阅信息写入redo log并定期创建snapshot

### 2. 消息路由转发

当客户端发布消息时：

1. **本地转发**：在本地查找订阅者并转发
2. **路由查询**：向路由服务查询远程订阅者
3. **远程转发**：将消息转发到有订阅者的远程服务器
4. **避免重复**：防止消息在服务器之间循环转发

### 3. 数据持久化机制

#### Redo Log
- 记录所有订阅/取消订阅操作
- 支持故障恢复时的操作重放
- 定期刷新到磁盘（可配置间隔）

#### Snapshot
- 定期保存完整的主题树状态
- 减少系统重启时的恢复时间
- 支持增量更新和压缩

### 4. 故障恢复

#### 路由服务故障恢复
1. 从最近的snapshot加载基础状态
2. 重放snapshot之后的redo log操作
3. 重建完整的全局主题树

#### MQTT服务器故障恢复
1. 重连到路由服务
2. 重新注册本地订阅信息
3. 恢复正常的消息路由功能

## 性能调优

### 1. 内存优化

```yaml
# 路由服务内存配置
router:
  max_memory_limit: 2147483648  # 根据预期负载调整

# MQTT服务器内存配置
memory:
  max_global_memory: 2147483648
  max_per_client_memory: 10485760
```

### 2. 并发优化

```yaml
# 路由服务并发配置
router:
  worker_thread_count: 8        # 根据CPU核心数调整
  coroutines_per_thread: 16     # 根据并发连接数调整

# MQTT服务器并发配置
server:
  worker_threads: 8
  max_connections: 10000
```

### 3. 持久化优化

```yaml
# 调整持久化频率
router:
  snapshot_interval_seconds: 600    # 减少快照频率
  redo_log_flush_interval_ms: 2000  # 增加批量写入
  max_redo_log_entries: 20000       # 增加内存缓存
```

## 故障排除

### 1. 路由服务无法启动

检查项：
- [ ] 端口是否被占用：`netstat -tlnp | grep 9090`
- [ ] 配置文件是否正确：验证YAML语法
- [ ] 数据目录是否可写：检查./data目录权限
- [ ] 内存是否充足：检查系统可用内存

```bash
# 查看详细错误信息
./bin/mqtt_router -c mqtt_router.yaml 2>&1 | tee debug.log
```

### 2. MQTT服务器连接路由失败

检查项：
- [ ] 路由服务是否运行：`./scripts/start-router.sh status`
- [ ] 网络连接是否正常：`telnet 127.0.0.1 9090`
- [ ] 防火墙设置：确保端口9090可访问
- [ ] 配置文件中的路由地址是否正确

```bash
# 检查网络连接
ping 127.0.0.1
telnet 127.0.0.1 9090
```

### 3. 消息路由不工作

检查项：
- [ ] 客户端是否成功连接：查看连接日志
- [ ] 订阅是否正确注册：检查路由服务日志
- [ ] 主题匹配是否正确：验证通配符使用
- [ ] 服务器ID是否唯一：确保每个实例有不同ID

```bash
# 查看路由服务统计信息
grep "Statistics:" /tmp/mqtt_router.log | tail -5
```

### 4. 性能问题

优化方案：
1. **增加并发数**：调整worker_thread_count和coroutines_per_thread
2. **优化内存**：增加内存限制，减少内存分配频率
3. **调整持久化**：减少snapshot频率，增加批量大小
4. **网络优化**：启用TCP_NODELAY，调整缓冲区大小

## 监控和运维

### 1. 关键监控指标

- **连接数监控**：活跃的MQTT服务器实例数量
- **订阅数监控**：全局主题订阅总数
- **消息吞吐量**：每秒处理的路由请求数
- **内存使用率**：路由服务内存占用情况
- **磁盘I/O**：redo log和snapshot的读写性能

### 2. 日志分析

```bash
# 统计连接数
grep "Client.*connected" logs/mqtts.log | wc -l

# 统计消息转发数
grep "forwarded.*subscribers" logs/mqtts.log | wc -l

# 查看错误信息
grep "ERROR\|WARN" logs/mqtts.log | tail -20

# 路由服务统计
grep "Statistics:" /tmp/mqtt_router.log | tail -10
```

### 3. 性能基准测试

```bash
# 使用MQTT压测工具
mosquitto_pub -h localhost -p 1883 -t test/topic -m "test message" -r 1000

# 监控系统资源
htop
iotop -o
```

## 开发和扩展

### 1. 添加新的RPC接口

1. 在`mqtt_router.proto`中定义新的RPC方法
2. 在`MQTTRouterRpcHandler`中实现处理逻辑
3. 在`MQTTRouterRpcClient`中添加客户端调用方法
4. 更新相关的序列化/反序列化代码

### 2. 扩展持久化功能

1. 在`RouterLogOpType`中添加新的操作类型
2. 在`MQTTRedoLogManager`中实现新操作的序列化
3. 在`MQTTPersistentTopicTree`中添加应用逻辑
4. 更新snapshot格式以支持新数据

### 3. 性能优化建议

1. **内存池优化**：为频繁分配的对象实现对象池
2. **批量处理**：增加更多的批量操作接口
3. **压缩算法**：对redo log和snapshot实现压缩
4. **索引优化**：为主题树添加更高效的索引结构

## 许可证和贡献

本项目遵循项目原有的许可证。欢迎提交Issue和Pull Request来改进系统功能和性能。

## 支持

如需技术支持或有任何问题，请：
1. 查看本文档的故障排除部分
2. 检查系统日志获取详细错误信息
3. 提交Issue描述问题和复现步骤