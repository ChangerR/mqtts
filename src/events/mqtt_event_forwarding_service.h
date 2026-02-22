#ifndef MQTT_EVENT_FORWARDING_SERVICE_H
#define MQTT_EVENT_FORWARDING_SERVICE_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include "mqtt_event_types.h"
#include "mqtt_event_forwarding_queue.h"
#include "mqtt_event_rpc_client.h"
#include "mqtt_coroutine_utils.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {
namespace events {

// Forwarding service configuration
struct ForwardingServiceConfig {
    MQTTString server_host;
    int server_port;
    int worker_thread_count;
    int coroutines_per_thread;
    int connection_timeout_ms;
    int request_timeout_ms;
    int max_batch_size;
    int batch_timeout_ms;
    size_t max_queue_size;
    int queue_drop_policy;  // 0 = drop oldest, 1 = drop newest
    bool enabled;
    
    ForwardingServiceConfig() 
        : server_port(0), worker_thread_count(2), coroutines_per_thread(4),
          connection_timeout_ms(5000), request_timeout_ms(10000),
          max_batch_size(100), batch_timeout_ms(1000), max_queue_size(10000),
          queue_drop_policy(0), enabled(false) {}
};

// Per-thread worker statistics
struct WorkerThreadStats {
    std::atomic<uint64_t> events_processed{0};
    std::atomic<uint64_t> events_dropped{0};
    std::atomic<uint64_t> batches_sent{0};
    std::atomic<uint64_t> successful_batches{0};
    std::atomic<uint64_t> failed_batches{0};
    std::atomic<uint64_t> connection_errors{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    
    WorkerThreadStats() = default;
    
    WorkerThreadStats(const WorkerThreadStats& other) {
        events_processed.store(other.events_processed.load());
        events_dropped.store(other.events_dropped.load());
        batches_sent.store(other.batches_sent.load());
        successful_batches.store(other.successful_batches.load());
        failed_batches.store(other.failed_batches.load());
        connection_errors.store(other.connection_errors.load());
        bytes_sent.store(other.bytes_sent.load());
        bytes_received.store(other.bytes_received.load());
    }
    
    WorkerThreadStats& operator=(const WorkerThreadStats& other) {
        if (this != &other) {
            events_processed.store(other.events_processed.load());
            events_dropped.store(other.events_dropped.load());
            batches_sent.store(other.batches_sent.load());
            successful_batches.store(other.successful_batches.load());
            failed_batches.store(other.failed_batches.load());
            connection_errors.store(other.connection_errors.load());
            bytes_sent.store(other.bytes_sent.load());
            bytes_received.store(other.bytes_received.load());
        }
        return *this;
    }
    
    void reset() {
        events_processed.store(0);
        events_dropped.store(0);
        batches_sent.store(0);
        successful_batches.store(0);
        failed_batches.store(0);
        connection_errors.store(0);
        bytes_sent.store(0);
        bytes_received.store(0);
    }
};

// Forwarding service statistics
struct ForwardingServiceStats {
    std::atomic<uint64_t> total_events_received{0};
    std::atomic<uint64_t> total_events_forwarded{0};
    std::atomic<uint64_t> total_events_dropped{0};
    std::atomic<uint64_t> total_batches_sent{0};
    std::atomic<uint64_t> total_successful_batches{0};
    std::atomic<uint64_t> total_failed_batches{0};
    std::atomic<uint64_t> total_connection_errors{0};
    std::atomic<uint64_t> total_bytes_sent{0};
    std::atomic<uint64_t> total_bytes_received{0};
    
    MQTTVector<WorkerThreadStats> worker_stats;
    
    ForwardingServiceStats() = default;
    
    ForwardingServiceStats(const ForwardingServiceStats& other) {
        total_events_received.store(other.total_events_received.load());
        total_events_forwarded.store(other.total_events_forwarded.load());
        total_events_dropped.store(other.total_events_dropped.load());
        total_batches_sent.store(other.total_batches_sent.load());
        total_successful_batches.store(other.total_successful_batches.load());
        total_failed_batches.store(other.total_failed_batches.load());
        total_connection_errors.store(other.total_connection_errors.load());
        total_bytes_sent.store(other.total_bytes_sent.load());
        total_bytes_received.store(other.total_bytes_received.load());
        worker_stats = other.worker_stats;
    }
    
