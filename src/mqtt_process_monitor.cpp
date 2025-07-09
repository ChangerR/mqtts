#include "mqtt_process_monitor.h"
#include <iomanip>
#include <sstream>
#include <ctime>
#include <sys/resource.h>
#include <unistd.h>
#include "logger.h"
#include "mqtt_memory_tags.h"

namespace mqtt {

ProcessMonitor::ProcessMonitor(GlobalSessionManager* session_manager, int interval_seconds)
    : session_manager_(session_manager), 
      interval_seconds_(interval_seconds),
      verbose_output_(false),
      running_(false),
      start_time_(std::chrono::steady_clock::now()),
      messages_sent_(0),
      messages_received_(0),
      bytes_sent_(0),
      bytes_received_(0)
{
    if (interval_seconds_ <= 0) {
        interval_seconds_ = 5;  // 默认5秒
    }
}

ProcessMonitor::~ProcessMonitor()
{
    stop();
}

int ProcessMonitor::start()
{
    if (running_.load()) {
        LOG_WARN("Process monitor is already running");
        return MQ_SUCCESS;
    }
    
    if (!session_manager_) {
        LOG_ERROR("Session manager is null, cannot start process monitor");
        return MQ_ERR_INVALID_ARGS;
    }
    
    running_.store(true);
    start_time_ = std::chrono::steady_clock::now();
    
    try {
        monitor_thread_.reset(new std::thread(&ProcessMonitor::monitor_loop, this));
        LOG_INFO("Process monitor started with {}s interval", interval_seconds_);
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to start process monitor thread: {}", e.what());
        running_.store(false);
        return MQ_ERR_INTERNAL;
    }
    
    return MQ_SUCCESS;
}

void ProcessMonitor::stop()
{
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    
    if (monitor_thread_ && monitor_thread_->joinable()) {
        monitor_thread_->join();
    }
    
    LOG_INFO("Process monitor stopped");
}

void ProcessMonitor::monitor_loop()
{
    LOG_INFO("Process monitor thread started");
    
    while (running_.load()) {
        try {
            print_status_once();
        } catch (const std::exception& e) {
            LOG_ERROR("Error in process monitor loop: {}", e.what());
        }
        
        // 使用更精确的睡眠控制
        for (int i = 0; i < interval_seconds_ * 10 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    LOG_INFO("Process monitor thread stopped");
}

int ProcessMonitor::get_process_stats(ProcessStats& stats)
{
    // 重置统计信息
    stats = ProcessStats();
    
    // 设置时间戳和运行时间
    stats.timestamp = std::chrono::steady_clock::now();
    stats.uptime = std::chrono::duration_cast<std::chrono::seconds>(
        stats.timestamp - start_time_);
    
    // 收集各种统计信息
    collect_memory_stats(stats);
    collect_connection_stats(stats);
    collect_topic_stats(stats);
    
    // 收集吞吐量统计
    stats.messages_sent = messages_sent_.load();
    stats.messages_received = messages_received_.load();
    stats.bytes_sent = bytes_sent_.load();
    stats.bytes_received = bytes_received_.load();
    
    return MQ_SUCCESS;
}

void ProcessMonitor::collect_memory_stats(ProcessStats& stats)
{
    auto& memory_manager = MQTTMemoryManager::get_instance();
    
    // 收集各标签的内存使用情况
    for (int i = 0; i < static_cast<int>(MQTTMemoryTag::MEM_TAG_COUNT); ++i) {
        MQTTMemoryTag tag = static_cast<MQTTMemoryTag>(i);
        std::string tag_name = get_memory_tag_str(tag);
        
        size_t tag_memory = memory_manager.get_tag_memory_usage(tag);
        stats.memory_by_tag[tag_name] = tag_memory;
        stats.total_memory_used += tag_memory;
    }
}

void ProcessMonitor::collect_connection_stats(ProcessStats& stats)
{
    if (!session_manager_) {
        return;
    }
    
    // 获取线程数量
    stats.total_threads = session_manager_->get_thread_count();
    
    // 获取总连接数和活跃连接数
    size_t total_handlers = 0;
    size_t active_handlers = 0;
    
    int result = session_manager_->get_total_handler_count(total_handlers);
    if (result == MQ_SUCCESS) {
        stats.total_connections = total_handlers;
        stats.active_connections = total_handlers;  // 假设所有连接都是活跃的
    }
    
    // 获取待处理消息数
    size_t pending_count = 0;
    result = session_manager_->get_total_pending_message_count(pending_count);
    if (result == MQ_SUCCESS) {
        stats.pending_messages = pending_count;
    }
}

void ProcessMonitor::collect_topic_stats(ProcessStats& stats)
{
    if (!session_manager_) {
        return;
    }
    
    // 获取topic tree统计信息
    size_t subscriber_count = 0;
    size_t node_count = 0;
    
    int result = session_manager_->get_topic_tree_stats(subscriber_count, node_count);
    if (result == MQ_SUCCESS) {
        stats.total_subscribers = subscriber_count;
        stats.total_topic_nodes = node_count;
    }
}

void ProcessMonitor::print_status_once()
{
    ProcessStats stats;
    int result = get_process_stats(stats);
    if (result != MQ_SUCCESS) {
        LOG_ERROR("Failed to get process stats: {}", result);
        return;
    }
    
    print_process_stats(stats);
}

void ProcessMonitor::print_process_stats(const ProcessStats& stats)
{
    std::stringstream ss;
    
    // 时间信息
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    ss << "\n" << std::string(80, '=') << "\n";
    ss << "MQTT Server Process Status - " << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    ss << " (Uptime: " << format_duration(stats.uptime) << ")\n";
    ss << std::string(80, '=') << "\n";
    
    // 连接统计
    ss << "CONNECTION STATS:\n";
    ss << "  Total Connections: " << stats.total_connections << "\n";
    ss << "  Active Connections: " << stats.active_connections << "\n";
    ss << "  Worker Threads: " << stats.total_threads << "\n";
    ss << "  Pending Messages: " << stats.pending_messages << "\n";
    ss << "\n";
    
    // Topic统计
    ss << "TOPIC STATS:\n";
    ss << "  Total Subscribers: " << stats.total_subscribers << "\n";
    ss << "  Topic Tree Nodes: " << stats.total_topic_nodes << "\n";
    ss << "\n";
    
    // 内存统计
    ss << "MEMORY STATS:\n";
    ss << "  Total Used: " << format_bytes(stats.total_memory_used) << "\n";
    
    if (verbose_output_) {
        ss << "  Memory by Tag:\n";
        for (const auto& pair : stats.memory_by_tag) {
            if (pair.second > 0) {
                ss << "    " << std::left << std::setw(20) << pair.first << ": " 
                   << format_bytes(pair.second) << "\n";
            }
        }
    }
    ss << "\n";
    
    // 吞吐量统计
    ss << "THROUGHPUT STATS:\n";
    ss << "  Messages Sent: " << stats.messages_sent << "\n";
    ss << "  Messages Received: " << stats.messages_received << "\n";
    ss << "  Bytes Sent: " << format_bytes(stats.bytes_sent) << "\n";
    ss << "  Bytes Received: " << format_bytes(stats.bytes_received) << "\n";
    
    // 计算平均吞吐量
    if (stats.uptime.count() > 0) {
        double msg_per_sec = static_cast<double>(stats.messages_sent + stats.messages_received) / stats.uptime.count();
        double bytes_per_sec = static_cast<double>(stats.bytes_sent + stats.bytes_received) / stats.uptime.count();
        ss << "  Average Throughput: " << std::fixed << std::setprecision(2) 
           << msg_per_sec << " msg/s, " << format_bytes(static_cast<size_t>(bytes_per_sec)) << "/s\n";
    }
    
    // 系统资源信息
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        ss << "\nSYSTEM RESOURCE STATS:\n";
        ss << "  Max RSS: " << format_bytes(usage.ru_maxrss * 1024) << "\n";
        ss << "  User CPU Time: " << usage.ru_utime.tv_sec << "." << std::setfill('0') << std::setw(6) << usage.ru_utime.tv_usec << "s\n";
        ss << "  System CPU Time: " << usage.ru_stime.tv_sec << "." << std::setfill('0') << std::setw(6) << usage.ru_stime.tv_usec << "s\n";
    }
    
    ss << std::string(80, '=') << "\n";
    
    // 输出到日志
    LOG_INFO("{}", ss.str());
}

std::string ProcessMonitor::format_bytes(size_t bytes)
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }
    
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
    return ss.str();
}

