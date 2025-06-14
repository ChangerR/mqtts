#!/bin/bash

# MQTT服务器启动脚本
# 作者: MQTT Team
# 版本: 1.0

set -e  # 遇到错误立即退出

# 脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# 配置
PROGRAM_NAME="mqtts"
EXECUTABLE="$SCRIPT_DIR/bin/$PROGRAM_NAME"
CONFIG_FILE="$SCRIPT_DIR/config/mqtts.yaml"
PID_FILE="$SCRIPT_DIR/mqtts.pid"
LOG_DIR="$SCRIPT_DIR/logs"
DAEMON_MODE=false
FORCE_START=false

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
    echo "MQTT服务器启动脚本"
    echo
    echo "用法: $0 [选项]"
    echo
    echo "选项:"
    echo "  -d, --daemon       后台运行模式"
    echo "  -f, --force        强制启动（即使已运行）"
    echo "  -c, --config FILE  指定配置文件（默认：mqtts.yaml）"
    echo "  -h, --help         显示此帮助信息"
    echo
    echo "示例:"
    echo "  $0                 # 前台启动"
    echo "  $0 -d              # 后台启动"
    echo "  $0 -f -d           # 强制后台启动"
    echo "  $0 -c /path/to/config.yaml  # 指定配置文件"
}

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--daemon)
            DAEMON_MODE=true
            shift
            ;;
        -f|--force)
            FORCE_START=true
            shift
            ;;
        -c|--config)
            CONFIG_FILE="$2"
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

# 检查可执行文件
check_executable() {
    if [ ! -f "$EXECUTABLE" ]; then
        log_error "可执行文件不存在: $EXECUTABLE"
        log_info "请先运行 ./build.sh 构建项目"
        exit 1
    fi
    
    if [ ! -x "$EXECUTABLE" ]; then
        log_error "文件不可执行: $EXECUTABLE"
        exit 1
    fi
}

# 检查配置文件
check_config() {
    if [ ! -f "$CONFIG_FILE" ]; then
        log_error "配置文件不存在: $CONFIG_FILE"
        exit 1
    fi
    
    log_info "使用配置文件: $CONFIG_FILE"
}

# 创建必要目录
create_directories() {
    mkdir -p "$LOG_DIR"
    mkdir -p "$(dirname "$PID_FILE")"
}

# 检查进程是否运行
is_running() {
    if [ -f "$PID_FILE" ]; then
        local pid=$(cat "$PID_FILE")
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            return 0  # 正在运行
        else
            # PID文件存在但进程不存在，清理PID文件
            rm -f "$PID_FILE"
            return 1  # 没有运行
        fi
    else
        return 1  # 没有运行
    fi
}

# 获取运行状态
get_status() {
    if is_running; then
        local pid=$(cat "$PID_FILE")
        echo "running (PID: $pid)"
        return 0
    else
        echo "stopped"
        return 1
    fi
}

# 停止已运行的进程
stop_existing() {
    if is_running; then
        if [ "$FORCE_START" = true ]; then
            log_warning "检测到程序正在运行，强制停止..."
            local pid=$(cat "$PID_FILE")
            kill -TERM "$pid" 2>/dev/null || true
            
            # 等待进程停止
            local count=0
            while kill -0 "$pid" 2>/dev/null && [ $count -lt 10 ]; do
                sleep 1
                count=$((count + 1))
            done
            
            # 如果还没停止，强制杀死
            if kill -0 "$pid" 2>/dev/null; then
                log_warning "进程未响应，强制杀死..."
                kill -KILL "$pid" 2>/dev/null || true
            fi
            
            rm -f "$PID_FILE"
            log_success "已停止运行中的进程"
        else
            log_error "程序已经在运行中 (PID: $(cat "$PID_FILE"))"
            log_info "使用 -f 选项强制启动，或先运行 ./stop.sh 停止服务"
            exit 1
        fi
    fi
}

# 启动程序
start_program() {
    log_info "启动MQTT服务器..."
    
    if [ "$DAEMON_MODE" = true ]; then
        # 后台模式
        log_info "以后台模式启动..."
        nohup "$EXECUTABLE" -c "$CONFIG_FILE" > "$LOG_DIR/mqtts.out" 2>&1 &
        local pid=$!
        echo $pid > "$PID_FILE"
        
        # 等待一会检查进程是否成功启动
        sleep 2
        if kill -0 "$pid" 2>/dev/null; then
            log_success "MQTT服务器已启动 (PID: $pid)"
            log_info "输出重定向到: $LOG_DIR/mqtts.out"
            log_info "使用 './stop.sh' 停止服务"
            log_info "使用 './status.sh' 查看状态"
        else
            log_error "程序启动失败"
            rm -f "$PID_FILE"
            exit 1
        fi
    else
        # 前台模式
        log_info "以前台模式启动..."
        log_info "按 Ctrl+C 停止服务"
        
        # 设置信号处理
        trap 'log_info "正在停止服务..."; rm -f "$PID_FILE"; exit 0' INT TERM
        
        # 记录PID
        echo $$ > "$PID_FILE"
        
        # 启动程序
        exec "$EXECUTABLE" -c "$CONFIG_FILE"
    fi
}

# 显示启动信息
show_start_info() {
    log_info "启动信息："
    echo "  程序名称: $PROGRAM_NAME"
    echo "  可执行文件: $EXECUTABLE"
    echo "  配置文件: $CONFIG_FILE"
    echo "  PID文件: $PID_FILE"
    echo "  日志目录: $LOG_DIR"
    echo "  运行模式: $([ "$DAEMON_MODE" = true ] && echo "后台" || echo "前台")"
}

# 主函数
main() {
    log_info "MQTT服务器启动脚本"
    
    check_executable
    check_config
    create_directories
    show_start_info
    
    # 检查当前状态
    local status=$(get_status)
    log_info "当前状态: $status"
    
    stop_existing
    start_program
}

# 执行主函数
main "$@" 