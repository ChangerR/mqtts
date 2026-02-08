#!/bin/bash

# MQTT Router Service Startup Script
# Version: 1.0.0

# 脚本配置
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
ROUTER_BIN="$PROJECT_ROOT/bin/mqtt_router"
DEFAULT_CONFIG="$PROJECT_ROOT/mqtt_router.yaml"
PID_FILE="/tmp/mqtt_router.pid"
LOG_FILE="/tmp/mqtt_router.log"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印带颜色的消息
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 显示帮助信息
show_help() {
    cat << EOF
MQTT Router Service Management Script

Usage: $0 [COMMAND] [OPTIONS]

Commands:
    start       Start the router service
    stop        Stop the router service
    restart     Restart the router service
    status      Show router service status
    logs        Show router service logs
    build       Build the router service
    clean       Clean up pid and log files

Options:
    -c, --config FILE   Configuration file (default: $DEFAULT_CONFIG)
    -d, --daemon        Run as daemon (background)
    -h, --help          Show this help message
    -v, --verbose       Verbose output

Examples:
    $0 start                    # Start with default config
    $0 start -c custom.yaml     # Start with custom config
    $0 start -d                 # Start as daemon
    $0 stop                     # Stop the service
    $0 status                   # Check status
    $0 logs                     # Show logs

EOF
}

