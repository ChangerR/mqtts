#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="${IMAGE_NAME:-mqtts:local}"
CONTAINER_NAME="${CONTAINER_NAME:-mqtts}"
CONFIG_PATH="${CONFIG_PATH:-$PROJECT_ROOT/mqtts.yaml}"
LOG_DIR="${LOG_DIR:-$PROJECT_ROOT/logs}"
EXTRA_ARGS=("$@")

if ! command -v docker >/dev/null 2>&1; then
  echo "docker 未安装或不在 PATH 中" >&2
  exit 1
fi

if [ ! -f "$CONFIG_PATH" ]; then
  echo "配置文件不存在: $CONFIG_PATH" >&2
  exit 1
fi

mkdir -p "$LOG_DIR"

if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  echo "镜像不存在，先构建: $IMAGE_NAME"
  docker build -t "$IMAGE_NAME" "$PROJECT_ROOT"
fi

if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
  docker rm -f "$CONTAINER_NAME" >/dev/null
fi

docker run -d \
  --name "$CONTAINER_NAME" \
  --restart unless-stopped \
  -p 1883:1883 \
  -p 8080:8080 \
  -v "$CONFIG_PATH":/app/config/mqtts.yaml:ro \
  -v "$LOG_DIR":/app/logs \
  "${EXTRA_ARGS[@]}" \
  "$IMAGE_NAME"

echo "容器已启动: $CONTAINER_NAME"
docker ps --filter "name=$CONTAINER_NAME"
