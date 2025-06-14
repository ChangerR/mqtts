#!/bin/bash

# MQTT服务器停止脚本
# 作者: MQTT Team
# 版本: 1.0

set -e  # 遇到错误立即退出

# 脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# 配置
PROGRAM_NAME="mqtts"
PID_FILE="$SCRIPT_DIR/mqtts.pid"
FORCE_STOP=false
TIMEOUT=30

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
    echo "MQTT服务器停止脚本"
    echo
    echo "用法: $0 [选项]"
    echo
    echo "选项:"
    echo "  -f, --force        强制停止（使用SIGKILL）"
    echo "  -t, --timeout NUM  等待超时时间（秒，默认30）"
    echo "  -h, --help         显示此帮助信息"
    echo
    echo "示例:"
    echo "  $0                 # 优雅停止"
    echo "  $0 -f              # 强制停止"
    echo "  $0 -t 60           # 等待60秒后强制停止"
}

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--force)
            FORCE_STOP=true
            shift
            ;;
        -t|--timeout)
            TIMEOUT="$2"
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

# 获取PID
get_pid() {
    if [ -f "$PID_FILE" ]; then
        cat "$PID_FILE"
    else
        echo ""
    fi
}

# 优雅停止进程
graceful_stop() {
    local pid=$1
    log_info "发送SIGTERM信号到进程 $pid"
    kill -TERM "$pid" 2>/dev/null || return 1
    
    # 等待进程停止
    local count=0
    while kill -0 "$pid" 2>/dev/null && [ $count -lt $TIMEOUT ]; do
        sleep 1
        count=$((count + 1))
        if [ $((count % 5)) -eq 0 ]; then
            log_info "等待进程停止... ($count/$TIMEOUT 秒)"
        fi
    done
    
    # 检查进程是否已停止
    if kill -0 "$pid" 2>/dev/null; then
        return 1  # 进程仍在运行
    else
        return 0  # 进程已停止
    fi
}

# 强制停止进程
force_stop() {
    local pid=$1
    log_warning "强制停止进程 $pid"
    kill -KILL "$pid" 2>/dev/null || return 1
    
    # 等待一会确认进程已被杀死
    sleep 2
    if kill -0 "$pid" 2>/dev/null; then
        return 1  # 进程仍在运行
    else
        return 0  # 进程已停止
    fi
}

# 清理PID文件
cleanup_pid_file() {
    if [ -f "$PID_FILE" ]; then
        rm -f "$PID_FILE"
        log_info "已清理PID文件: $PID_FILE"
    fi
}

# 查找并停止所有相关进程
stop_all_processes() {
    log_info "查找所有相关进程..."
    
    # 使用pgrep查找进程
    local pids=$(pgrep -f "$PROGRAM_NAME" 2>/dev/null || true)
    
    if [ -n "$pids" ]; then
        log_warning "发现额外的相关进程："
        for pid in $pids; do
            local cmd=$(ps -p $pid -o cmd= 2>/dev/null || echo "Unknown")
            echo "  PID $pid: $cmd"
        done
        
        if [ "$FORCE_STOP" = true ]; then
            log_info "强制停止所有相关进程..."
            for pid in $pids; do
                kill -KILL "$pid" 2>/dev/null || true
            done
        else
            log_info "优雅停止所有相关进程..."
            for pid in $pids; do
                kill -TERM "$pid" 2>/dev/null || true
            done
            
            # 等待进程停止
            sleep 3
            
            # 检查是否还有进程运行
            local remaining=$(pgrep -f "$PROGRAM_NAME" 2>/dev/null || true)
            if [ -n "$remaining" ]; then
                log_warning "仍有进程运行，强制停止..."
                for pid in $remaining; do
                    kill -KILL "$pid" 2>/dev/null || true
                done
            fi
        fi
    fi
}

# 显示停止信息
show_stop_info() {
    log_info "停止信息："
    echo "  程序名称: $PROGRAM_NAME"
    echo "  PID文件: $PID_FILE"
    echo "  超时时间: $TIMEOUT 秒"
    echo "  强制模式: $([ "$FORCE_STOP" = true ] && echo "是" || echo "否")"
}

# 主停止逻辑
stop_service() {
    if ! is_running; then
        log_warning "MQTT服务器未运行"
        cleanup_pid_file
        stop_all_processes  # 查找并清理可能的残留进程
        return 0
    fi
    
    local pid=$(get_pid)
    log_info "正在停止MQTT服务器 (PID: $pid)..."
    
    if [ "$FORCE_STOP" = true ]; then
        # 强制停止
        if force_stop "$pid"; then
            log_success "进程已强制停止"
            cleanup_pid_file
        else
            log_error "无法强制停止进程 $pid"
            return 1
        fi
    else
        # 优雅停止
        if graceful_stop "$pid"; then
            log_success "进程已优雅停止"
            cleanup_pid_file
        else
            log_warning "优雅停止超时，尝试强制停止..."
            if force_stop "$pid"; then
                log_success "进程已强制停止"
                cleanup_pid_file
            else
                log_error "无法停止进程 $pid"
                return 1
            fi
        fi
    fi
    
    # 最后检查是否还有相关进程
    stop_all_processes
}

# 主函数
main() {
    log_info "MQTT服务器停止脚本"
    
    show_stop_info
    
    if stop_service; then
        log_success "MQTT服务器已成功停止"
    else
        log_error "停止MQTT服务器失败"
        exit 1
    fi
}

# 执行主函数
main "$@" 