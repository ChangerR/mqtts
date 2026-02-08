#include "mqtt_event_rpc_client.h"
#include <cstring>
#include <sstream>
#include "logger.h"
#include "mqtt_define.h"

namespace mqtt {
namespace events {

// Simple protobuf-like serialization for demonstration
// In real implementation, you would use actual protobuf generated code

namespace {
    // Helper functions for binary serialization
    template<typename VectorType>
    void writeUint32(VectorType& buffer, uint32_t value) {
        buffer.push_back((value >> 24) & 0xFF);
        buffer.push_back((value >> 16) & 0xFF);
        buffer.push_back((value >> 8) & 0xFF);
        buffer.push_back(value & 0xFF);
    }
    
    template<typename VectorType>
    void writeString(VectorType& buffer, const MQTTString& str) {
        writeUint32(buffer, static_cast<uint32_t>(str.length()));
        buffer.insert(buffer.end(), str.begin(), str.end());
    }
    
    template<typename VectorType>
    void writeBytes(VectorType& buffer, const MQTTVector<uint8_t>& data) {
        writeUint32(buffer, static_cast<uint32_t>(data.size()));
        buffer.insert(buffer.end(), data.begin(), data.end());
    }
    
    template<typename VectorType>
    uint32_t readUint32(const VectorType& buffer, size_t& offset) {
        if (offset + 4 > buffer.size()) return 0;
        uint32_t value = (static_cast<uint32_t>(buffer[offset]) << 24) |
                        (static_cast<uint32_t>(buffer[offset + 1]) << 16) |
                        (static_cast<uint32_t>(buffer[offset + 2]) << 8) |
                        static_cast<uint32_t>(buffer[offset + 3]);
        offset += 4;
        return value;
    }
    