    ForwardingServiceStats& operator=(const ForwardingServiceStats& other) {
        if (this != &other) {
            total_events_received.store(other.total_events_received.load());
            total_events_forwarded.store(other.total_events_forwarded.load());
            total_events_dropped.store(other.total_events_dropped.load());
            total_batches_sent.store(other.total_batches_sent.load());
            total_successful_batches.store(other.total_successful_batches.load());
            total_failed_batches.store(other.total_failed_batches.load());
            total_connection_errors.store(other.total_connection_errors.load());
            total_bytes_sent.store(other.total_bytes_sent.load());
            total_bytes_received.store(other.total_bytes_received.load());
            worker_stats = other.worker_stats;
        }
        return *this;
    }
    
    void reset() {
        total_events_received.store(0);
        total_events_forwarded.store(0);
        total_events_dropped.store(0);
        total_batches_sent.store(0);
        total_successful_batches.store(0);
        total_failed_batches.store(0);
        total_connection_errors.store(0);
        total_bytes_sent.store(0);
        total_bytes_received.store(0);
        
        for (auto& worker_stat : worker_stats) {
            worker_stat.reset();
        }
    }
};

// Worker thread context
struct WorkerContext {
    int thread_id;
    std::unique_ptr<std::thread> thread;
    std::atomic<bool> running;
    MQTTVector<std::unique_ptr<EventRpcClient>> rpc_clients;
    WorkerThreadStats stats;
    
    WorkerContext(int id) : thread_id(id), running(false) {}
};

// Multi-threaded event forwarding service
class EventForwardingService {
public:
    /**
     * @brief Constructor
     * @param config Service configuration
     */
    explicit EventForwardingService(const ForwardingServiceConfig& config);
    
    /**
     * @brief Destructor
     */
    ~EventForwardingService();
    
    /**
     * @brief Initialize the forwarding service
     * @return 0 on success, negative error code on failure
     */
    int initialize();
    
    /**
     * @brief Start the forwarding service
     * @return 0 on success, negative error code on failure
     */
    int start();
    
    /**
     * @brief Stop the forwarding service
     */
    void stop();
    
    /**
     * @brief Forward a single event
     * @param event Event to forward
     * @return true if enqueued successfully, false if dropped
     */
    bool forwardEvent(std::shared_ptr<MQTTEvent> event);
    
    /**
     * @brief Forward a batch of events
     * @param batch Event batch to forward
     * @return Number of events successfully enqueued
     */
    size_t forwardEventBatch(const EventBatch& batch);
    
    /**
     * @brief Check if service is running
     * @return true if running
     */
    bool isRunning() const;
    
    /**
     * @brief Check if service is enabled
     * @return true if enabled
     */
    bool isEnabled() const;
    
    /**
     * @brief Get service configuration
     * @return Configuration structure
     */
    const ForwardingServiceConfig& getConfig() const;
    
    /**
     * @brief Update service configuration
     * @param config New configuration
     * @return 0 on success, negative error code on failure
     */
    int updateConfig(const ForwardingServiceConfig& config);
    
    /**
     * @brief Get service statistics
     * @return Statistics structure
     */
    ForwardingServiceStats getStats() const;
    
    /**
     * @brief Reset statistics
     */
    void resetStats();
    
    /**
     * @brief Get queue statistics
     * @return Queue statistics
     */
    ForwardingQueueStats getQueueStats() const;
    
    /**
     * @brief Get current queue size
     * @return Number of events in queue
     */
    size_t getQueueSize() const;
    
private:
    ForwardingServiceConfig config_;
    std::atomic<bool> initialized_;
    std::atomic<bool> running_;
    std::atomic<bool> enabled_;
    
    std::unique_ptr<EventForwardingQueue> event_queue_;
    MQTTVector<std::unique_ptr<WorkerContext>> workers_;
    
    mutable ForwardingServiceStats stats_;
    
    // Worker thread main function
    void workerThreadMain(WorkerContext* context);
    
    // Process events in batches using coroutines
    void processEventBatches(WorkerContext* context);
    
    // Create RPC clients for a worker thread
    int createRpcClients(WorkerContext* context);
    
    // Update global statistics from worker statistics
    void updateGlobalStats() const;
    
    // Validate configuration
    bool validateConfig(const ForwardingServiceConfig& config) const;
    
    // Initialize worker threads
    int initializeWorkers();
    
    // Stop worker threads
    void stopWorkers();
};

} // namespace events
} // namespace mqtt

#endif // MQTT_EVENT_FORWARDING_SERVICE_H