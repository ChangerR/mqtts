#include "mqtt_router_rpc_client.h"
#include "mqtt_memory_tags.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>

MQTTRouterRpcClient::MQTTRouterRpcClient(MQTTAllocator* allocator, const RpcClientConfig& config)
    : allocator_(allocator)
    , config_(config)
    , socket_fd_(-1)
    , connected_(false)
    , should_stop_(false)
    , next_request_id_(1)
    , pending_requests_(MQTTSTLAllocator<std::pair<const uint32_t, std::shared_ptr<PendingRequest>>>(allocator))
    , last_error_time_(0)
    , last_error_code_(0)
    , last_error_message_(MQTTStrAllocator(allocator))
    , total_requests_(0)
    , successful_requests_(0)
    , failed_requests_(0)
    , server_id_(MQTTStrAllocator(allocator))
{
    std::string temp_str = "mqtt_server_" + std::to_string(getpid());
    server_id_.assign(temp_str.begin(), temp_str.end());
}

MQTTRouterRpcClient::~MQTTRouterRpcClient() {
    disconnect();
}

int MQTTRouterRpcClient::initialize() {
    return 0;
}

int MQTTRouterRpcClient::connect() {
    if (connected_) {
        return 0;
    }
    
    if (connect_to_server() != 0) {
        return -1;
    }
    
    connected_ = true;
    should_stop_ = false;
    
    connection_thread_ = std::thread(&MQTTRouterRpcClient::connection_thread_func, this);
    
    if (config_.enable_heartbeat) {
        heartbeat_thread_ = std::thread(&MQTTRouterRpcClient::heartbeat_thread_func, this);
    }
    
    spdlog::info("RPC client connected to router at {}:{}", config_.router_host, config_.router_port);
    return 0;
}

int MQTTRouterRpcClient::disconnect() {
    if (!connected_) {
        return 0;
    }
    
    should_stop_ = true;
    connected_ = false;
    
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    
    if (connection_thread_.joinable()) {
        connection_thread_.join();
    }
    
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    
    {
        std::lock_guard<std::mutex> lock(pending_requests_mutex_);
        for (auto& pair : pending_requests_) {
            auto& request = pair.second;
            if (request->mutex && request->cv) {
                std::lock_guard<std::mutex> req_lock(*request->mutex);
                request->completed = true;
                request->error_code = -1;
                request->cv->notify_one();
            }
        }
        pending_requests_.clear();
    }
    
    spdlog::info("RPC client disconnected from router");
    return 0;
}

int MQTTRouterRpcClient::connect_to_server() {
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        spdlog::error("Failed to create socket: {}", strerror(errno));
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config_.router_port);
    
    if (inet_pton(AF_INET, config_.router_host.c_str(), &server_addr.sin_addr) <= 0) {
        spdlog::error("Invalid router host address: {}", config_.router_host);
        close(socket_fd_);
        socket_fd_ = -1;
        return -1;
    }
    
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
    
    int result = ::connect(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (result < 0 && errno != EINPROGRESS) {
        spdlog::error("Failed to connect to router: {}", strerror(errno));
        close(socket_fd_);
        socket_fd_ = -1;
        return -1;
    }
    
    if (errno == EINPROGRESS) {
        struct pollfd pfd;
        pfd.fd = socket_fd_;
        pfd.events = POLLOUT;
        
        int poll_result = poll(&pfd, 1, config_.connect_timeout_ms);
        if (poll_result <= 0) {
            spdlog::error("Connection timeout to router");
            close(socket_fd_);
            socket_fd_ = -1;
            return -1;
        }
        
        int so_error;
        socklen_t so_error_len = sizeof(so_error);
        if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) < 0 || so_error != 0) {
            spdlog::error("Failed to connect to router: {}", strerror(so_error));
            close(socket_fd_);
            socket_fd_ = -1;
            return -1;
        }
    }
    
    flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags & ~O_NONBLOCK);
    
    return 0;
}

