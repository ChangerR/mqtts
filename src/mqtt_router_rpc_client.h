#ifndef MQTT_ROUTER_RPC_CLIENT_H
#define MQTT_ROUTER_RPC_CLIENT_H

#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <queue>
#include "mqtt_allocator.h"
#include "mqtt_string_utils.h"
#include "mqtt_stl_allocator.h"
#include "co_routine.h"

using namespace mqtt;

class MQTTRouterRpcClient {
public:
    struct RouteTarget {
        MQTTString server_id;
        MQTTString client_id;
        uint8_t qos;
        
        RouteTarget(MQTTAllocator* allocator = nullptr)
            : server_id(MQTTStrAllocator(allocator))
            , client_id(MQTTStrAllocator(allocator))
            , qos(0)
        {}
    };
    
    using RouteTargetVector = MQTTVector<RouteTarget>;
    
    struct SubscribeRequest {
        MQTTString server_id;
        MQTTString client_id;
        MQTTString topic_filter;
        uint8_t qos;
        
        SubscribeRequest(MQTTAllocator* allocator = nullptr)
            : server_id(MQTTStrAllocator(allocator))
            , client_id(MQTTStrAllocator(allocator))
            , topic_filter(MQTTStrAllocator(allocator))
            , qos(0)
        {}
    };
    
    struct UnsubscribeRequest {
        MQTTString server_id;
        MQTTString client_id;
        MQTTString topic_filter;
        
        UnsubscribeRequest(MQTTAllocator* allocator = nullptr)
            : server_id(MQTTStrAllocator(allocator))
            , client_id(MQTTStrAllocator(allocator))
            , topic_filter(MQTTStrAllocator(allocator))
        {}
    };
    
    struct ClientConnectRequest {
        MQTTString server_id;
        MQTTString client_id;
        MQTTString username;
        uint32_t protocol_version;
        uint32_t keep_alive;
        bool clean_session;
        
        ClientConnectRequest(MQTTAllocator* allocator = nullptr)
            : server_id(MQTTStrAllocator(allocator))
            , client_id(MQTTStrAllocator(allocator))
            , username(MQTTStrAllocator(allocator))
            , protocol_version(4)
            , keep_alive(60)
            , clean_session(true)
        {}
    };
    
    struct ClientDisconnectRequest {
        MQTTString server_id;
        MQTTString client_id;
        MQTTString disconnect_reason;
        
        ClientDisconnectRequest(MQTTAllocator* allocator = nullptr)
            : server_id(MQTTStrAllocator(allocator))
            , client_id(MQTTStrAllocator(allocator))
            , disconnect_reason(MQTTStrAllocator(allocator))
        {}
    };
    
    struct RoutePublishRequest {
        MQTTString topic;
        MQTTByteVector payload;
        uint8_t qos;
        bool retain;
        MQTTString publisher_server_id;
        MQTTString publisher_client_id;
        
        RoutePublishRequest(MQTTAllocator* allocator = nullptr)
            : topic(MQTTStrAllocator(allocator))
            , payload(MQTTSTLAllocator<uint8_t>(allocator))
            , qos(0)
            , retain(false)
            , publisher_server_id(MQTTStrAllocator(allocator))
            , publisher_client_id(MQTTStrAllocator(allocator))
        {}
    };
    
    struct RouterStatistics {
        uint64_t total_servers;
        uint64_t total_clients;
        uint64_t total_subscriptions;
        uint64_t total_routes;
        uint64_t memory_usage_bytes;
        uint64_t redo_log_entries;
        uint64_t last_snapshot_time;
        
        RouterStatistics()
            : total_servers(0)
            , total_clients(0)
            , total_subscriptions(0)
            , total_routes(0)
            , memory_usage_bytes(0)
            , redo_log_entries(0)
            , last_snapshot_time(0)
        {}
    };

    struct RpcClientConfig {
        std::string router_host;
        int router_port;
        int connect_timeout_ms;
        int request_timeout_ms;
        int max_retry_count;
        int retry_delay_ms;
        bool enable_heartbeat;
        int heartbeat_interval_ms;
        size_t max_pending_requests;
        
        RpcClientConfig()
            : router_host("127.0.0.1")
            , router_port(9090)
            , connect_timeout_ms(5000)
            , request_timeout_ms(10000)
            , max_retry_count(3)
            , retry_delay_ms(1000)
            , enable_heartbeat(true)
            , heartbeat_interval_ms(30000)
            , max_pending_requests(1000)
        {}
    };

public:
    explicit MQTTRouterRpcClient(MQTTAllocator* allocator, const RpcClientConfig& config);
    ~MQTTRouterRpcClient();
    
    virtual int initialize();
    virtual int connect();
    virtual int disconnect();
    
