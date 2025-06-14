#!/bin/bash

# MQTT服务器打包脚本
# 作者: MQTT Team
# 版本: 1.0

set -e  # 遇到错误立即退出

# 脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# 配置
PROGRAM_NAME="mqtts"
VERSION=$(git describe --tags --always)
if [ -z "$VERSION" ]; then
    VERSION="dev-$(date +%Y%m%d)"
fi
PACKAGE_NAME="mqtts-${VERSION}"
TEMP_DIR="/tmp/${PACKAGE_NAME}"
OUTPUT_DIR="$PROJECT_ROOT/releases"
BUILD_TYPE="Release"
CLEAN_BUILD=false
INCLUDE_LOGS=false
STRIP_BINARY=true
SIGN_PACKAGE=false
VALIDATE_CONFIG=true

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
    echo "MQTT服务器打包脚本"
    echo
    echo "用法: $0 [选项]"
    echo
    echo "选项:"
    echo "  -v, --version VER  指定版本号（默认：时间戳）"
    echo "  -o, --output DIR   指定输出目录（默认：releases）"
    echo "  -c, --clean        清理构建"
    echo "  -d, --debug        调试版本构建"
    echo "  -l, --logs         包含日志文件"
    echo "  --no-strip         不压缩可执行文件"
    echo "  --sign             签名打包文件"
    echo "  --no-validate      跳过配置验证"
    echo "  -h, --help         显示此帮助信息"
    echo
    echo "示例:"
    echo "  $0                    # 默认打包"
    echo "  $0 -v 1.0.0          # 指定版本"
    echo "  $0 -c -d             # 清理后调试构建"
}

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -v|--version)
            VERSION="$2"
            PACKAGE_NAME="mqtts-${VERSION}"
            TEMP_DIR="/tmp/${PACKAGE_NAME}"
            shift 2
            ;;
        -o|--output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -l|--logs)
            INCLUDE_LOGS=true
            shift
            ;;
        --no-strip)
            STRIP_BINARY=false
            shift
            ;;
        --sign)
            SIGN_PACKAGE=true
            shift
            ;;
        --no-validate)
            VALIDATE_CONFIG=false
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            log_error "未知选项: $1"
            show_help
            exit 1
            ;;
    esac
done

