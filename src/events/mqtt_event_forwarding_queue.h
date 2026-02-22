#ifndef MQTT_EVENT_FORWARDING_QUEUE_H
#define MQTT_EVENT_FORWARDING_QUEUE_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include "mqtt_event_types.h"
#include "mqtt_allocator.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {
namespace events {

// Statistics for the forwarding queue
struct ForwardingQueueStats {
    std::atomic<uint64_t> total_enqueued{0};
    std::atomic<uint64_t> total_dequeued{0};
    std::atomic<uint64_t> total_dropped{0};
    std::atomic<uint64_t> current_queue_size{0};
    std::atomic<uint64_t> max_queue_size{0};
    
    ForwardingQueueStats() = default;
    
    ForwardingQueueStats(const ForwardingQueueStats& other) {
        total_enqueued.store(other.total_enqueued.load());
        total_dequeued.store(other.total_dequeued.load());
        total_dropped.store(other.total_dropped.load());
        current_queue_size.store(other.current_queue_size.load());
        max_queue_size.store(other.max_queue_size.load());
    }
    
    ForwardingQueueStats& operator=(const ForwardingQueueStats& other) {
        if (this != &other) {
            total_enqueued.store(other.total_enqueued.load());
            total_dequeued.store(other.total_dequeued.load());
            total_dropped.store(other.total_dropped.load());
            current_queue_size.store(other.current_queue_size.load());
            max_queue_size.store(other.max_queue_size.load());
        }
        return *this;
    }
    
    void reset() {
        total_enqueued.store(0);
        total_dequeued.store(0);
        total_dropped.store(0);
        current_queue_size.store(0);
        max_queue_size.store(0);
    }
};

// Thread-safe forwarding queue with message dropping capability
class EventForwardingQueue {
public:
    /**
     * @brief Constructor
     * @param max_queue_size Maximum queue size before dropping messages
     * @param drop_policy Drop policy: 0 = drop oldest, 1 = drop newest
     */
    explicit EventForwardingQueue(size_t max_queue_size = 10000, int drop_policy = 0);
    
    /**
     * @brief Destructor
     */
    ~EventForwardingQueue();
    
    /**
     * @brief Enqueue an event
     * @param event Event to enqueue
     * @return true if enqueued successfully, false if dropped
     */
    bool enqueue(std::shared_ptr<MQTTEvent> event);
    
    /**
     * @brief Enqueue a batch of events
     * @param batch Event batch to enqueue
     * @return Number of events successfully enqueued
     */
    size_t enqueueBatch(const EventBatch& batch);
    
    /**
     * @brief Dequeue an event (blocking)
     * @param timeout_ms Timeout in milliseconds, -1 for infinite
     * @return Event pointer or nullptr if timeout
     */
    std::shared_ptr<MQTTEvent> dequeue(int timeout_ms = -1);
    
    /**
     * @brief Dequeue multiple events (blocking)
     * @param events Output vector to store events
     * @param max_count Maximum number of events to dequeue
     * @param timeout_ms Timeout in milliseconds, -1 for infinite
     * @return Number of events dequeued
     */
    size_t dequeueBatch(MQTTVector<std::shared_ptr<MQTTEvent>>& events, 
                        size_t max_count = 100, 
                        int timeout_ms = -1);
    
    /**
     * @brief Try to dequeue an event (non-blocking)
     * @return Event pointer or nullptr if queue is empty
     */
    std::shared_ptr<MQTTEvent> tryDequeue();
    
    /**
     * @brief Get current queue size
     * @return Current number of events in queue
     */
    size_t size() const;
    
    /**
     * @brief Check if queue is empty
     * @return true if empty
     */
    bool empty() const;
    
    /**
     * @brief Clear the queue
     */
    void clear();
    
    /**
     * @brief Get queue statistics
     * @return Statistics structure
     */
    ForwardingQueueStats getStats() const;
    
    /**
     * @brief Reset statistics
     */
    void resetStats();
    
    /**
     * @brief Set maximum queue size
     * @param max_size New maximum size
     */
    void setMaxQueueSize(size_t max_size);
    
    /**
     * @brief Get maximum queue size
     * @return Maximum queue size
     */
    size_t getMaxQueueSize() const;
    
    /**
     * @brief Stop the queue (wake up all waiting threads)
     */
    void stop();
    
private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::shared_ptr<MQTTEvent>, std::deque<std::shared_ptr<MQTTEvent>, MQTTSTLAllocator<std::shared_ptr<MQTTEvent>>>> queue_;
    size_t max_queue_size_;
    int drop_policy_;  // 0 = drop oldest, 1 = drop newest
    std::atomic<bool> stopped_;
    
    mutable ForwardingQueueStats stats_;
    
    // Helper method to drop messages according to policy
    void dropMessages(size_t count);
    
    // Helper method to update statistics
    void updateMaxQueueSize();
};

} // namespace events
} // namespace mqtt

#endif // MQTT_EVENT_FORWARDING_QUEUE_H