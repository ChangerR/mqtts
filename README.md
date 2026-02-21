# MQTTS

MQTTS 是一个基于 C++ 的 MQTT v5.0 服务端实现。

## 容器优先约定（重要）

由于项目目标环境是 Linux，而当前开发机可能是 macOS，后续开发与任务验证统一遵循：

- 编译在容器中执行
- 单元测试在容器中执行
- 运行验证在容器中执行

默认入口脚本：`./bin/container-validate.sh`

## 前置要求

- 已安装 Docker（Desktop 或 Engine）
- 仓库根目录执行命令

## 一键容器验证（编译 + 单测）

```bash
./bin/container-validate.sh all
```

该命令会：

- 构建开发镜像（基于 `.devcontainer/Dockerfile`）
- 在容器中执行 CMake configure + build
- 在容器中执行 `ctest --output-on-failure`

## 分步骤命令

1. 仅构建开发镜像

```bash
./bin/container-validate.sh image
```

2. 仅配置

```bash
./bin/container-validate.sh configure
```

3. 容器内编译

```bash
./bin/container-validate.sh build
```

4. 容器内执行单测

```bash
./bin/container-validate.sh test
```

5. 仅运行某类测试（ctest 过滤）

```bash
TEST_REGEX=test_mqtt_parser ./bin/container-validate.sh test
```

## 运行服务容器验证

```bash
./bin/container-validate.sh run
```

等价于调用 `./bin/docker-run.sh`，默认行为：

- 自动构建运行镜像 `mqtts:local`（若本地不存在）
- 启动容器并映射端口 `1883`（MQTT）和 `8080`（WebSocket）
- 挂载配置 `${PROJECT_ROOT}/mqtts.yaml -> /app/config/mqtts.yaml:ro`

查看日志：

```bash
docker logs -f mqtts
```

停止并删除：

```bash
docker rm -f mqtts
```

## 常用环境变量

`bin/container-validate.sh` 支持以下参数：

- `DEV_IMAGE`：开发镜像名，默认 `mqtts-dev:local`
- `DEV_DOCKERFILE`：开发镜像 Dockerfile，默认 `.devcontainer/Dockerfile`
- `BUILD_DIR`：构建目录，默认 `build-container`
- `BUILD_TYPE`：默认 `RelWithDebInfo`
- `JOBS`：并行编译线程数（默认 CPU 核数）
- `CTEST_TIMEOUT`：单测超时秒数，默认 `120`
- `TEST_REGEX`：ctest 过滤正则

示例：

```bash
BUILD_TYPE=Debug JOBS=8 ./bin/container-validate.sh all
```

## 本地开发 Shell（可选）

```bash
./bin/container-validate.sh shell
```

该命令会进入开发镜像并挂载当前仓库，便于手动调试。