# 检查系统依赖
check_system_dependencies() {
    log_info "检查系统依赖..."
    
    local missing_deps=()
    
    # 必需工具
    local required_tools=("tar" "gzip" "cmake" "make" "g++" "pkg-config")
    for tool in "${required_tools[@]}"; do
        if ! command -v "$tool" &> /dev/null; then
            missing_deps+=("$tool")
        fi
    done
    
    # 可选工具
    if [ "$STRIP_BINARY" = true ] && ! command -v strip &> /dev/null; then
        log_warning "strip工具未找到，将跳过二进制压缩"
        STRIP_BINARY=false
    fi
    
    if [ "$SIGN_PACKAGE" = true ] && ! command -v gpg &> /dev/null; then
        log_error "GPG未安装，无法签名包"
        exit 1
    fi
    
    # 检查必需库
    if ! pkg-config --exists yaml-cpp; then
        missing_deps+=("libyaml-cpp-dev")
    fi
    
    if [ ${#missing_deps[@]} -gt 0 ]; then
        log_error "缺少以下依赖："
        for dep in "${missing_deps[@]}"; do
            echo "  - $dep"
        done
        exit 1
    fi
    
    log_success "系统依赖检查通过"
}

# 验证配置文件
validate_config() {
    if [ "$VALIDATE_CONFIG" = false ]; then
        return 0
    fi
    
    log_info "验证配置文件..."
    
    local config_file="$PROJECT_ROOT/mqtts.yaml"
    if [ ! -f "$config_file" ]; then
        log_error "配置文件不存在: $config_file"
        exit 1
    fi
    
    # 检查配置文件语法
    if command -v python3 &> /dev/null; then
        python3 -c "import yaml; yaml.safe_load(open('$config_file'))" 2>/dev/null || {
            log_error "配置文件YAML语法错误"
            exit 1
        }
    elif command -v yamllint &> /dev/null; then
        yamllint "$config_file" || {
            log_error "配置文件验证失败"
            exit 1
        }
    else
        log_warning "无法验证YAML语法，跳过配置验证"
    fi
    
    # 检查必要配置项
    local required_keys=("server.bind_address" "server.port" "logger.level")
    for key in "${required_keys[@]}"; do
        if ! grep -q "${key#*.}" "$config_file"; then
            log_warning "配置项可能缺失: $key"
        fi
    done
    
    log_success "配置文件验证通过"
}

# 检查Git信息
get_git_info() {
    local git_commit="Unknown"
    local git_branch="Unknown"
    local git_dirty=""
    
    if command -v git &> /dev/null && [ -d "$PROJECT_ROOT/.git" ]; then
        git_commit=$(cd "$PROJECT_ROOT" && git rev-parse HEAD 2>/dev/null || echo "Unknown")
        git_branch=$(cd "$PROJECT_ROOT" && git branch --show-current 2>/dev/null || echo "Unknown")
        
        # 检查是否有未提交的更改
        if ! git -C "$PROJECT_ROOT" diff-index --quiet HEAD -- 2>/dev/null; then
            git_dirty=" (dirty)"
        fi
    fi
    
    echo "Git提交: ${git_commit}${git_dirty}"
    echo "Git分支: $git_branch"
}

# 清理临时目录
cleanup() {
    if [ -d "$TEMP_DIR" ]; then
        rm -rf "$TEMP_DIR"
        log_info "清理临时目录: $TEMP_DIR"
    fi
}

# 设置清理钩子
trap cleanup EXIT

# 构建项目
build_project() {
    log_info "构建项目..."
    
    local build_args=""
    if [ "$CLEAN_BUILD" = true ]; then
        build_args="$build_args -c"
    fi
    
    if [ "$BUILD_TYPE" = "Release" ]; then
        build_args="$build_args -r"
    elif [ "$BUILD_TYPE" = "Debug" ]; then
        build_args="$build_args -d"
    fi
    
    # 执行构建
    cd "$SCRIPT_DIR"
    ./build.sh $build_args
    
    if [ $? -eq 0 ]; then
        log_success "项目构建完成"
    else
        log_error "项目构建失败"
        exit 1
    fi
}

# 创建打包目录结构
create_package_structure() {
    log_info "创建打包目录结构..."
    
    # 清理并创建临时目录
    rm -rf "$TEMP_DIR"
    mkdir -p "$TEMP_DIR"
    
    # 创建目录结构
    mkdir -p "$TEMP_DIR/bin"
    mkdir -p "$TEMP_DIR/config"
    mkdir -p "$TEMP_DIR/logs"
    mkdir -p "$TEMP_DIR/scripts"
    mkdir -p "$TEMP_DIR/docs"
    mkdir -p "$TEMP_DIR/lib"
    mkdir -p "$TEMP_DIR/data"
    
    log_success "目录结构已创建"
}

# 复制可执行文件和依赖库
copy_executable() {
    log_info "复制可执行文件..."
    
    local exe_src="$PROJECT_ROOT/build/mqtts"
    local exe_dst="$TEMP_DIR/bin/mqtts"
    
    if [ ! -f "$exe_src" ]; then
        log_error "可执行文件不存在: $exe_src"
        exit 1
    fi
    
    cp "$exe_src" "$exe_dst"
    chmod +x "$exe_dst"
    
    # 检查并复制动态库依赖
    log_info "检查动态库依赖..."
    if command -v ldd &> /dev/null; then
        local deps=$(ldd "$exe_src" | grep -v "linux-vdso" | grep -v "ld-linux" | awk '{print $3}' | grep "^/")
        local copied_libs=()
        
        for lib in $deps; do
            if [[ "$lib" == /lib/* || "$lib" == /usr/lib/* ]]; then
                # 系统库，不复制
                continue
            fi
            
            local lib_name=$(basename "$lib")
            cp "$lib" "$TEMP_DIR/lib/"
            copied_libs+=("$lib_name")
        done
        
        if [ ${#copied_libs[@]} -gt 0 ]; then
            log_info "已复制动态库: ${copied_libs[*]}"
            
            # 创建库加载脚本
            cat > "$TEMP_DIR/bin/mqtts-wrapper" << 'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$SCRIPT_DIR/../lib:$LD_LIBRARY_PATH"
exec "$SCRIPT_DIR/mqtts" "$@"
EOF
            chmod +x "$TEMP_DIR/bin/mqtts-wrapper"
        fi
    fi
    
    # 压缩可执行文件
    if [ "$STRIP_BINARY" = true ] && command -v strip &> /dev/null; then
        log_info "压缩可执行文件..."
        strip "$exe_dst"
    fi
    
    log_success "可执行文件已复制"
}

# 复制脚本文件
copy_scripts() {
    log_info "复制脚本文件..."
    
    # 复制管理脚本
    cp "$SCRIPT_DIR/build.sh" "$TEMP_DIR/scripts/"
    cp "$SCRIPT_DIR/start.sh" "$TEMP_DIR/scripts/"
    cp "$SCRIPT_DIR/stop.sh" "$TEMP_DIR/scripts/"
    cp "$SCRIPT_DIR/status.sh" "$TEMP_DIR/scripts/"
    
    # 创建便捷脚本链接
    ln -sf "scripts/start.sh" "$TEMP_DIR/start.sh"
    ln -sf "scripts/stop.sh" "$TEMP_DIR/stop.sh"
    ln -sf "scripts/status.sh" "$TEMP_DIR/status.sh"
    
    # 设置可执行权限
    chmod +x "$TEMP_DIR/scripts"/*.sh
    
    log_success "脚本文件已复制"
}

# 复制配置文件
copy_config() {
    log_info "复制配置文件..."
    
    # 复制主配置文件
    cp "$PROJECT_ROOT/mqtts.yaml" "$TEMP_DIR/config/"
    
    # 创建示例配置文件
    cp "$PROJECT_ROOT/mqtts.yaml" "$TEMP_DIR/config/mqtts.yaml.example"
    
    # 复制其他配置文件（如果存在）
    for config in "$PROJECT_ROOT"/*.yaml "$PROJECT_ROOT"/*.yml "$PROJECT_ROOT"/*.conf; do
        if [ -f "$config" ] && [ "$config" != "$PROJECT_ROOT/mqtts.yaml" ]; then
            cp "$config" "$TEMP_DIR/config/"
        fi
    done
    
    log_success "配置文件已复制"
}

# 复制文档
copy_docs() {
    log_info "复制文档..."
    
    # 复制README
    if [ -f "$PROJECT_ROOT/README.md" ]; then
        cp "$PROJECT_ROOT/README.md" "$TEMP_DIR/docs/"
    fi
    
    # 复制许可证
    if [ -f "$PROJECT_ROOT/LICENSE" ]; then
        cp "$PROJECT_ROOT/LICENSE" "$TEMP_DIR/docs/"
    fi
    
    # 复制其他文档
    for doc in "$PROJECT_ROOT"/*.md "$PROJECT_ROOT"/docs/*; do
        if [ -f "$doc" ]; then
            cp "$doc" "$TEMP_DIR/docs/" 2>/dev/null || true
        fi
    done
    
    # 生成部署说明
    cat > "$TEMP_DIR/docs/DEPLOYMENT.md" << 'EOF'
# MQTT服务器部署说明

## 快速部署

```bash
# 1. 解压文件
tar -xzf mqtts-*.tar.gz
cd mqtts-*

# 2. 运行安装脚本（需要root权限）
sudo ./install.sh

# 3. 启动服务
systemctl start mqtts
systemctl enable mqtts
```

## 手动部署

```bash
# 1. 解压到目标目录
sudo mkdir -p /opt/mqtts
sudo tar -xzf mqtts-*.tar.gz -C /opt/mqtts --strip-components=1

# 2. 创建服务用户
sudo useradd -r -s /bin/false mqtts

# 3. 设置权限
sudo chown -R mqtts:mqtts /opt/mqtts
sudo chmod +x /opt/mqtts/bin/mqtts

# 4. 手动启动
cd /opt/mqtts
sudo -u mqtts ./start.sh
```

## 配置说明

主配置文件位于 `config/mqtts.yaml`，包含以下主要配置项：

- `server.bind_address`: 绑定地址
- `server.port`: 监听端口
- `server.max_connections`: 最大连接数
- `logger.level`: 日志级别
- `logger.file_path`: 日志文件路径

## 服务管理

```bash
# 查看状态
./status.sh

# 启动服务
./start.sh

# 停止服务
./stop.sh

# 后台启动
./start.sh -d
```

## 故障排除

1. 检查日志文件：`logs/mqtts.log`
2. 检查端口占用：`netstat -tuln | grep 1883`
3. 检查进程状态：`./status.sh -d`
4. 检查配置文件：`yaml-lint config/mqtts.yaml`

## 更新升级

1. 停止服务：`./stop.sh`
2. 备份配置：`cp config/mqtts.yaml config/mqtts.yaml.bak`
3. 解压新版本到临时目录
4. 复制新的可执行文件和脚本
5. 恢复配置文件
6. 重新启动服务
EOF
    
    log_success "文档已复制"
}

# 复制日志文件（可选）
copy_logs() {
    if [ "$INCLUDE_LOGS" = true ]; then
        log_info "复制日志文件..."
        
        local log_src="$PROJECT_ROOT/logs"
        if [ -d "$log_src" ]; then
            cp -r "$log_src"/* "$TEMP_DIR/logs/" 2>/dev/null || true
            log_success "日志文件已复制"
        else
            log_warning "日志目录不存在，跳过日志文件复制"
        fi
    fi
}

# 生成版本信息
generate_version_info() {
    log_info "生成版本信息..."
    
    local version_file="$TEMP_DIR/VERSION"
    local git_info=$(get_git_info)
    
    cat > "$version_file" << EOF
MQTT服务器版本信息
==================

版本号: $VERSION
构建时间: $(date)
构建类型: $BUILD_TYPE
构建主机: $(hostname)
系统信息: $(uname -a)
$git_info

编译器信息:
$(g++ --version | head -1)

依赖库版本:
$(pkg-config --modversion yaml-cpp 2>/dev/null || echo "yaml-cpp: Unknown")

包内容:
EOF
    
    # 添加文件列表和大小
    find "$TEMP_DIR" -type f -exec ls -lh {} \; | sed 's|'$TEMP_DIR'||' | sort >> "$version_file"
    
    # 添加校验和
    echo "" >> "$version_file"
    echo "文件校验和:" >> "$version_file"
    find "$TEMP_DIR" -type f -name "*.sh" -o -name "mqtts" -o -name "*.yaml" | while read file; do
        local checksum=$(sha256sum "$file" | cut -d' ' -f1)
        local relative_path=$(echo "$file" | sed 's|'$TEMP_DIR'/||')
        echo "$checksum  $relative_path" >> "$version_file"
    done
    
    log_success "版本信息已生成"
}

# 创建压缩包
create_archive() {
    log_info "创建压缩包..."
    
    # 创建输出目录
    mkdir -p "$OUTPUT_DIR"
    
    local archive_path="$OUTPUT_DIR/${PACKAGE_NAME}.tar.gz"
    
    # 进入临时目录的父目录
    cd "$(dirname "$TEMP_DIR")"
    
    # 创建tar包
    tar -czf "$archive_path" "$(basename "$TEMP_DIR")"
    
    if [ $? -eq 0 ]; then
        log_success "压缩包已创建: $archive_path"
        
        # 显示文件信息
        local size=$(du -h "$archive_path" | cut -f1)
        log_info "压缩包大小: $size"
        
        # 生成校验和
        local checksum=$(sha256sum "$archive_path" | cut -d' ' -f1)
        echo "$checksum  $(basename "$archive_path")" > "${archive_path}.sha256"
        log_info "SHA256校验和: $checksum"
        
        # 签名包（如果启用）
        if [ "$SIGN_PACKAGE" = true ]; then
            log_info "签名包..."
            gpg --detach-sign --armor "$archive_path"
            if [ $? -eq 0 ]; then
                log_success "包已签名: ${archive_path}.asc"
            else
                log_error "包签名失败"
            fi
        fi
        
        return 0
    else
        log_error "创建压缩包失败"
        return 1
    fi
}

# 显示打包信息
show_package_info() {
    log_info "打包信息："
    echo "  包名称: $PACKAGE_NAME"
    echo "  版本号: $VERSION"
    echo "  构建类型: $BUILD_TYPE"
    echo "  输出目录: $OUTPUT_DIR"
    echo "  包含日志: $([ "$INCLUDE_LOGS" = true ] && echo "是" || echo "否")"
    echo "  压缩二进制: $([ "$STRIP_BINARY" = true ] && echo "是" || echo "否")"
    echo "  签名包: $([ "$SIGN_PACKAGE" = true ] && echo "是" || echo "否")"
}

# 主函数
main() {
    log_info "开始打包MQTT服务器..."
    
    show_package_info
    
    check_system_dependencies
    validate_config
    build_project
    create_package_structure
    copy_executable
    copy_scripts
    copy_config
    copy_docs
    copy_logs
    generate_version_info
    
    if create_archive; then
        log_success "MQTT服务器打包完成！"
        echo
        echo "生成的文件:"
        ls -lh "$OUTPUT_DIR/${PACKAGE_NAME}"*
        echo
        echo "部署方法:"
        echo "1. 解压: tar -xzf ${PACKAGE_NAME}.tar.gz"
        echo "2. 进入: cd ${PACKAGE_NAME}"
        echo "3. 安装: sudo ./install.sh"
        echo "4. 启动: systemctl start mqtts"
    else
        log_error "打包失败"
        exit 1
    fi
}

# 执行主函数
main "$@" 