int MQTTRouterRpcClient::send_message(const RpcMessage& message) {
    if (!connected_ || socket_fd_ < 0) {
        return -1;
    }
    
    uint8_t header[9];
    header[0] = static_cast<uint8_t>(message.type);
    
    header[1] = (message.request_id >> 24) & 0xFF;
    header[2] = (message.request_id >> 16) & 0xFF;
    header[3] = (message.request_id >> 8) & 0xFF;
    header[4] = message.request_id & 0xFF;
    
    header[5] = (message.payload_size >> 24) & 0xFF;
    header[6] = (message.payload_size >> 16) & 0xFF;
    header[7] = (message.payload_size >> 8) & 0xFF;
    header[8] = message.payload_size & 0xFF;
    
    if (send(socket_fd_, header, sizeof(header), 0) != sizeof(header)) {
        spdlog::error("Failed to send message header: {}", strerror(errno));
        return -1;
    }
    
    if (message.payload_size > 0) {
        if (send(socket_fd_, message.payload.data(), message.payload_size, 0) != static_cast<ssize_t>(message.payload_size)) {
            spdlog::error("Failed to send message payload: {}", strerror(errno));
            return -1;
        }
    }
    
    return 0;
}

int MQTTRouterRpcClient::receive_message(RpcMessage& message) {
    if (!connected_ || socket_fd_ < 0) {
        return -1;
    }
    
    uint8_t header[9];
    if (recv(socket_fd_, header, sizeof(header), 0) != sizeof(header)) {
        return -1;
    }
    
    message.type = static_cast<MessageType>(header[0]);
    message.request_id = (static_cast<uint32_t>(header[1]) << 24) |
                        (static_cast<uint32_t>(header[2]) << 16) |
                        (static_cast<uint32_t>(header[3]) << 8) |
                        static_cast<uint32_t>(header[4]);
    
    message.payload_size = (static_cast<uint32_t>(header[5]) << 24) |
                          (static_cast<uint32_t>(header[6]) << 16) |
                          (static_cast<uint32_t>(header[7]) << 8) |
                          static_cast<uint32_t>(header[8]);
    
    if (message.payload_size > 0) {
        message.payload.resize(message.payload_size);
        if (recv(socket_fd_, message.payload.data(), message.payload_size, 0) != static_cast<ssize_t>(message.payload_size)) {
            return -1;
        }
    }
    
    return 0;
}

int MQTTRouterRpcClient::send_request(MessageType type, const MQTTByteVector& payload, MQTTByteVector& response) {
    if (!connected_) {
        return -1;
    }
    
    uint32_t request_id = generate_request_id();
    
    auto pending_request = std::make_shared<PendingRequest>(allocator_);
    pending_request->request_id = request_id;
    pending_request->type = type;
    pending_request->timestamp = std::chrono::steady_clock::now();
    pending_request->completed = false;
    pending_request->error_code = 0;
    
    std::mutex request_mutex;
    std::condition_variable request_cv;
    pending_request->mutex = &request_mutex;
    pending_request->cv = &request_cv;
    
    {
        std::lock_guard<std::mutex> lock(pending_requests_mutex_);
        pending_requests_[request_id] = pending_request;
    }
    
    RpcMessage message(allocator_);
    message.type = type;
    message.request_id = request_id;
    message.payload_size = payload.size();
    message.payload = payload;
    
    if (send_message(message) != 0) {
        std::lock_guard<std::mutex> lock(pending_requests_mutex_);
        pending_requests_.erase(request_id);
        return -1;
    }
    
    std::unique_lock<std::mutex> lock(request_mutex);
    bool completed = request_cv.wait_for(lock, std::chrono::milliseconds(config_.request_timeout_ms),
                                        [&]() { return pending_request->completed; });
    
    if (!completed) {
        std::lock_guard<std::mutex> pending_lock(pending_requests_mutex_);
        pending_requests_.erase(request_id);
        return -1;
    }
    
    int error_code = pending_request->error_code;
    if (error_code == 0) {
        response = pending_request->response_payload;
    }
    
    {
        std::lock_guard<std::mutex> pending_lock(pending_requests_mutex_);
        pending_requests_.erase(request_id);
    }
    
    return error_code;
}

int MQTTRouterRpcClient::send_request_async(MessageType type, const MQTTByteVector& payload) {
    if (!connected_) {
        return -1;
    }
    
    uint32_t request_id = generate_request_id();
    
    RpcMessage message(allocator_);
    message.type = type;
    message.request_id = request_id;
    message.payload_size = payload.size();
    message.payload = payload;
    
    return send_message(message);
}

