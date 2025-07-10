#include "mqtt_event_forwarding_queue.h"
#include <chrono>
#include "logger.h"

namespace mqtt {
namespace events {

EventForwardingQueue::EventForwardingQueue(size_t max_queue_size, int drop_policy)
    : max_queue_size_(max_queue_size), drop_policy_(drop_policy), stopped_(false) {
    if (max_queue_size_ == 0) {
        max_queue_size_ = 10000;  // Default size
    }
    
    LOG_INFO("EventForwardingQueue initialized: max_size={}, drop_policy={}", 
             max_queue_size_, drop_policy_);
}

EventForwardingQueue::~EventForwardingQueue() {
    stop();
}

bool EventForwardingQueue::enqueue(std::shared_ptr<MQTTEvent> event) {
    if (!event || stopped_.load()) {
        return false;
    }
    
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Check if queue is full
    if (queue_.size() >= max_queue_size_) {
        // Drop messages according to policy
        if (drop_policy_ == 0) {
            // Drop oldest (front of queue)
            if (!queue_.empty()) {
                queue_.pop();
                stats_.total_dropped.fetch_add(1);
            }
        } else {
            // Drop newest (don't add current event)
            stats_.total_dropped.fetch_add(1);
            return false;
        }
    }
    
    queue_.push(event);
    stats_.total_enqueued.fetch_add(1);
    stats_.current_queue_size.store(queue_.size());
    updateMaxQueueSize();
    
    lock.unlock();
    condition_.notify_one();
    
    return true;
}

size_t EventForwardingQueue::enqueueBatch(const EventBatch& batch) {
    if (batch.empty() || stopped_.load()) {
        return 0;
    }
    
    size_t enqueued_count = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    
    for (const auto& event : batch.events) {
        if (stopped_.load()) {
            break;
        }
        
        // Check if queue is full
        if (queue_.size() >= max_queue_size_) {
            if (drop_policy_ == 0) {
                // Drop oldest
                if (!queue_.empty()) {
                    queue_.pop();
                    stats_.total_dropped.fetch_add(1);
                }
            } else {
                // Drop newest (skip current event)
                stats_.total_dropped.fetch_add(1);
                continue;
            }
        }
        
        queue_.push(std::make_shared<MQTTEvent>(event));
        enqueued_count++;
    }
    
    stats_.total_enqueued.fetch_add(enqueued_count);
    stats_.current_queue_size.store(queue_.size());
    updateMaxQueueSize();
    
    lock.unlock();
    condition_.notify_all();
    
    return enqueued_count;
}

std::shared_ptr<MQTTEvent> EventForwardingQueue::dequeue(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (timeout_ms < 0) {
        // Infinite wait
        condition_.wait(lock, [this] { return !queue_.empty() || stopped_.load(); });
    } else {
        // Timed wait
        auto timeout = std::chrono::milliseconds(timeout_ms);
        if (!condition_.wait_for(lock, timeout, [this] { return !queue_.empty() || stopped_.load(); })) {
            return nullptr;  // Timeout
        }
    }
    
    if (stopped_.load() || queue_.empty()) {
        return nullptr;
    }
    
    auto event = queue_.front();
    queue_.pop();
    stats_.total_dequeued.fetch_add(1);
    stats_.current_queue_size.store(queue_.size());
    
    return event;
}

size_t EventForwardingQueue::dequeueBatch(MQTTVector<std::shared_ptr<MQTTEvent>>& events, 
                                          size_t max_count, 
                                          int timeout_ms) {
    events.clear();
    
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (timeout_ms < 0) {
        // Infinite wait
        condition_.wait(lock, [this] { return !queue_.empty() || stopped_.load(); });
    } else {
        // Timed wait
        auto timeout = std::chrono::milliseconds(timeout_ms);
        if (!condition_.wait_for(lock, timeout, [this] { return !queue_.empty() || stopped_.load(); })) {
            return 0;  // Timeout
        }
    }
    
    if (stopped_.load()) {
        return 0;
    }
    
    size_t count = 0;
    while (!queue_.empty() && count < max_count) {
        events.push_back(queue_.front());
        queue_.pop();
        count++;
    }
    
    stats_.total_dequeued.fetch_add(count);
    stats_.current_queue_size.store(queue_.size());
    
    return count;
}

std::shared_ptr<MQTTEvent> EventForwardingQueue::tryDequeue() {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (queue_.empty() || stopped_.load()) {
        return nullptr;
    }
    
    auto event = queue_.front();
    queue_.pop();
    stats_.total_dequeued.fetch_add(1);
    stats_.current_queue_size.store(queue_.size());
    
    return event;
}

size_t EventForwardingQueue::size() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return queue_.size();
}

bool EventForwardingQueue::empty() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return queue_.empty();
}

void EventForwardingQueue::clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    
    size_t dropped_count = queue_.size();
    while (!queue_.empty()) {
        queue_.pop();
    }
    
    stats_.total_dropped.fetch_add(dropped_count);
    stats_.current_queue_size.store(0);
    
    LOG_INFO("EventForwardingQueue cleared, dropped {} events", dropped_count);
}

ForwardingQueueStats EventForwardingQueue::getStats() const {
    return stats_;
}

void EventForwardingQueue::resetStats() {
    stats_.reset();
}

void EventForwardingQueue::setMaxQueueSize(size_t max_size) {
    std::unique_lock<std::mutex> lock(mutex_);
    max_queue_size_ = (max_size > 0) ? max_size : 10000;
    
    // Drop excess messages if current size exceeds new limit
    while (queue_.size() > max_queue_size_) {
        queue_.pop();
        stats_.total_dropped.fetch_add(1);
    }
    
    stats_.current_queue_size.store(queue_.size());
    
    LOG_INFO("EventForwardingQueue max size changed to: {}", max_queue_size_);
}

size_t EventForwardingQueue::getMaxQueueSize() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return max_queue_size_;
}

void EventForwardingQueue::stop() {
    stopped_.store(true);
    condition_.notify_all();
}

void EventForwardingQueue::dropMessages(size_t count) {
    for (size_t i = 0; i < count && !queue_.empty(); ++i) {
        queue_.pop();
        stats_.total_dropped.fetch_add(1);
    }
}

void EventForwardingQueue::updateMaxQueueSize() {
    size_t current_size = queue_.size();
    uint64_t max_size = stats_.max_queue_size.load();
    
    if (current_size > max_size) {
        stats_.max_queue_size.store(current_size);
    }
}

} // namespace events
} // namespace mqtt