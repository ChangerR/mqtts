#!/bin/bash

# MQTT服务器状态查询脚本
# 作者: MQTT Team
# 版本: 1.0

# 脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# 配置
PROGRAM_NAME="mqtts"
EXECUTABLE="$SCRIPT_DIR/bin/$PROGRAM_NAME"
CONFIG_FILE="$SCRIPT_DIR/config/mqtts.yaml"
PID_FILE="$SCRIPT_DIR/mqtts.pid"
LOG_DIR="$SCRIPT_DIR/logs"
DETAILED=false

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
    echo "MQTT服务器状态查询脚本"
    echo
    echo "用法: $0 [选项]"
    echo
    echo "选项:"
    echo "  -d, --detailed     显示详细信息"
    echo "  -h, --help         显示此帮助信息"
    echo
    echo "示例:"
    echo "  $0                 # 显示基本状态"
    echo "  $0 -d              # 显示详细状态"
}

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--detailed)
            DETAILED=true
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

# 检查进程是否运行
is_running() {
    if [ -f "$PID_FILE" ]; then
        local pid=$(cat "$PID_FILE")
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            return 0  # 正在运行
        else
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

# 获取进程启动时间
get_start_time() {
    local pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        ps -o lstart= -p "$pid" 2>/dev/null | sed 's/^ *//' || echo "Unknown"
    else
        echo "Unknown"
    fi
}

# 获取进程运行时间
get_uptime() {
    local pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        ps -o etime= -p "$pid" 2>/dev/null | sed 's/^ *//' || echo "Unknown"
    else
        echo "Unknown"
    fi
}

# 获取内存使用情况
get_memory_usage() {
    local pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        local rss=$(ps -o rss= -p "$pid" 2>/dev/null | sed 's/^ *//')
        if [ -n "$rss" ]; then
            echo "${rss} KB"
        else
            echo "Unknown"
        fi
    else
        echo "Unknown"
    fi
}

# 获取CPU使用率
get_cpu_usage() {
    local pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        ps -o pcpu= -p "$pid" 2>/dev/null | sed 's/^ *//' || echo "Unknown"
    else
        echo "Unknown"
    fi
}

# 获取线程数
get_thread_count() {
    local pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        ps -o thcount= -p "$pid" 2>/dev/null | sed 's/^ *//' || echo "Unknown"
    else
        echo "Unknown"
    fi
}

# 检查端口占用
check_port() {
    local port=$(grep -A 10 "^server:" "$CONFIG_FILE" 2>/dev/null | grep "port:" | awk '{print $2}' | head -1)
    if [ -n "$port" ]; then
        if netstat -tuln 2>/dev/null | grep -q ":$port "; then
            echo "监听中 (端口 $port)"
        else
            echo "未监听 (端口 $port)"
        fi
    else
        echo "配置未找到"
    fi
}

# 获取日志文件信息
get_log_info() {
    if [ -d "$LOG_DIR" ]; then
        local log_files=$(find "$LOG_DIR" -name "*.log" -o -name "*.out" 2>/dev/null | wc -l)
        local log_size=$(du -sh "$LOG_DIR" 2>/dev/null | cut -f1 || echo "Unknown")
        echo "$log_files 个文件，总大小: $log_size"
    else
        echo "日志目录不存在"
    fi
}

# 显示基本状态
show_basic_status() {
    echo "==============================================="
    echo "              MQTT服务器状态                    "
    echo "==============================================="
    
    if is_running; then
        local pid=$(get_pid)
        log_success "服务状态: 运行中 (PID: $pid)"
        
        # 基本信息
        echo "运行时间: $(get_uptime "$pid")"
        echo "内存使用: $(get_memory_usage "$pid")"
        echo "端口状态: $(check_port)"
        
    else
        log_error "服务状态: 已停止"
        
        # 检查PID文件
        if [ -f "$PID_FILE" ]; then
            log_warning "PID文件存在但进程未运行: $PID_FILE"
        fi
    fi
    
    # 检查可执行文件
    if [ -f "$EXECUTABLE" ]; then
        echo "可执行文件: 存在"
    else
        log_warning "可执行文件: 不存在 ($EXECUTABLE)"
    fi
    
    # 检查配置文件
    if [ -f "$CONFIG_FILE" ]; then
        echo "配置文件: 存在"
    else
        log_warning "配置文件: 不存在 ($CONFIG_FILE)"
    fi
    
    echo "日志信息: $(get_log_info)"
}

# 显示详细状态
show_detailed_status() {
    show_basic_status
    
    if is_running; then
        local pid=$(get_pid)
        
        echo ""
        echo "==============================================="
        echo "                详细信息                       "
        echo "==============================================="
        
        echo "进程信息:"
        echo "  PID: $pid"
        echo "  启动时间: $(get_start_time "$pid")"
        echo "  运行时间: $(get_uptime "$pid")"
        echo "  CPU使用率: $(get_cpu_usage "$pid")%"
        echo "  内存使用: $(get_memory_usage "$pid")"
        echo "  线程数: $(get_thread_count "$pid")"
        
        # 进程详细信息
        echo ""
        echo "进程树:"
        if command -v pstree &> /dev/null; then
            pstree -p "$pid" 2>/dev/null || echo "  无法获取进程树"
        else
            ps --forest -o pid,ppid,cmd -p "$pid" 2>/dev/null || echo "  无法获取进程信息"
        fi
        
        # 文件描述符
        echo ""
        echo "文件描述符:"
        if [ -d "/proc/$pid/fd" ]; then
            local fd_count=$(ls -1 "/proc/$pid/fd" 2>/dev/null | wc -l)
            echo "  打开的文件描述符数: $fd_count"
        else
            echo "  无法获取文件描述符信息"
        fi
        
        # 网络连接
        echo ""
        echo "网络连接:"
        if command -v ss &> /dev/null; then
            ss -tuln | grep ":1883" || echo "  无监听端口"
        elif command -v netstat &> /dev/null; then
            netstat -tuln | grep ":1883" || echo "  无监听端口"
        else
            echo "  无法检查网络连接"
        fi
        
    fi
    
    # 系统资源
    echo ""
    echo "==============================================="
    echo "                系统资源                       "
    echo "==============================================="
    
    echo "系统负载:"
    if [ -f "/proc/loadavg" ]; then
        echo "  $(cat /proc/loadavg)"
    else
        echo "  无法获取系统负载"
    fi
    
    echo "内存使用:"
    if command -v free &> /dev/null; then
        free -h | grep -E "^Mem|^Swap" | while read line; do
            echo "  $line"
        done
    else
        echo "  无法获取内存信息"
    fi
    
    echo "磁盘使用:"
    if command -v df &> /dev/null; then
        df -h "$PROJECT_ROOT" | tail -1 | while read line; do
            echo "  $line"
        done
    else
        echo "  无法获取磁盘信息"
    fi
    
    # 日志文件
    echo ""
    echo "==============================================="
    echo "                日志文件                       "
    echo "==============================================="
    
    if [ -d "$LOG_DIR" ]; then
        find "$LOG_DIR" -name "*.log" -o -name "*.out" 2>/dev/null | while read logfile; do
            if [ -f "$logfile" ]; then
                local size=$(du -h "$logfile" 2>/dev/null | cut -f1)
                local mtime=$(stat -c %y "$logfile" 2>/dev/null | cut -d'.' -f1)
                echo "  $logfile ($size, 修改时间: $mtime)"
            fi
        done
    else
        echo "  日志目录不存在: $LOG_DIR"
    fi
    
    # 配置信息
    echo ""
    echo "==============================================="
    echo "                配置信息                       "
    echo "==============================================="
    
    if [ -f "$CONFIG_FILE" ]; then
        echo "服务配置:"
        grep -E "bind_address|port|max_connections" "$CONFIG_FILE" 2>/dev/null | while read line; do
            echo "  $line"
        done
        
        echo "日志配置:"
        grep -E "level|file_path" "$CONFIG_FILE" 2>/dev/null | while read line; do
            echo "  $line"
        done
    else
        echo "  配置文件不存在: $CONFIG_FILE"
    fi
}

# 检查相关进程
check_related_processes() {
    local pids=$(pgrep -f "$PROGRAM_NAME" 2>/dev/null || true)
    
    if [ -n "$pids" ]; then
        echo ""
        echo "==============================================="
        echo "              相关进程                         "
        echo "==============================================="
        
        for pid in $pids; do
            local cmd=$(ps -p $pid -o cmd= 2>/dev/null || echo "Unknown")
            local mem=$(ps -o rss= -p $pid 2>/dev/null | sed 's/^ *//' || echo "0")
            local cpu=$(ps -o pcpu= -p $pid 2>/dev/null | sed 's/^ *//' || echo "0")
            echo "  PID $pid: $cmd (内存: ${mem}KB, CPU: ${cpu}%)"
        done
    fi
}

# 主函数
main() {
    if [ "$DETAILED" = true ]; then
        show_detailed_status
    else
        show_basic_status
    fi
    
    check_related_processes
    
    echo ""
    echo "==============================================="
    
    # 返回适当的退出码
    if is_running; then
        exit 0
    else
        exit 1
    fi
}

# 执行主函数
main "$@" 