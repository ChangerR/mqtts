#!/bin/bash

# MQTT服务器构建脚本
# 作者: MQTT Team
# 版本: 1.0

set -e  # 遇到错误立即退出

# 脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

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
    echo "MQTT服务器构建脚本"
    echo
    echo "用法: $0 [选项]"
    echo
    echo "选项:"
    echo "  -c, --clean        清理构建目录"
    echo "  -r, --release      发布版本构建（优化）"
    echo "  -d, --debug        调试版本构建"
    echo "  -j, --jobs NUM     并行构建任务数（默认：CPU核心数）"
    echo "  -h, --help         显示此帮助信息"
    echo
    echo "示例:"
    echo "  $0                 # 默认构建"
    echo "  $0 -c              # 清理后构建"
    echo "  $0 -r              # 发布版本构建"
    echo "  $0 -d -j 4         # 调试版本，4线程并行"
}

# 默认参数
CLEAN=false
BUILD_TYPE="RelWithDebInfo"
JOBS=1

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -c|--clean)
            CLEAN=true
            shift
            ;;
        -r|--release)
            BUILD_TYPE="Release"
            shift
            ;;
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
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

# 检查依赖
check_dependencies() {
    log_info "检查构建依赖..."
    
    # 检查cmake
    if ! command -v cmake &> /dev/null; then
        log_error "cmake 未安装，请先安装cmake"
        exit 1
    fi
    
    # 检查make
    if ! command -v make &> /dev/null; then
        log_error "make 未安装，请先安装make"
        exit 1
    fi
    
    # 检查g++
    if ! command -v g++ &> /dev/null; then
        log_error "g++ 未安装，请先安装g++"
        exit 1
    fi
    
    # 检查pkg-config
    if ! command -v pkg-config &> /dev/null; then
        log_error "pkg-config 未安装，请先安装pkg-config"
        exit 1
    fi
    
    # 检查yaml-cpp
    if ! pkg-config --exists yaml-cpp; then
        log_error "yaml-cpp 未安装，请先安装libyaml-cpp-dev"
        exit 1
    fi
    
    log_success "所有依赖检查通过"
}

# 清理构建目录
clean_build() {
    if [ "$CLEAN" = true ]; then
        log_info "清理构建目录..."
        rm -rf "$BUILD_DIR"
        log_success "构建目录已清理"
    fi
}

# 创建构建目录
create_build_dir() {
    log_info "创建构建目录..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
}

# 配置项目
configure_project() {
    log_info "配置项目（构建类型：$BUILD_TYPE）..."
    
    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_CXX_STANDARD=11
    
    if [ $? -eq 0 ]; then
        log_success "项目配置完成"
    else
        log_error "项目配置失败"
        exit 1
    fi
}

# 编译项目
build_project() {
    log_info "开始编译项目（并行任务数：$JOBS）..."
    
    make -j"$JOBS"
    
    if [ $? -eq 0 ]; then
        log_success "项目编译完成"
    else
        log_error "项目编译失败"
        exit 1
    fi
}

# 运行测试
run_tests() {
    log_info "运行单元测试..."
    
    if [ -d "unittest" ]; then
        cd unittest
        for test_file in test_*; do
            if [ -x "$test_file" ]; then
                log_info "运行测试：$test_file"
                ./"$test_file"
                if [ $? -ne 0 ]; then
                    log_warning "测试 $test_file 失败"
                fi
            fi
        done
        cd ..
        log_success "测试运行完成"
    else
        log_warning "未找到测试文件"
    fi
}

# 显示构建信息
show_build_info() {
    log_info "构建信息："
    echo "  项目目录: $PROJECT_ROOT"
    echo "  构建目录: $BUILD_DIR" 
    echo "  构建类型: $BUILD_TYPE"
    echo "  并行任务: $JOBS"
    echo "  可执行文件: $BUILD_DIR/mqtts"
    echo "  配置文件: $PROJECT_ROOT/mqtts.yaml"
}

# 主函数
main() {
    log_info "开始构建MQTT服务器..."
    
    check_dependencies
    clean_build
    create_build_dir
    configure_project
    build_project
    run_tests
    
    log_success "MQTT服务器构建完成！"
    show_build_info
    
    # 检查可执行文件
    if [ -f "$BUILD_DIR/mqtts" ]; then
        log_success "可执行文件已生成：$BUILD_DIR/mqtts"
        
        # 显示文件信息
        ls -lh "$BUILD_DIR/mqtts"
        
        # 创建符号链接到bin目录
        ln -sf "../build/mqtts" "$SCRIPT_DIR/mqtts"
        log_info "已创建符号链接：$SCRIPT_DIR/mqtts"
    else
        log_error "可执行文件未生成"
        exit 1
    fi
}

# 执行主函数
main "$@" 
