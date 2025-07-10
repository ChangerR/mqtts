#include "mqtt_event_forwarding_service.h"
#include <algorithm>
#include <chrono>
#include "logger.h"
#include "mqtt_define.h"

namespace mqtt {
namespace events {

EventForwardingService::EventForwardingService(const ForwardingServiceConfig& config)
    : config_(config), initialized_(false), running_(false), enabled_(config.enabled) {
    
    if (!validateConfig(config_)) {
        LOG_WARN("Invalid forwarding service configuration, service disabled");
        enabled_.store(false);
    }
    
    LOG_INFO("EventForwardingService created: enabled={}, host={}:{}, workers={}, coroutines_per_thread={}",
             enabled_.load(), config_.server_host, config_.server_port, 
             config_.worker_thread_count, config_.coroutines_per_thread);
}

EventForwardingService::~EventForwardingService() {
    stop();
}

int EventForwardingService::initialize() {
    if (initialized_.load()) {
        return MQ_SUCCESS;
    }
    
    if (!enabled_.load()) {
        LOG_INFO("EventForwardingService is disabled, skipping initialization");
        return MQ_SUCCESS;
    }
    
    // Create event queue
    event_queue_ = std::unique_ptr<EventForwardingQueue>(new EventForwardingQueue(config_.max_queue_size, config_.queue_drop_policy));
    if (!event_queue_) {
        LOG_ERROR("Failed to create event forwarding queue");
        return MQ_ERR_MEMORY_ALLOC;
    }
    
    // Initialize worker threads
    int result = initializeWorkers();
    if (result != MQ_SUCCESS) {
        LOG_ERROR("Failed to initialize worker threads: {}", result);
        return result;
    }
    
    // Initialize statistics
    stats_.worker_stats.resize(config_.worker_thread_count);
    
    initialized_.store(true);
    LOG_INFO("EventForwardingService initialized successfully");
    
    return MQ_SUCCESS;
}

int EventForwardingService::start() {
    if (!enabled_.load()) {
        LOG_INFO("EventForwardingService is disabled, not starting");
        return MQ_SUCCESS;
    }
    
    if (!initialized_.load()) {
        int result = initialize();
        if (result != MQ_SUCCESS) {
            return result;
        }
    }
    
    if (running_.load()) {
        LOG_WARN("EventForwardingService is already running");
        return MQ_SUCCESS;
    }
    
    // Start worker threads
    for (auto& worker : workers_) {
        worker->running.store(true);
        worker->thread.reset(new std::thread(&EventForwardingService::workerThreadMain, this, worker.get()));
    }
    
    running_.store(true);
    LOG_INFO("EventForwardingService started with {} worker threads", workers_.size());
    
    return MQ_SUCCESS;
}

void EventForwardingService::stop() {
    if (!running_.load()) {
        return;
    }
    
    LOG_INFO("Stopping EventForwardingService...");
    
    running_.store(false);
    
    // Stop event queue to wake up waiting threads
    if (event_queue_) {
        event_queue_->stop();
    }
    
    // Stop worker threads
    stopWorkers();
    
    LOG_INFO("EventForwardingService stopped");
}

bool EventForwardingService::forwardEvent(std::shared_ptr<MQTTEvent> event) {
    if (!enabled_.load() || !running_.load() || !event_queue_) {
        return false;
    }
    
    bool result = event_queue_->enqueue(event);
    stats_.total_events_received.fetch_add(1);
    
    if (!result) {
        stats_.total_events_dropped.fetch_add(1);
    }
    
    return result;
}

size_t EventForwardingService::forwardEventBatch(const EventBatch& batch) {
    if (!enabled_.load() || !running_.load() || !event_queue_) {
        return 0;
    }
    
    size_t enqueued_count = event_queue_->enqueueBatch(batch);
    stats_.total_events_received.fetch_add(batch.size());
    stats_.total_events_dropped.fetch_add(batch.size() - enqueued_count);
    
    return enqueued_count;
}

bool EventForwardingService::isRunning() const {
    return running_.load();
}

bool EventForwardingService::isEnabled() const {
    return enabled_.load();
}

const ForwardingServiceConfig& EventForwardingService::getConfig() const {
    return config_;
}

int EventForwardingService::updateConfig(const ForwardingServiceConfig& config) {
    if (!validateConfig(config)) {
        LOG_ERROR("Invalid configuration provided");
        return MQ_ERR_INVALID_ARGS;
    }
    
    bool was_running = running_.load();
    
    // Stop service if running
    if (was_running) {
        stop();
    }
    
    // Update configuration
    config_ = config;
    enabled_.store(config.enabled);
    initialized_.store(false);
    
    // Restart if it was running before
    if (was_running && enabled_.load()) {
        return start();
    }
    
    LOG_INFO("EventForwardingService configuration updated");
    return MQ_SUCCESS;
}

ForwardingServiceStats EventForwardingService::getStats() const {
    updateGlobalStats();
    return stats_;
}

void EventForwardingService::resetStats() {
    stats_.reset();
    
    if (event_queue_) {
        event_queue_->resetStats();
    }
}

ForwardingQueueStats EventForwardingService::getQueueStats() const {
    return event_queue_ ? event_queue_->getStats() : ForwardingQueueStats{};
}

size_t EventForwardingService::getQueueSize() const {
    return event_queue_ ? event_queue_->size() : 0;
}

void EventForwardingService::workerThreadMain(WorkerContext* context) {
    LOG_INFO("EventForwardingService worker thread {} started", context->thread_id);
    
    // Create RPC clients for this worker
    if (createRpcClients(context) != MQ_SUCCESS) {
        LOG_ERROR("Failed to create RPC clients for worker {}", context->thread_id);
        return;
    }
    
    // Process events in batches
    processEventBatches(context);
    
    LOG_INFO("EventForwardingService worker thread {} stopped", context->thread_id);
}

void EventForwardingService::processEventBatches(WorkerContext* context) {
    MQTTVector<std::shared_ptr<MQTTEvent>> events;
    events.reserve(config_.max_batch_size);
    
    int current_client_index = 0;
    
    while (context->running.load() && running_.load()) {
        events.clear();
        
        // Dequeue events with timeout
        size_t dequeued_count = event_queue_->dequeueBatch(events, config_.max_batch_size, config_.batch_timeout_ms);
        
        if (dequeued_count == 0) {
            continue;  // Timeout or shutdown
        }
        
        context->stats.events_processed.fetch_add(dequeued_count);
        
        // Convert to vector of MQTTEvent for RPC client
        MQTTVector<MQTTEvent> event_batch;
        event_batch.reserve(dequeued_count);
        
        for (const auto& event_ptr : events) {
            if (event_ptr) {
                event_batch.push_back(*event_ptr);
            }
        }
        
        if (event_batch.empty()) {
            continue;
        }
        
        // Round-robin through RPC clients
        if (context->rpc_clients.empty()) {
            context->stats.events_dropped.fetch_add(event_batch.size());
            continue;
        }
        
        auto& rpc_client = context->rpc_clients[current_client_index];
        current_client_index = (current_client_index + 1) % context->rpc_clients.size();
        
        // Send batch using RPC client
        context->stats.batches_sent.fetch_add(1);
        
        RpcResponse response = rpc_client->sendEventBatch(event_batch);
        
        if (response.success) {
            context->stats.successful_batches.fetch_add(1);
        } else {
            context->stats.failed_batches.fetch_add(1);
            context->stats.events_dropped.fetch_add(event_batch.size());
            
            if (response.error_message.find("connection") != std::string::npos) {
                context->stats.connection_errors.fetch_add(1);
            }
            
            LOG_DEBUG("Worker {} failed to send batch: {}", context->thread_id, response.error_message);
        }
        
        // Update statistics from RPC client
        auto client_stats = rpc_client->getStats();
        context->stats.bytes_sent.store(client_stats.bytes_sent.load());
        context->stats.bytes_received.store(client_stats.bytes_received.load());
    }
}

int EventForwardingService::createRpcClients(WorkerContext* context) {
    context->rpc_clients.clear();
    context->rpc_clients.reserve(config_.coroutines_per_thread);
    
    for (int i = 0; i < config_.coroutines_per_thread; ++i) {
        std::unique_ptr<EventRpcClient> client(new EventRpcClient(
            config_.server_host,
            config_.server_port,
            config_.connection_timeout_ms,
            config_.request_timeout_ms
        ));
        
        if (client->initialize() != MQ_SUCCESS) {
            LOG_ERROR("Failed to initialize RPC client {} for worker {}", i, context->thread_id);
            return MQ_ERR_INTERNAL;
        }
        
        context->rpc_clients.push_back(std::move(client));
    }
    
    LOG_DEBUG("Created {} RPC clients for worker {}", config_.coroutines_per_thread, context->thread_id);
    return MQ_SUCCESS;
}

void EventForwardingService::updateGlobalStats() const {
    // Reset global stats
    stats_.total_events_forwarded.store(0);
    stats_.total_events_dropped.store(0);
    stats_.total_batches_sent.store(0);
    stats_.total_successful_batches.store(0);
    stats_.total_failed_batches.store(0);
    stats_.total_connection_errors.store(0);
    stats_.total_bytes_sent.store(0);
    stats_.total_bytes_received.store(0);
    
    // Aggregate from worker stats
    for (size_t i = 0; i < workers_.size() && i < stats_.worker_stats.size(); ++i) {
        const auto& worker_stats = workers_[i]->stats;
        
        stats_.total_events_forwarded.fetch_add(worker_stats.events_processed.load());
        stats_.total_events_dropped.fetch_add(worker_stats.events_dropped.load());
        stats_.total_batches_sent.fetch_add(worker_stats.batches_sent.load());
        stats_.total_successful_batches.fetch_add(worker_stats.successful_batches.load());
        stats_.total_failed_batches.fetch_add(worker_stats.failed_batches.load());
        stats_.total_connection_errors.fetch_add(worker_stats.connection_errors.load());
        stats_.total_bytes_sent.fetch_add(worker_stats.bytes_sent.load());
        stats_.total_bytes_received.fetch_add(worker_stats.bytes_received.load());
        
        // Copy to stats for detailed reporting
        stats_.worker_stats[i] = worker_stats;
    }
}

bool EventForwardingService::validateConfig(const ForwardingServiceConfig& config) const {
    if (!config.enabled) {
        return true;  // Disabled config is always valid
    }
    
    if (config.server_host.empty()) {
        LOG_ERROR("Server host cannot be empty");
        return false;
    }
    
    if (config.server_port <= 0 || config.server_port > 65535) {
        LOG_ERROR("Invalid server port: {}", config.server_port);
        return false;
    }
    
    if (config.worker_thread_count <= 0 || config.worker_thread_count > 32) {
        LOG_ERROR("Invalid worker thread count: {}", config.worker_thread_count);
        return false;
    }
    
    if (config.coroutines_per_thread <= 0 || config.coroutines_per_thread > 16) {
        LOG_ERROR("Invalid coroutines per thread: {}", config.coroutines_per_thread);
        return false;
    }
    
    if (config.max_batch_size <= 0 || config.max_batch_size > 10000) {
        LOG_ERROR("Invalid max batch size: {}", config.max_batch_size);
        return false;
    }
    
    if (config.max_queue_size == 0) {
        LOG_ERROR("Max queue size cannot be zero");
        return false;
    }
    
    return true;
}

int EventForwardingService::initializeWorkers() {
    workers_.clear();
    workers_.reserve(config_.worker_thread_count);
    
    for (int i = 0; i < config_.worker_thread_count; ++i) {
        std::unique_ptr<WorkerContext> worker(new WorkerContext(i));
        workers_.push_back(std::move(worker));
    }
    
    LOG_DEBUG("Initialized {} worker contexts", workers_.size());
    return MQ_SUCCESS;
}

void EventForwardingService::stopWorkers() {
    // Signal all workers to stop
    for (auto& worker : workers_) {
        worker->running.store(false);
    }
    
    // Wait for all worker threads to finish
    for (auto& worker : workers_) {
        if (worker->thread && worker->thread->joinable()) {
            worker->thread->join();
        }
    }
    
    LOG_DEBUG("All worker threads stopped");
}

} // namespace events
} // namespace mqtt