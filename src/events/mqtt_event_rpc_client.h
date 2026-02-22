#ifndef MQTT_EVENT_RPC_CLIENT_H
#define MQTT_EVENT_RPC_CLIENT_H

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include "mqtt_event_types.h"
#include "mqtt_socket.h"
#include "mqtt_coroutine_utils.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {
namespace events {

// RPC client statistics
struct RpcClientStats {
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> successful_requests{0};
    std::atomic<uint64_t> failed_requests{0};
    std::atomic<uint64_t> timeout_requests{0};
    std::atomic<uint64_t> connection_errors{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> total_events_sent{0};
    
    RpcClientStats() = default;
    
    RpcClientStats(const RpcClientStats& other) {
        total_requests.store(other.total_requests.load());
        successful_requests.store(other.successful_requests.load());
        failed_requests.store(other.failed_requests.load());
        timeout_requests.store(other.timeout_requests.load());
        connection_errors.store(other.connection_errors.load());
        bytes_sent.store(other.bytes_sent.load());
        bytes_received.store(other.bytes_received.load());
        total_events_sent.store(other.total_events_sent.load());
    }
    
    RpcClientStats& operator=(const RpcClientStats& other) {
        if (this != &other) {
            total_requests.store(other.total_requests.load());
            successful_requests.store(other.successful_requests.load());
            failed_requests.store(other.failed_requests.load());
            timeout_requests.store(other.timeout_requests.load());
            connection_errors.store(other.connection_errors.load());
            bytes_sent.store(other.bytes_sent.load());
            bytes_received.store(other.bytes_received.load());
            total_events_sent.store(other.total_events_sent.load());
        }
        return *this;
    }
    
    void reset() {
        total_requests.store(0);
        successful_requests.store(0);
        failed_requests.store(0);
        timeout_requests.store(0);
        connection_errors.store(0);
        bytes_sent.store(0);
        bytes_received.store(0);
        total_events_sent.store(0);
    }
};

// RPC response structure
struct RpcResponse {
    bool success;
    MQTTString error_message;
    int processed_count;
    std::chrono::milliseconds response_time;
    
    RpcResponse() : success(false), processed_count(0), response_time(0) {}
};

// Coroutine-based RPC client for event forwarding
class EventRpcClient {
public:
    /**
     * @brief Constructor
     * @param server_host Target server host
     * @param server_port Target server port
     * @param connection_timeout Connection timeout in milliseconds
     * @param request_timeout Request timeout in milliseconds
     */
    EventRpcClient(const MQTTString& server_host, 
                   int server_port,
                   int connection_timeout = 5000,
                   int request_timeout = 10000);
    
    /**
     * @brief Destructor
     */
    ~EventRpcClient();
    
    /**
     * @brief Initialize the RPC client
     * @return 0 on success, negative error code on failure
     */
    int initialize();
    
    /**
     * @brief Send a single event
     * @param event Event to send
     * @return RPC response
     */
    RpcResponse sendEvent(const MQTTEvent& event);
    
    /**
     * @brief Send a batch of events
     * @param events Events to send
     * @return RPC response
     */
    RpcResponse sendEventBatch(const MQTTVector<MQTTEvent>& events);
    
    /**
     * @brief Send event batch from EventBatch structure
     * @param batch Event batch to send
     * @return RPC response
     */
    RpcResponse sendEventBatch(const EventBatch& batch);
    
    /**
     * @brief Check if client is connected
     * @return true if connected
     */
    bool isConnected() const;
    
    /**
     * @brief Reconnect to server
     * @return 0 on success, negative error code on failure
     */
    int reconnect();
    
    /**
     * @brief Close connection
     */
    void disconnect();
    
    /**
     * @brief Get client statistics
     * @return Statistics structure
     */
    RpcClientStats getStats() const;
    
    /**
     * @brief Reset statistics
     */
    void resetStats();
    
    /**
     * @brief Set connection timeout
     * @param timeout_ms Timeout in milliseconds
     */
    void setConnectionTimeout(int timeout_ms);
    
    /**
     * @brief Set request timeout
     * @param timeout_ms Timeout in milliseconds
     */
    void setRequestTimeout(int timeout_ms);
    
    /**
     * @brief Get server host
     * @return Server host
     */
    const MQTTString& getServerHost() const { return server_host_; }
    
    /**
     * @brief Get server port
     * @return Server port
     */
    int getServerPort() const { return server_port_; }
    
private:
    MQTTString server_host_;
    int server_port_;
    int connection_timeout_;
    int request_timeout_;
    
    std::unique_ptr<MQTTSocket> socket_;
    std::atomic<bool> connected_;
    std::atomic<bool> initialized_;
    
    mutable RpcClientStats stats_;
    
    // Helper methods for protobuf serialization
    MQTTVector<uint8_t> serializeEventBatch(const EventBatch& batch);
    MQTTVector<uint8_t> serializeEvents(const MQTTVector<MQTTEvent>& events);
    RpcResponse deserializeResponse(const MQTTVector<uint8_t>& data);
    
    // Helper methods for network communication
    int sendData(const MQTTVector<uint8_t>& data);
    int receiveData(MQTTVector<uint8_t>& data);
    
    // Helper method to create RPC request
    MQTTVector<uint8_t> createRpcRequest(const MQTTVector<uint8_t>& serialized_batch);
    
    // Helper method to parse RPC response
    RpcResponse parseRpcResponse(const MQTTVector<uint8_t>& response_data);
    
    // Connection management
    int establishConnection();
    void closeConnection();
    
    // Update statistics
    void updateStats(bool success, size_t bytes_sent, size_t bytes_received, 
                     size_t events_count, std::chrono::milliseconds response_time);
};

} // namespace events
} // namespace mqtt

#endif // MQTT_EVENT_RPC_CLIENT_H