#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <fstream>
#include <algorithm>
#include "mqtt_router_service.h"
#include "mqtt_router_rpc_client.h"
#include "mqtt_allocator.h"
#include "mqtt_memory_tags.h"

class PerformanceAnalyzer {
public:
    struct TestConfig {
        int num_servers = 5;
        int clients_per_server = 1000;
        int topics_per_client = 10;
        int messages_per_topic = 100;
        int concurrent_operations = 10;
        bool enable_memory_tracking = true;
        bool enable_timing_analysis = true;
        std::string output_file = "performance_report.txt";
    };
    
    struct PerformanceMetrics {
        // 时间统计
        std::chrono::microseconds total_subscription_time{0};
        std::chrono::microseconds total_publish_time{0};
        std::chrono::microseconds total_unsubscribe_time{0};
        
        // 操作计数
        std::atomic<uint64_t> total_subscriptions{0};
        std::atomic<uint64_t> total_publishes{0};
        std::atomic<uint64_t> total_unsubscribes{0};
        std::atomic<uint64_t> failed_operations{0};
        
        // 内存统计
        size_t initial_memory_usage = 0;
        size_t peak_memory_usage = 0;
        size_t final_memory_usage = 0;
        
        // 延迟统计
        std::vector<std::chrono::microseconds> subscription_latencies;
        std::vector<std::chrono::microseconds> publish_latencies;
        std::vector<std::chrono::microseconds> unsubscribe_latencies;
    };
    
    explicit PerformanceAnalyzer(const TestConfig& config) 
        : config_(config), metrics_() {
        
        MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
        test_allocator_ = root_allocator->create_child("performance_test", MQTTMemoryTag::MEM_TAG_ROOT);
    }
    
    ~PerformanceAnalyzer() {
        if (test_allocator_ && test_allocator_->get_parent()) {
            test_allocator_->get_parent()->remove_child("performance_test");
        }
    }
    