void MQTTRouterRpcClient::connection_thread_func() {
    while (!should_stop_) {
        if (!connected_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        RpcMessage message(allocator_);
        if (receive_message(message) != 0) {
            if (!should_stop_) {
                spdlog::warn("Lost connection to router, attempting to reconnect");
                connected_ = false;
                if (socket_fd_ >= 0) {
                    close(socket_fd_);
                    socket_fd_ = -1;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(config_.retry_delay_ms));
                
                if (connect_to_server() == 0) {
                    connected_ = true;
                    spdlog::info("Reconnected to router");
                }
            }
            continue;
        }
        
        std::lock_guard<std::mutex> lock(pending_requests_mutex_);
        auto it = pending_requests_.find(message.request_id);
        if (it != pending_requests_.end()) {
            auto& request = it->second;
            if (request->mutex && request->cv) {
                std::lock_guard<std::mutex> req_lock(*request->mutex);
                request->completed = true;
                request->error_code = 0;
                request->response_payload = message.payload;
                request->cv->notify_one();
            }
        }
    }
}

void MQTTRouterRpcClient::heartbeat_thread_func() {
    while (!should_stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.heartbeat_interval_ms));
        
        if (!connected_) {
            continue;
        }
        
        MQTTByteVector payload{MQTTSTLAllocator<uint8_t>(allocator_)};
        MQTTByteVector response{MQTTSTLAllocator<uint8_t>(allocator_)};
        
        if (send_request(MessageType::HEARTBEAT, payload, response) != 0) {
            spdlog::debug("Heartbeat failed");
        }
        
        cleanup_expired_requests();
    }
}