    // 同步调用
    int subscribe(const SubscribeRequest& request);
    int unsubscribe(const UnsubscribeRequest& request);
    int client_connect(const ClientConnectRequest& request);
    int client_disconnect(const ClientDisconnectRequest& request);
    virtual int route_publish(const RoutePublishRequest& request, RouteTargetVector& targets);
    int get_statistics(RouterStatistics& stats);
    
    // 异步调用
    virtual int subscribe_async(const SubscribeRequest& request);
    virtual int unsubscribe_async(const UnsubscribeRequest& request);
    virtual int client_connect_async(const ClientConnectRequest& request);
    virtual int client_disconnect_async(const ClientDisconnectRequest& request);
    
    // 批量操作
    int batch_subscribe(const MQTTVector<SubscribeRequest>& requests);
    int batch_unsubscribe(const MQTTVector<UnsubscribeRequest>& requests);
    
    // 连接状态管理
    virtual bool is_connected() const;
    uint64_t get_last_error_time() const;
    int get_last_error_code() const;
    const MQTTString& get_last_error_message() const;
    
    // 统计信息
    uint64_t get_total_requests() const;
    uint64_t get_successful_requests() const;
    uint64_t get_failed_requests() const;
    
private:
    enum class MessageType : uint8_t {
        SUBSCRIBE = 1,
        UNSUBSCRIBE = 2,
        CLIENT_CONNECT = 3,
        CLIENT_DISCONNECT = 4,
        ROUTE_PUBLISH = 5,
        BATCH_SUBSCRIBE = 6,
        BATCH_UNSUBSCRIBE = 7,
        GET_STATISTICS = 8,
        HEARTBEAT = 9
    };
    
    struct RpcMessage {
        MessageType type;
        uint32_t request_id;
        uint32_t payload_size;
        MQTTByteVector payload;
        
        RpcMessage(MQTTAllocator* allocator = nullptr)
            : type(MessageType::SUBSCRIBE)
            , request_id(0)
            , payload_size(0)
            , payload(MQTTSTLAllocator<uint8_t>(allocator))
        {}
    };
    
    struct PendingRequest {
        uint32_t request_id;
        MessageType type;
        std::chrono::steady_clock::time_point timestamp;
        std::condition_variable* cv;
        std::mutex* mutex;
        bool completed;
        int error_code;
        MQTTByteVector response_payload;
        
        PendingRequest(MQTTAllocator* allocator = nullptr)
            : request_id(0)
            , type(MessageType::SUBSCRIBE)
            , cv(nullptr)
            , mutex(nullptr)
            , completed(false)
            , error_code(0)
            , response_payload(MQTTSTLAllocator<uint8_t>(allocator))
        {}
    };
    
    using PendingRequestMap = MQTTMap<uint32_t, std::shared_ptr<PendingRequest>>;
    
    int connect_to_server();
    int send_message(const RpcMessage& message);
    int receive_message(RpcMessage& message);
    
    int send_request(MessageType type, const MQTTByteVector& payload, MQTTByteVector& response);
    int send_request_async(MessageType type, const MQTTByteVector& payload);
    
    void connection_thread_func();
    void heartbeat_thread_func();
    void cleanup_expired_requests();
    
    uint32_t generate_request_id();
    
    int serialize_subscribe_request(const SubscribeRequest& request, MQTTByteVector& payload);
    int serialize_unsubscribe_request(const UnsubscribeRequest& request, MQTTByteVector& payload);
    int serialize_client_connect_request(const ClientConnectRequest& request, MQTTByteVector& payload);
    int serialize_client_disconnect_request(const ClientDisconnectRequest& request, MQTTByteVector& payload);
    int serialize_route_publish_request(const RoutePublishRequest& request, MQTTByteVector& payload);
    
    int deserialize_route_targets(const MQTTByteVector& payload, RouteTargetVector& targets);
    int deserialize_statistics(const MQTTByteVector& payload, RouterStatistics& stats);
    
    MQTTAllocator* allocator_;
    RpcClientConfig config_;
    
    int socket_fd_;
    std::atomic<bool> connected_;
    std::atomic<bool> should_stop_;
    
    std::thread connection_thread_;
    std::thread heartbeat_thread_;
    
    std::atomic<uint32_t> next_request_id_;
    PendingRequestMap pending_requests_;
    std::mutex pending_requests_mutex_;
    
    std::atomic<uint64_t> last_error_time_;
    std::atomic<int> last_error_code_;
    MQTTString last_error_message_;
    std::mutex error_mutex_;
    
    std::atomic<uint64_t> total_requests_;
    std::atomic<uint64_t> successful_requests_;
    std::atomic<uint64_t> failed_requests_;
    
    MQTTString server_id_;
};

#endif // MQTT_ROUTER_RPC_CLIENT_H