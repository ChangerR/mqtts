#!/bin/bash

# MQTT服务器Docker打包脚本
# 作者: MQTT Team
# 版本: 1.0

set -e  # 遇到错误立即退出

# 脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 日志函数
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 显示帮助信息
show_help() {
    echo "MQTT服务器Docker打包脚本"
    echo
    echo "用法: $0 [选项]"
    echo
    echo "选项:"
    echo "  -v, --version VER    指定版本号（默认：从Git标签或时间戳生成）"
    echo "  -t, --tag TAG        指定额外的Docker标签（可多次使用）"
    echo "  -p, --push          构建后推送到Docker仓库"
    echo "  -r, --registry URL  指定Docker仓库地址"
    echo "  --no-cache          不使用Docker构建缓存"
    echo "  --skip-build        跳过构建步骤，直接使用已有的发布包"
    echo "  --package PATH      指定已有的发布包路径（配合--skip-build使用）"
    echo "  -h, --help          显示此帮助信息"
    echo
    echo "示例:"
    echo "  $0                              # 从源码构建并生成Docker镜像"
    echo "  $0 -v 1.0.0 -t latest          # 指定版本号和标签"
    echo "  $0 -p -r docker.example.com    # 构建并推送到私有仓库"
    echo "  $0 --skip-build --package ../releases/mqtts-1.0.0.tar.gz  # 使用已有发布包"
}

# 默认值
DOCKER_VERSION=""
DOCKER_TAGS=()
PUSH_IMAGE=false
DOCKER_REGISTRY=""
USE_CACHE=true
RELEASE_PACKAGE=""
SKIP_BUILD=false
BUILD_ARGS=""

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -v|--version)
            DOCKER_VERSION="$2"
            shift 2
            ;;
        -t|--tag)
            DOCKER_TAGS+=("$2")
            shift 2
            ;;
        -p|--push)
            PUSH_IMAGE=true
            shift
            ;;
        -r|--registry)
            DOCKER_REGISTRY="$2"
            shift 2
            ;;
        --no-cache)
            USE_CACHE=false
            shift
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        --package)
            RELEASE_PACKAGE="$2"
            shift 2
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            log_error "未知参数: $1"
            show_help
            exit 1
            ;;
    esac
done

# 检查Docker是否安装
if ! command -v docker &> /dev/null; then
    log_error "未找到Docker，请先安装Docker"
    exit 1
fi

# 如果没有跳过构建，则调用package.sh生成发布包
if [ "$SKIP_BUILD" = false ]; then
    log_info "开始构建发布包..."
    
    # 构建参数
    PACKAGE_ARGS=""
    if [ -n "$DOCKER_VERSION" ]; then
        PACKAGE_ARGS="$PACKAGE_ARGS -v $DOCKER_VERSION"
    fi
    
    # 调用package.sh
    if ! "$SCRIPT_DIR/package.sh" $PACKAGE_ARGS; then
        log_error "发布包构建失败"
        exit 1
    fi
    
    # 获取最新构建的发布包
    RELEASE_PACKAGE=$(find "$PROJECT_ROOT/releases" -name "mqtts-*.tar.gz" -type f -printf '%T@ %p\n' | sort -n | tail -1 | cut -f2- -d" ")
    
    if [ -z "$RELEASE_PACKAGE" ]; then
        log_error "无法找到构建的发布包"
        exit 1
    fi
    
    log_success "发布包构建完成: $RELEASE_PACKAGE"
else
    # 如果跳过构建，检查指定的发布包是否存在
    if [ -z "$RELEASE_PACKAGE" ]; then
        log_error "使用--skip-build时必须通过--package指定发布包路径"
        exit 1
    fi
    
    if [ ! -f "$RELEASE_PACKAGE" ]; then
        log_error "指定的发布包不存在: $RELEASE_PACKAGE"
        exit 1
    fi
fi

# 从发布包名称提取版本号（如果未指定）
if [ -z "$DOCKER_VERSION" ]; then
    DOCKER_VERSION=$(basename "$RELEASE_PACKAGE" | sed -E 's/mqtts-([0-9]+\.[0-9]+\.[0-9]+.*).tar.gz/\1/')
    if [ "$DOCKER_VERSION" == "$(basename "$RELEASE_PACKAGE")" ]; then
        log_error "无法从发布包名称提取版本号，请使用 -v 选项指定版本"
        exit 1
    fi
fi