void MQTTRouterRpcClient::cleanup_expired_requests() {
    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto it = pending_requests_.begin();
    while (it != pending_requests_.end()) {
        auto& request = it->second;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - request->timestamp).count();
        
        if (elapsed > config_.request_timeout_ms) {
            if (request->mutex && request->cv) {
                std::lock_guard<std::mutex> req_lock(*request->mutex);
                request->completed = true;
                request->error_code = -1;
                request->cv->notify_one();
            }
            it = pending_requests_.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t MQTTRouterRpcClient::generate_request_id() {
    return next_request_id_++;
}

int MQTTRouterRpcClient::serialize_subscribe_request(const SubscribeRequest& request, MQTTByteVector& payload) {
    payload.clear();
    
    std::string server_id = from_mqtt_string(request.server_id);
    std::string client_id = from_mqtt_string(request.client_id);
    std::string topic_filter = from_mqtt_string(request.topic_filter);
    
    uint32_t server_id_len = server_id.length();
    uint32_t client_id_len = client_id.length();
    uint32_t topic_filter_len = topic_filter.length();
    
    payload.resize(4 + server_id_len + 4 + client_id_len + 4 + topic_filter_len + 1);
    
    size_t offset = 0;
    
    payload[offset++] = (server_id_len >> 24) & 0xFF;
    payload[offset++] = (server_id_len >> 16) & 0xFF;
    payload[offset++] = (server_id_len >> 8) & 0xFF;
    payload[offset++] = server_id_len & 0xFF;
    
    if (server_id_len > 0) {
        memcpy(payload.data() + offset, server_id.c_str(), server_id_len);
        offset += server_id_len;
    }
    
    payload[offset++] = (client_id_len >> 24) & 0xFF;
    payload[offset++] = (client_id_len >> 16) & 0xFF;
    payload[offset++] = (client_id_len >> 8) & 0xFF;
    payload[offset++] = client_id_len & 0xFF;
    
    if (client_id_len > 0) {
        memcpy(payload.data() + offset, client_id.c_str(), client_id_len);
        offset += client_id_len;
    }
    
    payload[offset++] = (topic_filter_len >> 24) & 0xFF;
    payload[offset++] = (topic_filter_len >> 16) & 0xFF;
    payload[offset++] = (topic_filter_len >> 8) & 0xFF;
    payload[offset++] = topic_filter_len & 0xFF;
    
    if (topic_filter_len > 0) {
        memcpy(payload.data() + offset, topic_filter.c_str(), topic_filter_len);
        offset += topic_filter_len;
    }
    
    payload[offset++] = request.qos;
    
    return 0;
}

int MQTTRouterRpcClient::subscribe(const SubscribeRequest& request) {
    MQTTByteVector payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    MQTTByteVector response{MQTTSTLAllocator<uint8_t>(allocator_)};
    
    if (serialize_subscribe_request(request, payload) != 0) {
        return -1;
    }
    
    total_requests_++;
    
    int result = send_request(MessageType::SUBSCRIBE, payload, response);
    if (result == 0) {
        successful_requests_++;
    } else {
        failed_requests_++;
    }
    
    return result;
}

int MQTTRouterRpcClient::subscribe_async(const SubscribeRequest& request) {
    MQTTByteVector payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    
    if (serialize_subscribe_request(request, payload) != 0) {
        return -1;
    }
    
    total_requests_++;
    
    int result = send_request_async(MessageType::SUBSCRIBE, payload);
    if (result != 0) {
        failed_requests_++;
    }
    
    return result;
}

bool MQTTRouterRpcClient::is_connected() const {
    return connected_;
}

uint64_t MQTTRouterRpcClient::get_last_error_time() const {
    return last_error_time_;
}

int MQTTRouterRpcClient::get_last_error_code() const {
    return last_error_code_;
}

const MQTTString& MQTTRouterRpcClient::get_last_error_message() const {
    return last_error_message_;
}

uint64_t MQTTRouterRpcClient::get_total_requests() const {
    return total_requests_;
}

uint64_t MQTTRouterRpcClient::get_successful_requests() const {
    return successful_requests_;
}

uint64_t MQTTRouterRpcClient::get_failed_requests() const {
    return failed_requests_;
}

// Placeholder implementations for other methods
int MQTTRouterRpcClient::serialize_unsubscribe_request(const UnsubscribeRequest& request, MQTTByteVector& payload) {
    // Similar to serialize_subscribe_request
    return 0;
}

int MQTTRouterRpcClient::serialize_client_connect_request(const ClientConnectRequest& request, MQTTByteVector& payload) {
    // Implementation for client connect serialization
    return 0;
}

int MQTTRouterRpcClient::serialize_client_disconnect_request(const ClientDisconnectRequest& request, MQTTByteVector& payload) {
    // Implementation for client disconnect serialization
    return 0;
}

int MQTTRouterRpcClient::serialize_route_publish_request(const RoutePublishRequest& request, MQTTByteVector& payload) {
    // Implementation for route publish serialization
    return 0;
}

int MQTTRouterRpcClient::deserialize_route_targets(const MQTTByteVector& payload, RouteTargetVector& targets) {
    // Implementation for route targets deserialization
    return 0;
}

int MQTTRouterRpcClient::deserialize_statistics(const MQTTByteVector& payload, RouterStatistics& stats) {
    // Implementation for statistics deserialization
    return 0;
}

int MQTTRouterRpcClient::unsubscribe(const UnsubscribeRequest& request) {
    // Implementation using serialize_unsubscribe_request
    return 0;
}

int MQTTRouterRpcClient::client_connect(const ClientConnectRequest& request) {
    // Implementation using serialize_client_connect_request
    return 0;
}

int MQTTRouterRpcClient::client_disconnect(const ClientDisconnectRequest& request) {
    // Implementation using serialize_client_disconnect_request
    return 0;
}

int MQTTRouterRpcClient::route_publish(const RoutePublishRequest& request, RouteTargetVector& targets) {
    // Implementation using serialize_route_publish_request and deserialize_route_targets
    return 0;
}

int MQTTRouterRpcClient::get_statistics(RouterStatistics& stats) {
    // Implementation using deserialize_statistics
    return 0;
}

int MQTTRouterRpcClient::unsubscribe_async(const UnsubscribeRequest& request) {
    // Implementation using serialize_unsubscribe_request
    return 0;
}

int MQTTRouterRpcClient::client_connect_async(const ClientConnectRequest& request) {
    // Implementation using serialize_client_connect_request
    return 0;
}

int MQTTRouterRpcClient::client_disconnect_async(const ClientDisconnectRequest& request) {
    // Implementation using serialize_client_disconnect_request
    return 0;
}

int MQTTRouterRpcClient::batch_subscribe(const MQTTVector<SubscribeRequest>& requests) {
    // Implementation for batch subscribe
    return 0;
}

int MQTTRouterRpcClient::batch_unsubscribe(const MQTTVector<UnsubscribeRequest>& requests) {
    // Implementation for batch unsubscribe
    return 0;
}