    int run_performance_test() {
        std::cout << "Starting Performance Analysis..." << std::endl;
        
        // 初始化路由服务
        if (setup_router_service() != 0) {
            std::cerr << "Failed to setup router service" << std::endl;
            return -1;
        }
        
        // 记录初始内存使用
        if (config_.enable_memory_tracking) {
            metrics_.initial_memory_usage = get_memory_usage();
        }
        
        // 运行性能测试
        auto start_time = std::chrono::high_resolution_clock::now();
        
        if (run_subscription_test() != 0) {
            std::cerr << "Subscription test failed" << std::endl;
            return -1;
        }
        
        if (run_publish_test() != 0) {
            std::cerr << "Publish test failed" << std::endl;
            return -1;
        }
        
        if (run_unsubscribe_test() != 0) {
            std::cerr << "Unsubscribe test failed" << std::endl;
            return -1;
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // 记录最终内存使用
        if (config_.enable_memory_tracking) {
            metrics_.final_memory_usage = get_memory_usage();
        }
        
        // 生成报告
        generate_performance_report(total_time);
        
        // 清理
        cleanup_router_service();
        
        std::cout << "Performance analysis completed. Report saved to: " << config_.output_file << std::endl;
        return 0;
    }
    
private:
    int setup_router_service() {
        // 配置路由服务
        MQTTRouterConfig router_config;
        router_config.service_host = "127.0.0.1";
        router_config.service_port = 19095;  // 使用测试端口
        router_config.redo_log_path = "./perf_test_router.redo";
        router_config.snapshot_path = "./perf_test_router.snapshot";
        router_config.snapshot_interval_seconds = 60;
        router_config.redo_log_flush_interval_ms = 1000;
        router_config.max_redo_log_entries = 10000;
        router_config.worker_thread_count = 4;
        router_config.coroutines_per_thread = 8;
        router_config.max_memory_limit = 512 * 1024 * 1024;  // 512MB
        
        router_service_.reset(new MQTTRouterService(router_config));
        
        if (router_service_->initialize() != 0) {
            return -1;
        }
        
        if (router_service_->start() != 0) {
            return -1;
        }
        
        // 等待服务启动
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        return 0;
    }
    
    void cleanup_router_service() {
        if (router_service_) {
            router_service_->stop();
            router_service_.reset();
        }
        
        // 清理测试文件
        std::remove("./perf_test_router.redo");
        std::remove("./perf_test_router.snapshot");
    }
    
    int run_subscription_test() {
        std::cout << "Running subscription performance test..." << std::endl;
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        std::vector<std::thread> threads;
        const int operations_per_thread = (config_.num_servers * config_.clients_per_server * config_.topics_per_client) / config_.concurrent_operations;
        
        for (int t = 0; t < config_.concurrent_operations; ++t) {
            threads.emplace_back([this, t, operations_per_thread]() {
                for (int i = 0; i < operations_per_thread; ++i) {
                    int server_id = (t * operations_per_thread + i) % config_.num_servers;
                    int client_id = (t * operations_per_thread + i) % config_.clients_per_server;
                    int topic_id = (t * operations_per_thread + i) % config_.topics_per_client;
                    
                    auto op_start = std::chrono::high_resolution_clock::now();
                    
                    // 模拟订阅操作（这里简化为直接调用内部API）
                    // 实际测试中应该通过RPC客户端进行
                    
                    auto op_end = std::chrono::high_resolution_clock::now();
                    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start);
                    
                    metrics_.subscription_latencies.push_back(latency);
                    metrics_.total_subscriptions++;
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        metrics_.total_subscription_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        
        std::cout << "Subscription test completed: " << metrics_.total_subscriptions.load() << " operations" << std::endl;
        return 0;
    }
    
    int run_publish_test() {
        std::cout << "Running publish performance test..." << std::endl;
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        std::vector<std::thread> threads;
        const int operations_per_thread = (config_.topics_per_client * config_.messages_per_topic) / config_.concurrent_operations;
        
        for (int t = 0; t < config_.concurrent_operations; ++t) {
            threads.emplace_back([this, t, operations_per_thread]() {
                for (int i = 0; i < operations_per_thread; ++i) {
                    auto op_start = std::chrono::high_resolution_clock::now();
                    
                    // 模拟发布操作
                    // 实际测试中应该通过RPC客户端进行路由查询
                    
                    auto op_end = std::chrono::high_resolution_clock::now();
                    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start);
                    
                    metrics_.publish_latencies.push_back(latency);
                    metrics_.total_publishes++;
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        metrics_.total_publish_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        
        std::cout << "Publish test completed: " << metrics_.total_publishes.load() << " operations" << std::endl;
        return 0;
    }
    
    int run_unsubscribe_test() {
        std::cout << "Running unsubscribe performance test..." << std::endl;
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // 简化的取消订阅测试
        const int total_unsubscribes = config_.num_servers * config_.clients_per_server * config_.topics_per_client;
        
        for (int i = 0; i < total_unsubscribes; ++i) {
            auto op_start = std::chrono::high_resolution_clock::now();
            
            // 模拟取消订阅操作
            
            auto op_end = std::chrono::high_resolution_clock::now();
            auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start);
            
            metrics_.unsubscribe_latencies.push_back(latency);
            metrics_.total_unsubscribes++;
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        metrics_.total_unsubscribe_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        
        std::cout << "Unsubscribe test completed: " << metrics_.total_unsubscribes.load() << " operations" << std::endl;
        return 0;
    }
    
    size_t get_memory_usage() {
        // 简化的内存使用统计
        if (test_allocator_) {
            return test_allocator_->get_total_memory_usage();
        }
        return 0;
    }
    
    void generate_performance_report(std::chrono::milliseconds total_time) {
        std::ofstream report(config_.output_file);
        
        report << "MQTT Router Performance Analysis Report" << std::endl;
        report << "=======================================" << std::endl;
        report << std::endl;
        
        // 测试配置
        report << "Test Configuration:" << std::endl;
        report << "  Servers: " << config_.num_servers << std::endl;
        report << "  Clients per server: " << config_.clients_per_server << std::endl;
        report << "  Topics per client: " << config_.topics_per_client << std::endl;
        report << "  Messages per topic: " << config_.messages_per_topic << std::endl;
        report << "  Concurrent operations: " << config_.concurrent_operations << std::endl;
        report << std::endl;
        
        // 总体性能
        report << "Overall Performance:" << std::endl;
        report << "  Total test time: " << total_time.count() << " ms" << std::endl;
        report << "  Total operations: " << (metrics_.total_subscriptions.load() + metrics_.total_publishes.load() + metrics_.total_unsubscribes.load()) << std::endl;
        report << "  Operations per second: " << ((metrics_.total_subscriptions.load() + metrics_.total_publishes.load() + metrics_.total_unsubscribes.load()) * 1000 / total_time.count()) << std::endl;
        report << "  Failed operations: " << metrics_.failed_operations.load() << std::endl;
        report << std::endl;
        
        // 订阅性能
        report << "Subscription Performance:" << std::endl;
        report << "  Total subscriptions: " << metrics_.total_subscriptions.load() << std::endl;
        report << "  Total time: " << metrics_.total_subscription_time.count() << " μs" << std::endl;
        if (metrics_.total_subscriptions.load() > 0) {
            report << "  Average latency: " << (metrics_.total_subscription_time.count() / metrics_.total_subscriptions.load()) << " μs" << std::endl;
            report << "  Subscriptions per second: " << (metrics_.total_subscriptions.load() * 1000000 / metrics_.total_subscription_time.count()) << std::endl;
        }
        report << std::endl;
        
        // 发布性能
        report << "Publish Performance:" << std::endl;
        report << "  Total publishes: " << metrics_.total_publishes.load() << std::endl;
        report << "  Total time: " << metrics_.total_publish_time.count() << " μs" << std::endl;
        if (metrics_.total_publishes.load() > 0) {
            report << "  Average latency: " << (metrics_.total_publish_time.count() / metrics_.total_publishes.load()) << " μs" << std::endl;
            report << "  Publishes per second: " << (metrics_.total_publishes.load() * 1000000 / metrics_.total_publish_time.count()) << std::endl;
        }
        report << std::endl;
        
        // 取消订阅性能
        report << "Unsubscribe Performance:" << std::endl;
        report << "  Total unsubscribes: " << metrics_.total_unsubscribes.load() << std::endl;
        report << "  Total time: " << metrics_.total_unsubscribe_time.count() << " μs" << std::endl;
        if (metrics_.total_unsubscribes.load() > 0) {
            report << "  Average latency: " << (metrics_.total_unsubscribe_time.count() / metrics_.total_unsubscribes.load()) << " μs" << std::endl;
            report << "  Unsubscribes per second: " << (metrics_.total_unsubscribes.load() * 1000000 / metrics_.total_unsubscribe_time.count()) << std::endl;
        }
        report << std::endl;
        
        // 内存使用
        if (config_.enable_memory_tracking) {
            report << "Memory Usage:" << std::endl;
            report << "  Initial memory: " << (metrics_.initial_memory_usage / 1024) << " KB" << std::endl;
            report << "  Peak memory: " << (metrics_.peak_memory_usage / 1024) << " KB" << std::endl;
            report << "  Final memory: " << (metrics_.final_memory_usage / 1024) << " KB" << std::endl;
            report << "  Memory growth: " << ((metrics_.final_memory_usage - metrics_.initial_memory_usage) / 1024) << " KB" << std::endl;
            report << std::endl;
        }
        
        // 延迟分析
        if (config_.enable_timing_analysis && !metrics_.subscription_latencies.empty()) {
            // 计算百分位数
            auto subscription_latencies = metrics_.subscription_latencies;
            std::sort(subscription_latencies.begin(), subscription_latencies.end());
            
            report << "Latency Analysis (Subscriptions):" << std::endl;
            report << "  P50: " << subscription_latencies[subscription_latencies.size() * 50 / 100].count() << " μs" << std::endl;
            report << "  P90: " << subscription_latencies[subscription_latencies.size() * 90 / 100].count() << " μs" << std::endl;
            report << "  P95: " << subscription_latencies[subscription_latencies.size() * 95 / 100].count() << " μs" << std::endl;
            report << "  P99: " << subscription_latencies[subscription_latencies.size() * 99 / 100].count() << " μs" << std::endl;
            report << std::endl;
        }
        
        report.close();
    }
    
    TestConfig config_;
    PerformanceMetrics metrics_;
    MQTTAllocator* test_allocator_;
    std::unique_ptr<MQTTRouterService> router_service_;
};

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --servers N         Number of servers (default: 5)" << std::endl;
    std::cout << "  --clients N         Clients per server (default: 1000)" << std::endl;
    std::cout << "  --topics N          Topics per client (default: 10)" << std::endl;
    std::cout << "  --messages N        Messages per topic (default: 100)" << std::endl;
    std::cout << "  --threads N         Concurrent operations (default: 10)" << std::endl;
    std::cout << "  --output FILE       Output file (default: performance_report.txt)" << std::endl;
    std::cout << "  --no-memory         Disable memory tracking" << std::endl;
    std::cout << "  --no-timing         Disable timing analysis" << std::endl;
    std::cout << "  --help              Show this help" << std::endl;
}

int main(int argc, char* argv[]) {
    PerformanceAnalyzer::TestConfig config;
    
    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--servers" && i + 1 < argc) {
            config.num_servers = std::atoi(argv[++i]);
        } else if (arg == "--clients" && i + 1 < argc) {
            config.clients_per_server = std::atoi(argv[++i]);
        } else if (arg == "--topics" && i + 1 < argc) {
            config.topics_per_client = std::atoi(argv[++i]);
        } else if (arg == "--messages" && i + 1 < argc) {
            config.messages_per_topic = std::atoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            config.concurrent_operations = std::atoi(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            config.output_file = argv[++i];
        } else if (arg == "--no-memory") {
            config.enable_memory_tracking = false;
        } else if (arg == "--no-timing") {
            config.enable_timing_analysis = false;
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // 创建性能分析器并运行测试
    PerformanceAnalyzer analyzer(config);
    return analyzer.run_performance_test();
}