# 准备Docker构建目录
DOCKER_BUILD_DIR="/tmp/mqtts-docker-build"
rm -rf "$DOCKER_BUILD_DIR"
mkdir -p "$DOCKER_BUILD_DIR"

# 解压发布包
log_info "解压发布包..."
tar -xzf "$RELEASE_PACKAGE" -C "$DOCKER_BUILD_DIR"
EXTRACTED_DIR=$(find "$DOCKER_BUILD_DIR" -maxdepth 1 -type d -name "mqtts-*" | head -n 1)

if [ -z "$EXTRACTED_DIR" ]; then
    log_error "无法找到解压后的目录"
    exit 1
fi

# 创建Dockerfile
log_info "创建Dockerfile..."
cat > "$DOCKER_BUILD_DIR/Dockerfile" << EOF
FROM ubuntu:22.04

# 创建必要的目录
RUN mkdir -p /app/bin /app/config /app/logs

# 创建非特权用户
RUN useradd -r -s /bin/false -d /app mqtts && \\
    chown -R mqtts:mqtts /app

# 复制文件
COPY mqtts-*/bin/mqtts /app/bin/
COPY mqtts-*/config/mqtts.yaml /app/config/

# 设置权限
RUN chmod +x /app/bin/mqtts && \\
    chown -R mqtts:mqtts /app

# 设置工作目录
WORKDIR /app

# 暴露MQTT端口
EXPOSE 1883 8883

# 切换到非特权用户
USER mqtts

# 健康检查
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \\
    CMD nc -z localhost 1883 || exit 1

# 启动命令
CMD ["/app/bin/mqtts", "-c", "/app/config/mqtts.yaml"]
EOF

# 构建Docker镜像
log_info "构建Docker镜像..."
IMAGE_NAME="mqtts:${DOCKER_VERSION}"
if [ -n "$DOCKER_REGISTRY" ]; then
    IMAGE_NAME="${DOCKER_REGISTRY}/${IMAGE_NAME}"
fi

BUILD_ARGS=""
if [ "$USE_CACHE" = false ]; then
    BUILD_ARGS="--no-cache"
fi

cd "$DOCKER_BUILD_DIR"
if docker build $BUILD_ARGS -t "$IMAGE_NAME" .; then
    log_success "Docker镜像构建成功: $IMAGE_NAME"
    
    # 添加额外标签
    for tag in "${DOCKER_TAGS[@]}"; do
        TAG_NAME="mqtts:${tag}"
        if [ -n "$DOCKER_REGISTRY" ]; then
            TAG_NAME="${DOCKER_REGISTRY}/${TAG_NAME}"
        fi
        docker tag "$IMAGE_NAME" "$TAG_NAME"
        log_success "添加标签: $TAG_NAME"
    done
    
    # 推送镜像（如果启用）
    if [ "$PUSH_IMAGE" = true ]; then
        log_info "推送Docker镜像..."
        docker push "$IMAGE_NAME"
        for tag in "${DOCKER_TAGS[@]}"; do
            TAG_NAME="mqtts:${tag}"
            if [ -n "$DOCKER_REGISTRY" ]; then
                TAG_NAME="${DOCKER_REGISTRY}/${TAG_NAME}"
            fi
            docker push "$TAG_NAME"
        done
        log_success "Docker镜像推送完成"
    fi
    
    # 保存镜像（可选）
    if [ -d "$(dirname "$RELEASE_PACKAGE")" ]; then
        SAVE_PATH="$(dirname "$RELEASE_PACKAGE")/mqtts-${DOCKER_VERSION}-docker.tar.gz"
        log_info "保存Docker镜像到: $SAVE_PATH"
        docker save "$IMAGE_NAME" | gzip > "$SAVE_PATH"
        log_success "Docker镜像已保存"
    fi
    
    # 显示使用说明
    echo
    echo "Docker镜像使用说明:"
    echo "1. 运行容器:"
    echo "   docker run -d \\"
    echo "     --name mqtts \\"
    echo "     -p 1883:1883 \\"
    echo "     $IMAGE_NAME"
    echo
    echo "2. 查看容器状态:"
    echo "   docker ps -f name=mqtts"
    echo
    echo "3. 查看容器日志:"
    echo "   docker logs -f mqtts"
    echo
    echo "4. 停止容器:"
    echo "   docker stop mqtts"
    echo
    echo "5. 删除容器:"
    echo "   docker rm mqtts"
else
    log_error "Docker镜像构建失败"
    exit 1
fi

# 清理
rm -rf "$DOCKER_BUILD_DIR"
log_success "Docker打包完成！"