std::string ProcessMonitor::format_duration(std::chrono::seconds seconds)
{
    auto total_seconds = seconds.count();
    auto hours = total_seconds / 3600;
    auto minutes = (total_seconds % 3600) / 60;
    auto secs = total_seconds % 60;
    
    std::stringstream ss;
    ss << std::setfill('0') << std::setw(2) << hours << ":"
       << std::setw(2) << minutes << ":"
       << std::setw(2) << secs;
    return ss.str();
}

void ProcessMonitor::set_interval(int interval_seconds)
{
    if (interval_seconds > 0) {
        interval_seconds_ = interval_seconds;
        LOG_INFO("Process monitor interval changed to {}s", interval_seconds_);
    }
}

void ProcessMonitor::set_verbose_output(bool enabled)
{
    verbose_output_ = enabled;
    LOG_INFO("Process monitor verbose output {}", enabled ? "enabled" : "disabled");
}

void ProcessMonitor::add_sent_stats(size_t message_count, size_t bytes)
{
    messages_sent_.fetch_add(message_count);
    bytes_sent_.fetch_add(bytes);
}

void ProcessMonitor::add_received_stats(size_t message_count, size_t bytes)
{
    messages_received_.fetch_add(message_count);
    bytes_received_.fetch_add(bytes);
}

} // namespace mqtt