# 检查路由器进程是否运行
is_running() {
    if [ -f "$PID_FILE" ]; then
        local pid=$(cat "$PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            return 0
        else
            rm -f "$PID_FILE"
            return 1
        fi
    fi
    return 1
}

# 获取进程PID
get_pid() {
    if [ -f "$PID_FILE" ]; then
        cat "$PID_FILE"
    else
        echo ""
    fi
}

# 等待进程启动
wait_for_start() {
    local timeout=30
    local count=0
    
    while [ $count -lt $timeout ]; do
        if is_running; then
            return 0
        fi
        sleep 1
        count=$((count + 1))
    done
    return 1
}

# 等待进程停止
wait_for_stop() {
    local timeout=30
    local count=0
    
    while [ $count -lt $timeout ]; do
        if ! is_running; then
            return 0
        fi
        sleep 1
        count=$((count + 1))
    done
    return 1
}

# 创建必要的目录
create_directories() {
    local config_file="$1"
    
    # 创建data目录（用于redo log和snapshot）
    mkdir -p "$PROJECT_ROOT/data"
    mkdir -p "$PROJECT_ROOT/logs"
    
    # 从配置文件中读取路径并创建目录
    if [ -f "$config_file" ]; then
        # 提取redo log和snapshot路径
        local redo_log_path=$(grep "redo_log_path:" "$config_file" | sed 's/.*: *"//' | sed 's/".*//' | sed 's/\.\///')
        local snapshot_path=$(grep "snapshot_path:" "$config_file" | sed 's/.*: *"//' | sed 's/".*//' | sed 's/\.\///')
        
        if [ ! -z "$redo_log_path" ]; then
            mkdir -p "$(dirname "$PROJECT_ROOT/$redo_log_path")"
        fi
        
        if [ ! -z "$snapshot_path" ]; then
            mkdir -p "$(dirname "$PROJECT_ROOT/$snapshot_path")"
        fi
    fi
}

# 启动路由器服务
start_router() {
    local config_file="$1"
    local daemon_mode="$2"
    local verbose="$3"
    
    if is_running; then
        local pid=$(get_pid)
        print_warning "Router service is already running (PID: $pid)"
        return 1
    fi
    
    # 检查配置文件
    if [ ! -f "$config_file" ]; then
        print_error "Configuration file not found: $config_file"
        return 1
    fi
    
    # 检查可执行文件
    if [ ! -f "$ROUTER_BIN" ]; then
        print_error "Router binary not found: $ROUTER_BIN"
        print_info "Please run: $0 build"
        return 1
    fi
    
    if [ ! -x "$ROUTER_BIN" ]; then
        print_error "Router binary is not executable: $ROUTER_BIN"
        return 1
    fi
    
    # 创建必要的目录
    create_directories "$config_file"
    
    print_info "Starting MQTT Router Service..."
    print_info "Config file: $config_file"
    
    if [ "$daemon_mode" = "true" ]; then
        # 后台运行
        nohup "$ROUTER_BIN" -c "$config_file" > "$LOG_FILE" 2>&1 &
        local pid=$!
        echo $pid > "$PID_FILE"
        
        print_info "Starting in daemon mode (PID: $pid)"
        
        if wait_for_start; then
            print_success "Router service started successfully"
            print_info "PID: $(get_pid)"
            print_info "Logs: $LOG_FILE"
        else
            print_error "Failed to start router service within timeout"
            rm -f "$PID_FILE"
            return 1
        fi
    else
        # 前台运行
        print_info "Starting in foreground mode (Ctrl+C to stop)"
        "$ROUTER_BIN" -c "$config_file"
    fi
}

# 停止路由器服务
stop_router() {
    if ! is_running; then
        print_warning "Router service is not running"
        return 1
    fi
    
    local pid=$(get_pid)
    print_info "Stopping MQTT Router Service (PID: $pid)..."
    
    # 发送TERM信号
    kill -TERM "$pid" 2>/dev/null
    
    if wait_for_stop; then
        print_success "Router service stopped successfully"
        rm -f "$PID_FILE"
    else
        print_warning "Router service did not stop gracefully, forcing shutdown..."
        kill -KILL "$pid" 2>/dev/null
        rm -f "$PID_FILE"
        
        if wait_for_stop; then
            print_success "Router service forcefully stopped"
        else
            print_error "Failed to stop router service"
            return 1
        fi
    fi
}

# 重启路由器服务
restart_router() {
    local config_file="$1"
    local daemon_mode="$2"
    local verbose="$3"
    
    print_info "Restarting MQTT Router Service..."
    
    if is_running; then
        stop_router
    fi
    
    sleep 2
    start_router "$config_file" "$daemon_mode" "$verbose"
}

# 显示服务状态
show_status() {
    if is_running; then
        local pid=$(get_pid)
        print_success "Router service is running (PID: $pid)"
        
        # 显示进程信息
        if command -v ps >/dev/null 2>&1; then
            echo
            print_info "Process information:"
            ps -p "$pid" -o pid,ppid,cpu,mem,etime,cmd 2>/dev/null || true
        fi
        
        # 显示网络监听
        if command -v netstat >/dev/null 2>&1; then
            echo
            print_info "Network connections:"
            netstat -tlnp 2>/dev/null | grep "$pid" || true
        elif command -v ss >/dev/null 2>&1; then
            echo
            print_info "Network connections:"
            ss -tlnp 2>/dev/null | grep "$pid" || true
        fi
        
        return 0
    else
        print_warning "Router service is not running"
        return 1
    fi
}

# 显示日志
show_logs() {
    local lines="${1:-50}"
    
    if [ -f "$LOG_FILE" ]; then
        print_info "Showing last $lines lines of router logs:"
        echo "----------------------------------------"
        tail -n "$lines" "$LOG_FILE"
    else
        print_warning "Log file not found: $LOG_FILE"
    fi
}

# 构建路由器服务
build_router() {
    print_info "Building MQTT Router Service..."
    
    cd "$PROJECT_ROOT"
    
    if [ ! -d "build" ]; then
        mkdir build
    fi
    
    cd build
    
    if cmake .. && make -j$(nproc) mqtt_router; then
        print_success "Router service built successfully"
        
        # 确保bin目录存在
        mkdir -p "$PROJECT_ROOT/bin"
        
        # 复制可执行文件
        if [ -f "mqtt_router" ]; then
            cp mqtt_router "$PROJECT_ROOT/bin/"
            chmod +x "$PROJECT_ROOT/bin/mqtt_router"
            print_success "Router binary installed to: $PROJECT_ROOT/bin/mqtt_router"
        fi
    else
        print_error "Failed to build router service"
        return 1
    fi
}

# 清理文件
clean_files() {
    print_info "Cleaning up router service files..."
    
    if is_running; then
        print_error "Cannot clean while router service is running. Please stop it first."
        return 1
    fi
    
    rm -f "$PID_FILE"
    rm -f "$LOG_FILE"
    
    print_success "Cleanup completed"
}

# 主函数
main() {
    local command=""
    local config_file="$DEFAULT_CONFIG"
    local daemon_mode="false"
    local verbose="false"
    
    # 解析参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            start|stop|restart|status|logs|build|clean)
                command="$1"
                shift
                ;;
            -c|--config)
                config_file="$2"
                shift 2
                ;;
            -d|--daemon)
                daemon_mode="true"
                shift
                ;;
            -v|--verbose)
                verbose="true"
                shift
                ;;
            -h|--help)
                show_help
                exit 0
                ;;
            *)
                print_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done
    
    # 检查命令
    if [ -z "$command" ]; then
        print_error "No command specified"
        show_help
        exit 1
    fi
    
    # 执行命令
    case $command in
        start)
            start_router "$config_file" "$daemon_mode" "$verbose"
            ;;
        stop)
            stop_router
            ;;
        restart)
            restart_router "$config_file" "$daemon_mode" "$verbose"
            ;;
        status)
            show_status
            ;;
        logs)
            show_logs
            ;;
        build)
            build_router
            ;;
        clean)
            clean_files
            ;;
        *)
            print_error "Unknown command: $command"
            show_help
            exit 1
            ;;
    esac
}

# 捕获信号
trap 'print_info "Script interrupted"; exit 130' INT
trap 'print_info "Script terminated"; exit 143' TERM

# 运行主函数
main "$@"