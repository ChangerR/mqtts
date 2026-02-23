#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

DEV_DOCKERFILE="${DEV_DOCKERFILE:-$PROJECT_ROOT/.devcontainer/Dockerfile}"
DEV_IMAGE="${DEV_IMAGE:-mqtts-dev:local}"
DEV_CONTAINER_NAME="${DEV_CONTAINER_NAME:-mqtts-dev-builder}"
WORKDIR_IN_CONTAINER="${WORKDIR_IN_CONTAINER:-/workspace/mqtts}"
BUILD_DIR="${BUILD_DIR:-build-container}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
CTEST_TIMEOUT="${CTEST_TIMEOUT:-120}"
TEST_REGEX="${TEST_REGEX:-}"

if [ -n "${JOBS:-}" ]; then
  JOBS="${JOBS}"
elif command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
elif command -v getconf >/dev/null 2>&1; then
  JOBS="$(getconf _NPROCESSORS_ONLN)"
else
  JOBS="4"
fi

log() {
  printf '[container-validate] %s\n' "$1"
}

die() {
  printf '[container-validate] ERROR: %s\n' "$1" >&2
  exit 1
}

show_help() {
  cat <<'USAGE'
容器化编译/运行/测试脚本

用法:
  ./bin/container-validate.sh [全局参数] <command> [command args]

全局参数:
  --dockerfile PATH        开发镜像 Dockerfile，默认 .devcontainer/Dockerfile
  --image NAME             开发镜像名，默认 mqtts-dev:local
  --container-name NAME    编译容器名，默认 mqtts-dev-builder
  --workdir PATH           容器内工作目录，默认 /workspace/mqtts
  --build-dir DIR          容器内构建目录，默认 build-container
  --build-type TYPE        CMake 构建类型，默认 RelWithDebInfo
  -j, --jobs N             编译并行度，默认 CPU 核数
  --ctest-timeout SEC      单测超时时间(秒)，默认 120
  --test-regex REGEX       ctest 过滤正则（可选）
  -h, --help               显示帮助

命令:
  image               构建开发镜像（用于编译和单测）
  start               启动/恢复常驻编译容器（不存在会自动创建）
  stop                停止并删除常驻编译容器
  status              查看常驻编译容器状态
  configure           在常驻容器中执行 CMake 配置（容器未启动会自动启动）
  build               在常驻容器中执行编译（自动先 configure）
  test                在常驻容器中执行 ctest（容器未启动会自动启动）
  all                 在常驻容器中执行 configure + build + test
  run [docker args]   启动运行容器（透传参数给 ./bin/docker-run.sh）
  shell               进入常驻编译容器 shell（容器未启动会自动启动）
  help                显示帮助

示例:
  ./bin/container-validate.sh all
  ./bin/container-validate.sh --test-regex test_mqtt_parser test
  ./bin/container-validate.sh --image mqtts-dev:ci --container-name mqtts-dev-ci start
  ./bin/container-validate.sh stop
USAGE
}

require_docker() {
  command -v docker >/dev/null 2>&1 || die "docker 未安装或不在 PATH 中"
}

container_exists() {
  docker ps -a --format '{{.Names}}' | grep -qx "$DEV_CONTAINER_NAME"
}

container_running() {
  docker ps --format '{{.Names}}' | grep -qx "$DEV_CONTAINER_NAME"
}

build_dev_image() {
  require_docker
  [ -f "$DEV_DOCKERFILE" ] || die "开发镜像 Dockerfile 不存在: $DEV_DOCKERFILE"
  log "构建开发镜像: $DEV_IMAGE"
  docker build -f "$DEV_DOCKERFILE" -t "$DEV_IMAGE" "$PROJECT_ROOT"
}

start_builder_container() {
  require_docker
  build_dev_image

  if container_running; then
    log "编译容器已在运行: $DEV_CONTAINER_NAME"
    return
  fi

  if container_exists; then
    log "启动已存在的编译容器: $DEV_CONTAINER_NAME"
    docker start "$DEV_CONTAINER_NAME" >/dev/null
    return
  fi

  log "创建并启动常驻编译容器: $DEV_CONTAINER_NAME"
  docker run -d \
    --name "$DEV_CONTAINER_NAME" \
    -v "$PROJECT_ROOT":"$WORKDIR_IN_CONTAINER" \
    -w "$WORKDIR_IN_CONTAINER" \
    "$DEV_IMAGE" \
    bash -lc 'tail -f /dev/null' >/dev/null
}

ensure_builder_container_running() {
  if ! container_running; then
    log "编译容器未运行，自动启动"
    start_builder_container
  fi
}

stop_builder_container() {
  require_docker
  if container_exists; then
    log "停止并删除编译容器: $DEV_CONTAINER_NAME"
    docker rm -f "$DEV_CONTAINER_NAME" >/dev/null
  else
    log "编译容器不存在: $DEV_CONTAINER_NAME"
  fi
}

