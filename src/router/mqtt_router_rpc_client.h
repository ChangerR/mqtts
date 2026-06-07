#ifndef MQTT_ROUTER_RPC_CLIENT_H
#define MQTT_ROUTER_RPC_CLIENT_H

#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include "mqtt_allocator.h"
#include "mqtt_coroutine_utils.h"
#include "mqtt_define.h"
#include "mqtt_string_utils.h"
#include "mqtt_stl_allocator.h"
#include "mqtt_socket.h"
#include "co_routine.h"

using namespace mqtt;

struct ClusterConfig {
    bool cluster_enabled;
    std::string server_id;
    std::string server_token;
    std::string router_host;
    int router_port;
    std::string forwarding_host;
    int forwarding_port;
    int heartbeat_interval_ms;
    int request_timeout_ms;
    int max_inflight_forward_requests;
    int forwarding_pool_size;        // 每个远端保持的连接数，默认 4
    int forwarding_idle_timeout_ms;  // 连接空闲超时（毫秒），默认 30000

    ClusterConfig()
        : cluster_enabled(false)
        , server_id()
        , server_token()
        , router_host("127.0.0.1")
        , router_port(9090)
        , forwarding_host("127.0.0.1")
        , forwarding_port(10090)
        , heartbeat_interval_ms(30000)
        , request_timeout_ms(10000)
        , max_inflight_forward_requests(1024)
        , forwarding_pool_size(4)
        , forwarding_idle_timeout_ms(30000)
    {}
};

class MQTTRouterRpcClient {
public:
    struct NodeAuthRequest {
        MQTTString server_id;
        MQTTString token;
        uint64_t timestamp_ms;
        MQTTString nonce;
        MQTTString host;
        int port;
        int forwarding_port;

        explicit NodeAuthRequest(MQTTAllocator* allocator = nullptr)
            : server_id(MQTTStrAllocator(allocator))
            , token(MQTTStrAllocator(allocator))
            , timestamp_ms(0)
            , nonce(MQTTStrAllocator(allocator))
            , host(MQTTStrAllocator(allocator))
            , port(0)
            , forwarding_port(0)
        {}
    };

    struct NodeAuthResponse {
        int error_code;
        MQTTString session_id;
        uint64_t expires_at_ms;

        explicit NodeAuthResponse(MQTTAllocator* allocator = nullptr)
            : error_code(MQ_SUCCESS)
            , session_id(MQTTStrAllocator(allocator))
            , expires_at_ms(0)
        {}
    };

    struct RouteTarget {
        MQTTString server_id;
        MQTTString client_id;
        MQTTString host;
        int port;
        int forwarding_port;
        uint8_t qos;
        
        RouteTarget(MQTTAllocator* allocator = nullptr)
            : server_id(MQTTStrAllocator(allocator))
            , client_id(MQTTStrAllocator(allocator))
            , host(MQTTStrAllocator(allocator))
            , port(0)
            , forwarding_port(0)
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
        uint64_t message_id;
        MQTTString trace_id;
        
        RoutePublishRequest(MQTTAllocator* allocator = nullptr)
            : topic(MQTTStrAllocator(allocator))
            , payload(MQTTSTLAllocator<uint8_t>(allocator))
            , qos(0)
            , retain(false)
            , publisher_server_id(MQTTStrAllocator(allocator))
            , publisher_client_id(MQTTStrAllocator(allocator))
            , message_id(0)
            , trace_id(MQTTStrAllocator(allocator))
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
    virtual int set_cluster_config(const ClusterConfig& config);
    virtual int authenticate_node(const NodeAuthRequest& request, NodeAuthResponse& response);
    virtual int heartbeat();
    
    // 同步调用
    virtual int subscribe(const SubscribeRequest& request);
    virtual int unsubscribe(const UnsubscribeRequest& request);
    virtual int client_connect(const ClientConnectRequest& request);
    virtual int client_disconnect(const ClientDisconnectRequest& request);
    virtual int route_publish(const RoutePublishRequest& request, RouteTargetVector& targets);
    virtual int get_statistics(RouterStatistics& stats);
    
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
    virtual int get_connection_state(bool& is_connected) const;
    uint64_t get_last_error_time() const;
    int get_last_error_code() const;
    const MQTTString& get_last_error_message() const;
    
    // 统计信息
    uint64_t get_total_requests() const;
    uint64_t get_successful_requests() const;
    uint64_t get_failed_requests() const;
    
private:
    enum class MessageType : uint8_t {
        AUTHENTICATE_NODE = 1,
        SUBSCRIBE = 2,
        UNSUBSCRIBE = 3,
        CLIENT_CONNECT = 4,
        CLIENT_DISCONNECT = 5,
        ROUTE_PUBLISH = 6,
        BATCH_SUBSCRIBE = 7,
        BATCH_UNSUBSCRIBE = 8,
        GET_STATISTICS = 9,
        HEARTBEAT = 10
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
    
    int connect_to_server();
    void destroy_socket();
    int send_message(const RpcMessage& message);
    int receive_message(RpcMessage& message);
    
    int send_request(MessageType type, const MQTTByteVector& payload, MQTTByteVector& response);
    int send_request_async(MessageType type, const MQTTByteVector& payload);
    
    static void* heartbeat_routine(void* arg);
    void heartbeat_coroutine_main();
    
    uint32_t generate_request_id();
    
    int serialize_subscribe_request(const SubscribeRequest& request, MQTTByteVector& payload);
    int serialize_unsubscribe_request(const UnsubscribeRequest& request, MQTTByteVector& payload);
    int serialize_client_connect_request(const ClientConnectRequest& request, MQTTByteVector& payload);
    int serialize_client_disconnect_request(const ClientDisconnectRequest& request, MQTTByteVector& payload);
    int serialize_route_publish_request(const RoutePublishRequest& request, MQTTByteVector& payload);
    int serialize_authenticate_node_request(const NodeAuthRequest& request, MQTTByteVector& payload);
    
    int deserialize_route_targets(const MQTTByteVector& payload, RouteTargetVector& targets);
    int deserialize_statistics(const MQTTByteVector& payload, RouterStatistics& stats);
    int deserialize_authenticate_node_response(const MQTTByteVector& payload, NodeAuthResponse& response);
    
    MQTTAllocator* allocator_;
    RpcClientConfig config_;
    
    MQTTSocket* socket_;
    std::atomic<bool> connected_;
    std::atomic<bool> should_stop_;
    stCoRoutine_t* heartbeat_coroutine_;
    CoroCondition heartbeat_cond_;
    
    std::atomic<uint32_t> next_request_id_;
    CoroMutex request_mutex_;
    
    std::atomic<uint64_t> last_error_time_;
    std::atomic<int> last_error_code_;
    MQTTString last_error_message_;
    
    std::atomic<uint64_t> total_requests_;
    std::atomic<uint64_t> successful_requests_;
    std::atomic<uint64_t> failed_requests_;
    
    MQTTString server_id_;
    MQTTString server_token_;
    MQTTString forwarding_host_;
    MQTTString session_id_;
    int forwarding_port_;
};

#endif // MQTT_ROUTER_RPC_CLIENT_H
