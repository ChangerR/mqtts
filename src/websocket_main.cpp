#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_config.h"
#include "mqtt_session_manager_v2.h"
#include "websocket_server.h"
#include "websocket_mqtt_bridge.h"

static std::unique_ptr<websocket::WebSocketServer> g_websocket_server;
static std::unique_ptr<websocket::WebSocketMQTTBridge> g_mqtt_bridge;
static std::unique_ptr<mqtt::GlobalSessionManager> g_session_manager;
static volatile sig_atomic_t g_shutdown_requested = 0;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_shutdown_requested = 1;
        std::cout << "\n收到信号 " << signal << "，正在关闭服务器..." << std::endl;
        
        if (g_websocket_server) {
            g_websocket_server->stop();
        }
    }
}

void setup_signal_handlers() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
}

void print_usage(const char* program_name) {
    std::cout << "用法: " << program_name << " [选项]" << std::endl;
    std::cout << "选项:" << std::endl;
    std::cout << "  -c, --config <文件>    配置文件路径 (默认: websocket.yaml)" << std::endl;
    std::cout << "  -h, --help            显示帮助信息" << std::endl;
    std::cout << "  -v, --version         显示版本信息" << std::endl;
    std::cout << "  -d, --daemon          以守护进程模式运行" << std::endl;
}

void print_version() {
    std::cout << "WebSocket MQTT Bridge Server v1.0.0" << std::endl;
    std::cout << "兼容 MQTT v3.1.1 和 v5.0" << std::endl;
}

int load_config(const std::string& config_file, mqtt::WebSocketConfig& ws_config,
                mqtt::MemoryConfig& mem_config) {
    try {
        // 这里应该使用YAML解析器加载配置
        // 为了演示，我们使用默认配置
        ws_config.enabled = true;
        ws_config.bind_address = "127.0.0.1";
        ws_config.port = 8080;
        ws_config.max_connections = 10000;
        ws_config.thread_count = 2;
        ws_config.max_frame_size = 1048576;
        ws_config.max_message_size = 10485760;
        ws_config.handshake_timeout = 10;
        ws_config.ping_interval = 30;
        ws_config.pong_timeout = 10;
        ws_config.message_format = "json";
        
        mem_config.client_max_size = 1048576;
        
        std::cout << "配置加载成功: " << config_file << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "配置加载失败: " << e.what() << std::endl;
        return -1;
    }
}

void print_server_info(const websocket::WebSocketConfig& config) {
    std::cout << "===========================================" << std::endl;
    std::cout << "WebSocket MQTT Bridge Server" << std::endl;
    std::cout << "===========================================" << std::endl;
    std::cout << "监听地址: " << config.bind_address << ":" << config.port << std::endl;
    std::cout << "最大连接数: " << config.max_connections << std::endl;
    std::cout << "工作线程数: " << config.thread_count << std::endl;
    std::cout << "最大帧大小: " << config.max_frame_size << " bytes" << std::endl;
    std::cout << "最大消息大小: " << config.max_message_size << " bytes" << std::endl;
    std::cout << "消息格式: " << config.message_format << std::endl;
    std::cout << "===========================================" << std::endl;
}