show_builder_container_status() {
  require_docker
  if ! container_exists; then
    log "编译容器状态: not-found ($DEV_CONTAINER_NAME)"
    return
  fi

  local state
  state="$(docker inspect -f '{{.State.Status}}' "$DEV_CONTAINER_NAME")"
  log "编译容器状态: $state ($DEV_CONTAINER_NAME)"
}

run_in_dev_container() {
  local cmd="$1"
  ensure_builder_container_running
  if [ -t 1 ]; then
    docker exec -t "$DEV_CONTAINER_NAME" bash -lc "$cmd"
  else
    docker exec "$DEV_CONTAINER_NAME" bash -lc "$cmd"
  fi
}

configure_project() {
  log "容器内配置 CMake (BUILD_DIR=$BUILD_DIR, BUILD_TYPE=$BUILD_TYPE)"
  run_in_dev_container "cmake -S . -B \"$BUILD_DIR\" -G Ninja -DCMAKE_BUILD_TYPE=\"$BUILD_TYPE\" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
}

build_project() {
  configure_project
  log "容器内编译项目 (JOBS=$JOBS)"
  run_in_dev_container "cmake --build \"$BUILD_DIR\" -- -j\"$JOBS\""
}

run_tests() {
  log "容器内执行单测 (ctest)"
  local test_cmd="ctest --test-dir \"$BUILD_DIR\" --output-on-failure --timeout \"$CTEST_TIMEOUT\""
  if [ -n "$TEST_REGEX" ]; then
    test_cmd="$test_cmd -R \"$TEST_REGEX\""
  fi
  run_in_dev_container "$test_cmd"
}

open_shell() {
  ensure_builder_container_running
  docker exec -it "$DEV_CONTAINER_NAME" bash
}

run_service_container() {
  "$SCRIPT_DIR/docker-run.sh" "$@"
}

parse_args() {
  COMMAND=""
  COMMAND_ARGS=()

  while [ "$#" -gt 0 ]; do
    case "$1" in
      --dockerfile)
        [ "$#" -ge 2 ] || die "--dockerfile 缺少参数"
        DEV_DOCKERFILE="$2"
        shift 2
        ;;
      --image)
        [ "$#" -ge 2 ] || die "--image 缺少参数"
        DEV_IMAGE="$2"
        shift 2
        ;;
      --container-name)
        [ "$#" -ge 2 ] || die "--container-name 缺少参数"
        DEV_CONTAINER_NAME="$2"
        shift 2
        ;;
      --workdir)
        [ "$#" -ge 2 ] || die "--workdir 缺少参数"
        WORKDIR_IN_CONTAINER="$2"
        shift 2
        ;;
      --build-dir)
        [ "$#" -ge 2 ] || die "--build-dir 缺少参数"
        BUILD_DIR="$2"
        shift 2
        ;;
      --build-type)
        [ "$#" -ge 2 ] || die "--build-type 缺少参数"
        BUILD_TYPE="$2"
        shift 2
        ;;
      -j|--jobs)
        [ "$#" -ge 2 ] || die "--jobs 缺少参数"
        JOBS="$2"
        shift 2
        ;;
      --ctest-timeout)
        [ "$#" -ge 2 ] || die "--ctest-timeout 缺少参数"
        CTEST_TIMEOUT="$2"
        shift 2
        ;;
      --test-regex)
        [ "$#" -ge 2 ] || die "--test-regex 缺少参数"
        TEST_REGEX="$2"
        shift 2
        ;;
      help|-h|--help)
        COMMAND="help"
        shift
        break
        ;;
      image|start|stop|status|configure|build|test|all|run|shell)
        COMMAND="$1"
        shift
        COMMAND_ARGS=("$@")
        break
        ;;
      *)
        die "未知参数或命令: $1"
        ;;
    esac
  done

  if [ -z "$COMMAND" ]; then
    COMMAND="help"
  fi
}

main() {
  parse_args "$@"

  case "$COMMAND" in
    image)
      build_dev_image
      ;;
    start)
      start_builder_container
      show_builder_container_status
      ;;
    stop)
      stop_builder_container
      ;;
    status)
      show_builder_container_status
      ;;
    configure)
      configure_project
      ;;
    build)
      build_project
      ;;
    test)
      run_tests
      ;;
    all)
      build_project
      run_tests
      ;;
    run)
      if [ "${COMMAND_ARGS+set}" = "set" ] && [ "${#COMMAND_ARGS[@]}" -gt 0 ]; then
        run_service_container "${COMMAND_ARGS[@]}"
      else
        run_service_container
      fi
      ;;
    shell)
      open_shell
      ;;
    help)
      show_help
      ;;
    *)
      die "未知命令: $COMMAND"
      ;;
  esac
}

main "$@"