    template<typename VectorType>
    MQTTString readString(const VectorType& buffer, size_t& offset) {
        uint32_t length = readUint32(buffer, offset);
        if (offset + length > buffer.size()) {
            return to_mqtt_string("", MQTTMemoryManager::get_instance().get_root_allocator());
        }
        MQTTString str = to_mqtt_string(std::string(buffer.begin() + offset, buffer.begin() + offset + length), 
                                       MQTTMemoryManager::get_instance().get_root_allocator());
        offset += length;
        return str;
    }
}

EventRpcClient::EventRpcClient(const MQTTString& server_host, 
                               int server_port,
                               int connection_timeout,
                               int request_timeout)
    : server_host_(server_host), server_port_(server_port),
      connection_timeout_(connection_timeout), request_timeout_(request_timeout),
      connected_(false), initialized_(false) {
    
    if (connection_timeout_ <= 0) connection_timeout_ = 5000;
    if (request_timeout_ <= 0) request_timeout_ = 10000;
    
    LOG_INFO("EventRpcClient created: {}:{}, conn_timeout={}ms, req_timeout={}ms",
             server_host_, server_port_, connection_timeout_, request_timeout_);
}

EventRpcClient::~EventRpcClient() {
    disconnect();
}

int EventRpcClient::initialize() {
    if (initialized_.load()) {
        return MQ_SUCCESS;
    }
    
    try {
        socket_.reset(new MQTTSocket());
        initialized_.store(true);
        
        LOG_INFO("EventRpcClient initialized");
        return MQ_SUCCESS;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to initialize EventRpcClient: {}", e.what());
        return MQ_ERR_INTERNAL;
    }
}

RpcResponse EventRpcClient::sendEvent(const MQTTEvent& event) {
    MQTTVector<MQTTEvent> events(MQTTSTLAllocator<MQTTEvent>(MQTTMemoryManager::get_instance().get_root_allocator()));
    events.push_back(event);
    return sendEventBatch(events);
}

RpcResponse EventRpcClient::sendEventBatch(const MQTTVector<MQTTEvent>& events) {
    RpcResponse response;
    
    if (events.empty()) {
        response.error_message = to_mqtt_string("Empty event batch", MQTTMemoryManager::get_instance().get_root_allocator());
        return response;
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Serialize events
    MQTTVector<uint8_t> serialized_data = serializeEvents(events);
    if (serialized_data.empty()) {
        response.error_message = to_mqtt_string("Failed to serialize events", MQTTMemoryManager::get_instance().get_root_allocator());
        stats_.failed_requests.fetch_add(1);
        return response;
    }
    
    // Create RPC request
    MQTTVector<uint8_t> request_data = createRpcRequest(serialized_data);
    
    // Ensure connection
    if (!connected_.load()) {
        if (establishConnection() != MQ_SUCCESS) {
            response.error_message = to_mqtt_string("Failed to establish connection", MQTTMemoryManager::get_instance().get_root_allocator());
            stats_.connection_errors.fetch_add(1);
            return response;
        }
    }
    
    // Send request
    if (sendData(request_data) != MQ_SUCCESS) {
        response.error_message = to_mqtt_string("Failed to send request", MQTTMemoryManager::get_instance().get_root_allocator());
        stats_.failed_requests.fetch_add(1);
        connected_.store(false);
        return response;
    }
    
    // Receive response
    MQTTVector<uint8_t> response_data(MQTTSTLAllocator<uint8_t>(MQTTMemoryManager::get_instance().get_root_allocator()));
    if (receiveData(response_data) != MQ_SUCCESS) {
        response.error_message = to_mqtt_string("Failed to receive response", MQTTMemoryManager::get_instance().get_root_allocator());
        stats_.failed_requests.fetch_add(1);
        connected_.store(false);
        return response;
    }
    
    // Parse response
    response = parseRpcResponse(response_data);
    
    auto end_time = std::chrono::steady_clock::now();
    response.response_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Update statistics
    updateStats(response.success, request_data.size(), response_data.size(), 
                events.size(), response.response_time);
    
    return response;
}

RpcResponse EventRpcClient::sendEventBatch(const EventBatch& batch) {
    return sendEventBatch(batch.events);
}

bool EventRpcClient::isConnected() const {
    return connected_.load();
}

int EventRpcClient::reconnect() {
    disconnect();
    return establishConnection();
}

void EventRpcClient::disconnect() {
    closeConnection();
    connected_.store(false);
}

RpcClientStats EventRpcClient::getStats() const {
    return stats_;
}

void EventRpcClient::resetStats() {
    stats_.reset();
}

void EventRpcClient::setConnectionTimeout(int timeout_ms) {
    connection_timeout_ = (timeout_ms > 0) ? timeout_ms : 5000;
}

void EventRpcClient::setRequestTimeout(int timeout_ms) {
    request_timeout_ = (timeout_ms > 0) ? timeout_ms : 10000;
}

MQTTVector<uint8_t> EventRpcClient::serializeEvents(const MQTTVector<MQTTEvent>& events) {
    MQTTVector<uint8_t> buffer(MQTTSTLAllocator<uint8_t>(MQTTMemoryManager::get_instance().get_root_allocator()));
    
    // Write event count
    writeUint32(buffer, static_cast<uint32_t>(events.size()));
    
    // Write timestamp (batch timestamp)
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    writeUint32(buffer, static_cast<uint32_t>(timestamp >> 32));
    writeUint32(buffer, static_cast<uint32_t>(timestamp & 0xFFFFFFFF));
    
    // Write events
    for (const auto& event : events) {
        // Write event type
        writeUint32(buffer, static_cast<uint32_t>(event.type));
        
        // Write event data based on type
        switch (event.type) {
            case EventType::LOGIN: {
                auto login_data = event.getLoginData();
                if (login_data) {
                    writeString(buffer, login_data->metadata.event_id);
                    writeString(buffer, login_data->metadata.client_id);
                    writeString(buffer, login_data->metadata.server_id);
                    writeString(buffer, login_data->username);
                    writeString(buffer, login_data->protocol_version);
                    writeUint32(buffer, static_cast<uint32_t>(login_data->keep_alive));
                    buffer.push_back(login_data->clean_session ? 1 : 0);
                }
                break;
            }
            case EventType::LOGOUT: {
                auto logout_data = event.getLogoutData();
                if (logout_data) {
                    writeString(buffer, logout_data->metadata.event_id);
                    writeString(buffer, logout_data->metadata.client_id);
                    writeString(buffer, logout_data->metadata.server_id);
                    writeString(buffer, logout_data->reason);
                    writeUint32(buffer, static_cast<uint32_t>(logout_data->session_duration_seconds));
                    writeUint32(buffer, static_cast<uint32_t>(logout_data->messages_sent));
                    writeUint32(buffer, static_cast<uint32_t>(logout_data->messages_received));
                }
                break;
            }
            case EventType::PUBLISH: {
                auto publish_data = event.getPublishData();
                if (publish_data) {
                    writeString(buffer, publish_data->metadata.event_id);
                    writeString(buffer, publish_data->metadata.client_id);
                    writeString(buffer, publish_data->metadata.server_id);
                    writeString(buffer, publish_data->topic);
                    writeUint32(buffer, static_cast<uint32_t>(publish_data->qos));
                    buffer.push_back(publish_data->retain ? 1 : 0);
                    buffer.push_back(publish_data->duplicate ? 1 : 0);
                    writeUint32(buffer, static_cast<uint32_t>(publish_data->payload_size));
                    // Note: We don't send payload data to save bandwidth
                }
                break;
            }
        }
    }
    
    return buffer;
}

MQTTVector<uint8_t> EventRpcClient::serializeEventBatch(const EventBatch& batch) {
    return serializeEvents(batch.events);
}

RpcResponse EventRpcClient::deserializeResponse(const MQTTVector<uint8_t>& data) {
    return parseRpcResponse(data);
}

int EventRpcClient::sendData(const MQTTVector<uint8_t>& data) {
    if (!socket_ || !connected_.load()) {
        return MQ_ERR_SOCKET_SEND;
    }
    
    try {
        ssize_t sent = socket_->send(data.data(), data.size());
        if (sent != static_cast<ssize_t>(data.size())) {
            LOG_ERROR("Failed to send complete data: sent={}, expected={}", sent, data.size());
            return MQ_ERR_SOCKET_SEND;
        }
        
        stats_.bytes_sent.fetch_add(data.size());
        return MQ_SUCCESS;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during send: {}", e.what());
        return MQ_ERR_SOCKET_SEND;
    }
}

int EventRpcClient::receiveData(MQTTVector<uint8_t>& data) {
    if (!socket_ || !connected_.load()) {
        return MQ_ERR_SOCKET_RECV;
    }
    
    try {
        // First, read the response length (4 bytes)
        uint8_t length_buffer[4];
        int recv_len = 4;
        ssize_t received = socket_->recv(reinterpret_cast<char*>(length_buffer), recv_len);
        if (received != 4) {
            LOG_ERROR("Failed to receive response length: received={}", received);
            return MQ_ERR_SOCKET_RECV;
        }
        
        uint32_t response_length = (static_cast<uint32_t>(length_buffer[0]) << 24) |
                                  (static_cast<uint32_t>(length_buffer[1]) << 16) |
                                  (static_cast<uint32_t>(length_buffer[2]) << 8) |
                                  static_cast<uint32_t>(length_buffer[3]);
        
        if (response_length > 1024 * 1024) {  // 1MB limit
            LOG_ERROR("Response too large: {}", response_length);
            return MQ_ERR_PACKET_TOO_LARGE;
        }
        
        // Read the response data
        data.resize(response_length);
        int recv_len2 = static_cast<int>(response_length);
        received = socket_->recv(reinterpret_cast<char*>(data.data()), recv_len2);
        if (received != static_cast<ssize_t>(response_length)) {
            LOG_ERROR("Failed to receive complete response: received={}, expected={}", received, response_length);
            return MQ_ERR_SOCKET_RECV;
        }
        
        stats_.bytes_received.fetch_add(4 + response_length);
        return MQ_SUCCESS;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during receive: {}", e.what());
        return MQ_ERR_SOCKET_RECV;
    }
}

MQTTVector<uint8_t> EventRpcClient::createRpcRequest(const MQTTVector<uint8_t>& serialized_batch) {
    MQTTVector<uint8_t> request(MQTTSTLAllocator<uint8_t>(MQTTMemoryManager::get_instance().get_root_allocator()));
    
    // Write request length
    writeUint32(request, static_cast<uint32_t>(serialized_batch.size()));
    
    // Write batch data
    request.insert(request.end(), serialized_batch.begin(), serialized_batch.end());
    
    return request;
}

RpcResponse EventRpcClient::parseRpcResponse(const MQTTVector<uint8_t>& response_data) {
    RpcResponse response;
    
    if (response_data.size() < 8) {
        response.error_message = "Invalid response format";
        return response;
    }
    
    size_t offset = 0;
    
    // Read success flag
    response.success = (response_data[offset++] == 1);
    
    // Read processed count
    response.processed_count = static_cast<int>(readUint32(response_data, offset));
    
    // Read error message if present
    if (!response.success && offset < response_data.size()) {
        response.error_message = readString(response_data, offset);
    }
    
    return response;
}

int EventRpcClient::establishConnection() {
    if (!initialized_.load()) {
        if (initialize() != MQ_SUCCESS) {
            return MQ_ERR_INTERNAL;
        }
    }
    
    try {
        int result = socket_->connect(server_host_.c_str(), server_port_);
        if (result == MQ_SUCCESS) {
            connected_.store(true);
            LOG_INFO("EventRpcClient connected to {}:{}", server_host_, server_port_);
        } else {
            LOG_ERROR("Failed to connect to {}:{}, error: {}", server_host_, server_port_, result);
            stats_.connection_errors.fetch_add(1);
        }
        return result;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during connection: {}", e.what());
        stats_.connection_errors.fetch_add(1);
        return MQ_ERR_SOCKET_CONNECT;
    }
}

void EventRpcClient::closeConnection() {
    if (socket_) {
        socket_->close();
        LOG_DEBUG("EventRpcClient connection closed");
    }
}

void EventRpcClient::updateStats(bool success, size_t bytes_sent, size_t bytes_received, 
                                 size_t events_count, std::chrono::milliseconds response_time) {
    stats_.total_requests.fetch_add(1);
    stats_.total_events_sent.fetch_add(events_count);
    
    if (success) {
        stats_.successful_requests.fetch_add(1);
    } else {
        stats_.failed_requests.fetch_add(1);
    }
    
    if (response_time.count() > request_timeout_) {
        stats_.timeout_requests.fetch_add(1);
    }
    
    // bytes_sent and bytes_received are already updated in sendData/receiveData
}

} // namespace events
} // namespace mqtt