void print_statistics() {
    if (!g_websocket_server || !g_mqtt_bridge) {
        return;
    }
    
    auto& server_stats = g_websocket_server->get_statistics();
    auto& bridge_stats = g_mqtt_bridge->get_statistics();
    
    std::cout << "\n===========================================" << std::endl;
    std::cout << "服务器统计信息" << std::endl;
    std::cout << "===========================================" << std::endl;
    std::cout << "活跃连接数: " << server_stats.active_connections << std::endl;
    std::cout << "总连接数: " << server_stats.total_connections << std::endl;
    std::cout << "总消息数: " << server_stats.total_messages << std::endl;
    std::cout << "发送字节数: " << server_stats.total_bytes_sent << std::endl;
    std::cout << "接收字节数: " << server_stats.total_bytes_received << std::endl;
    std::cout << "握手错误: " << server_stats.handshake_errors << std::endl;
    std::cout << "协议错误: " << server_stats.protocol_errors << std::endl;
    std::cout << "-------------------------------------------" << std::endl;
    std::cout << "WebSocket消息 (接收): " << bridge_stats.websocket_messages_received << std::endl;
    std::cout << "WebSocket消息 (发送): " << bridge_stats.websocket_messages_sent << std::endl;
    std::cout << "MQTT消息 (接收): " << bridge_stats.mqtt_messages_received << std::endl;
    std::cout << "MQTT消息 (发送): " << bridge_stats.mqtt_messages_sent << std::endl;
    std::cout << "翻译错误: " << bridge_stats.translation_errors << std::endl;
    std::cout << "活跃订阅: " << bridge_stats.active_subscriptions << std::endl;
    std::cout << "===========================================" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string config_file = "websocket.yaml";
    bool daemon_mode = false;
    
    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            print_version();
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                config_file = argv[++i];
            } else {
                std::cerr << "错误: " << arg << " 需要一个参数" << std::endl;
                return 1;
            }
        } else if (arg == "-d" || arg == "--daemon") {
            daemon_mode = true;
        } else {
            std::cerr << "错误: 未知选项 " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // 加载配置
    websocket::WebSocketConfig ws_config;
    mqtt::MemoryConfig mem_config;
    
    if (load_config(config_file, ws_config, mem_config) != 0) {
        return 1;
    }
    
    // 检查WebSocket是否启用
    if (!ws_config.enabled) {
        std::cout << "WebSocket服务器未启用，退出" << std::endl;
        return 0;
    }
    
    // 设置信号处理器
    setup_signal_handlers();
    
    // 初始化日志系统
    Logger::initialize("websocket_server", "logs/websocket.log");
    
    print_server_info(ws_config);
    
    try {
        // 创建全局会话管理器
        g_session_manager = std::make_unique<mqtt::GlobalSessionManager>();
        
        // 预注册线程
        int ret = g_session_manager->pre_register_threads(ws_config.thread_count, ws_config.max_connections);
        if (ret != 0) {
            std::cerr << "预注册线程失败: " << ret << std::endl;
            return 1;
        }
        
        // 创建WebSocket服务器
        g_websocket_server = std::make_unique<websocket::WebSocketServer>(ws_config, mem_config);
        g_websocket_server->set_session_manager(g_session_manager.get());
        
        // 创建MQTT桥接器
        MQTTAllocator* main_allocator = g_session_manager->get_allocator();
        g_mqtt_bridge = std::make_unique<websocket::WebSocketMQTTBridge>(main_allocator);
        
        ret = g_mqtt_bridge->init(g_session_manager.get());
        if (ret != 0) {
            std::cerr << "MQTT桥接器初始化失败: " << ret << std::endl;
            return 1;
        }
        
        // 设置消息格式
        if (ws_config.message_format == "json") {
            g_mqtt_bridge->set_message_format(websocket::WebSocketMQTTBridge::MessageFormat::JSON);
        } else if (ws_config.message_format == "mqtt_packet") {
            g_mqtt_bridge->set_message_format(websocket::WebSocketMQTTBridge::MessageFormat::MQTT_PACKET);
        } else if (ws_config.message_format == "text_protocol") {
            g_mqtt_bridge->set_message_format(websocket::WebSocketMQTTBridge::MessageFormat::TEXT_PROTOCOL);
        }
        
        // 启动服务器
        ret = g_websocket_server->start();
        if (ret != 0) {
            std::cerr << "服务器启动失败: " << ret << std::endl;
            return 1;
        }
        
        std::cout << "WebSocket服务器启动成功，按 Ctrl+C 停止服务器" << std::endl;
        
        // 主循环
        while (!g_shutdown_requested && g_websocket_server->is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // 定期打印统计信息
            static int stats_counter = 0;
            if (++stats_counter >= 30) { // 每30秒打印一次
                print_statistics();
                stats_counter = 0;
            }
        }
        
        std::cout << "正在停止服务器..." << std::endl;
        
        // 停止服务器
        if (g_websocket_server) {
            g_websocket_server->stop();
        }
        
        // 最终统计信息
        print_statistics();
        
    } catch (const std::exception& e) {
        std::cerr << "服务器运行异常: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "服务器已关闭" << std::endl;
    return 0;
}