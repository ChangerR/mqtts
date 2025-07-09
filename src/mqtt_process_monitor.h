#ifndef MQTT_PROCESS_MONITOR_H
#define MQTT_PROCESS_MONITOR_H

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include "mqtt_allocator.h"
#include "mqtt_session_manager_v2.h"
#include "mqtt_topic_tree.h"

namespace mqtt {

/**
 * @brief 进程状态统计信息
 */
struct ProcessStats {
    // 连接统计
    size_t total_connections = 0;
    size_t active_connections = 0;
    size_t total_threads = 0;
    
    // 内存统计 (按标签分组)
    std::unordered_map<std::string, size_t> memory_by_tag;
    size_t total_memory_used = 0;
    
    // Topic统计
    size_t total_subscribers = 0;
    size_t total_topic_nodes = 0;
    size_t pending_messages = 0;
    
    // 系统统计
    std::chrono::steady_clock::time_point timestamp;
    std::chrono::seconds uptime;
    
    // 吞吐量统计
    size_t messages_sent = 0;
    size_t messages_received = 0;
    size_t bytes_sent = 0;
    size_t bytes_received = 0;
};

/**
 * @brief 进程监控器
 * 定期收集和打印进程运行状态信息
 */
class ProcessMonitor {
public:
    /**
     * @brief 构造函数
     * @param session_manager 全局会话管理器
     * @param interval_seconds 监控间隔（秒）
     */
    explicit ProcessMonitor(GlobalSessionManager* session_manager, int interval_seconds = 5);
    
    /**
     * @brief 析构函数
     */
    ~ProcessMonitor();
    
    // 禁止拷贝和赋值
    ProcessMonitor(const ProcessMonitor&) = delete;
    ProcessMonitor& operator=(const ProcessMonitor&) = delete;
    
    /**
     * @brief 启动监控
     * @return MQ_SUCCESS成功，其他值失败
     */
    int start();
    
    /**
     * @brief 停止监控
     */
    void stop();
    
    /**
     * @brief 获取当前进程状态
     * @param stats 输出参数：进程统计信息
     * @return MQ_SUCCESS成功，其他值失败
     */
    int get_process_stats(ProcessStats& stats);
    
    /**
     * @brief 手动打印一次状态
     */
    void print_status_once();
    
    /**
     * @brief 设置监控间隔
     * @param interval_seconds 间隔秒数
     */
    void set_interval(int interval_seconds);
    
    /**
     * @brief 设置是否启用详细输出
     * @param enabled 是否启用
     */
    void set_verbose_output(bool enabled);
    
    /**
     * @brief 增加消息发送统计
     * @param message_count 消息数量
     * @param bytes 字节数
     */
    void add_sent_stats(size_t message_count, size_t bytes);
    
    /**
     * @brief 增加消息接收统计
     * @param message_count 消息数量
     * @param bytes 字节数
     */
    void add_received_stats(size_t message_count, size_t bytes);

private:
    GlobalSessionManager* session_manager_;
    int interval_seconds_;
    bool verbose_output_;
    
    std::atomic<bool> running_;
    std::unique_ptr<std::thread> monitor_thread_;
    std::chrono::steady_clock::time_point start_time_;
    
    // 统计计数器
    std::atomic<size_t> messages_sent_;
    std::atomic<size_t> messages_received_;
    std::atomic<size_t> bytes_sent_;
    std::atomic<size_t> bytes_received_;
    
    /**
     * @brief 监控线程主函数
     */
    void monitor_loop();
    
    /**
     * @brief 收集内存统计信息
     * @param stats 输出参数：进程统计信息
     */
    void collect_memory_stats(ProcessStats& stats);
    
    /**
     * @brief 收集连接统计信息
     * @param stats 输出参数：进程统计信息
     */
    void collect_connection_stats(ProcessStats& stats);
    
    /**
     * @brief 收集Topic统计信息
     * @param stats 输出参数：进程统计信息
     */
    void collect_topic_stats(ProcessStats& stats);
    
    /**
     * @brief 打印进程状态
     * @param stats 进程统计信息
     */
    void print_process_stats(const ProcessStats& stats);
    
    /**
     * @brief 格式化字节数
     * @param bytes 字节数
     * @return 格式化后的字符串
     */
    std::string format_bytes(size_t bytes);
    
    /**
     * @brief 格式化时间间隔
     * @param seconds 秒数
     * @return 格式化后的字符串
     */
    std::string format_duration(std::chrono::seconds seconds);
};

} // namespace mqtt

#endif // MQTT_PROCESS_MONITOR_H