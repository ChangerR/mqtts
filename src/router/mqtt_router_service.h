#ifndef MQTT_ROUTER_SERVICE_H
#define MQTT_ROUTER_SERVICE_H

#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include "mqtt_allocator.h"
#include "mqtt_define.h"
#include "mqtt_topic_tree.h"
#include "mqtt_event_types.h"
#include "mqtt_string_utils.h"
#include "mqtt_stl_allocator.h"
#include "co_routine.h"
#include "mqtt_router_rpc_client.h"

using namespace mqtt;

struct MQTTRouterConfig {
    std::string service_host;
    int service_port;
    std::string redo_log_path;
    std::string snapshot_path;
    int snapshot_interval_seconds;
    int redo_log_flush_interval_ms;
    int max_redo_log_entries;
    int worker_thread_count;
    int coroutines_per_thread;
    size_t max_memory_limit;
    int auth_timestamp_tolerance_ms;
    int node_session_ttl_ms;
    std::unordered_map<std::string, std::string> node_token_map;
    
    MQTTRouterConfig() 
        : service_host("127.0.0.1")
        , service_port(9090)
        , redo_log_path("./mqtt_router.redo")
        , snapshot_path("./mqtt_router.snapshot")
        , snapshot_interval_seconds(300)
        , redo_log_flush_interval_ms(1000)
        , max_redo_log_entries(10000)
        , worker_thread_count(4)
        , coroutines_per_thread(8)
        , max_memory_limit(1024 * 1024 * 1024)
        , auth_timestamp_tolerance_ms(30000)
        , node_session_ttl_ms(120000)
        , node_token_map()
    {}
};

enum class RouterLogOpType : uint8_t {
    SUBSCRIBE = 1,
    UNSUBSCRIBE = 2,
    UNSUBSCRIBE_ALL = 3,
    CLIENT_CONNECT = 4,
    CLIENT_DISCONNECT = 5
};

struct RouterLogEntry {
    RouterLogOpType op_type;
    uint64_t sequence_id;
    uint64_t timestamp_ms;
    MQTTString server_id;
    MQTTString client_id;
    MQTTString topic_filter;
    uint8_t qos;
    
    RouterLogEntry(MQTTAllocator* allocator = nullptr)
        : op_type(RouterLogOpType::SUBSCRIBE)
        , sequence_id(0)
        , timestamp_ms(0)
        , server_id(MQTTStrAllocator(allocator))
        , client_id(MQTTStrAllocator(allocator))
        , topic_filter(MQTTStrAllocator(allocator))
        , qos(0)
    {}
};

using RouterLogEntryVector = MQTTVector<RouterLogEntry>;

struct ServerInfo {
    MQTTString server_id;
    MQTTString host;
    int port;
    int forwarding_port;
    uint64_t last_heartbeat_ms;
    bool is_active;
    MQTTString session_id;
    uint64_t client_count;
    uint64_t subscription_count;
    
    ServerInfo(MQTTAllocator* allocator = nullptr)
        : server_id(MQTTStrAllocator(allocator))
        , host(MQTTStrAllocator(allocator))
        , port(0)
        , forwarding_port(0)
        , last_heartbeat_ms(0)
        , is_active(false)
        , session_id(MQTTStrAllocator(allocator))
        , client_count(0)
        , subscription_count(0)
    {}
};

using ServerInfoMap = MQTTMap<MQTTString, ServerInfo>;

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

class MQTTPersistentTopicTree {
public:
    explicit MQTTPersistentTopicTree(MQTTAllocator* allocator);
    ~MQTTPersistentTopicTree();
    
    int subscribe(const MQTTString& server_id, const MQTTString& topic_filter, 
                  const MQTTString& client_id, uint8_t qos);
    int unsubscribe(const MQTTString& server_id, const MQTTString& topic_filter, 
                    const MQTTString& client_id);
    int unsubscribe_all(const MQTTString& server_id, const MQTTString& client_id);
    
    int find_route_targets(const MQTTString& topic, RouteTargetVector& targets);
    int find_route_targets(const MQTTString& topic,
                           const MQTTString& publisher_server_id,
                           RouteTargetVector& targets);
    
    int load_from_snapshot(const std::string& snapshot_path);
    int save_to_snapshot(const std::string& snapshot_path);
    
    int apply_redo_log_entry(const RouterLogEntry& entry);
    int get_statistics(size_t& total_servers, size_t& total_clients, size_t& total_subscriptions);
    
private:
    using SubscriptionEntryMap = MQTTMap<MQTTString, RouterLogEntry>;

    MQTTAllocator* allocator_;
    std::unique_ptr<ConcurrentTopicTree> topic_tree_;
    SubscriptionEntryMap subscription_entry_map_;
    std::mutex snapshot_mutex_;
};

class RouterNodeRegistry {
public:
    explicit RouterNodeRegistry(MQTTAllocator* allocator);
    ~RouterNodeRegistry();

    int initialize(int node_session_ttl_ms);
    int upsert_node(const ServerInfo& server_info);
    int remove_node(const MQTTString& server_id);
    int get_node(const MQTTString& server_id, ServerInfo& server_info);
    int get_active_nodes(ServerInfoMap& server_info_map);
    int mark_node_inactive(const MQTTString& server_id);
    int get_node_count(size_t& node_count) const;

private:
    int check_node_expired_(ServerInfo& server_info, uint64_t now_ms) const;

    MQTTAllocator* allocator_;
    int node_session_ttl_ms_;
    ServerInfoMap server_info_map_;
    mutable std::mutex registry_mutex_;
};

class RouterAuthStore {
public:
    explicit RouterAuthStore(MQTTAllocator* allocator);
    ~RouterAuthStore();

    int initialize(const MQTTRouterConfig& config);
    int validate_node_token(const MQTTRouterRpcClient::NodeAuthRequest& request, MQTTString& session_id);
    int get_session_expires_at_ms(uint64_t now_ms, uint64_t& expires_at_ms) const;
    int validate_session_request(const MQTTString& authenticated_server_id,
                                 const MQTTString& request_server_id) const;

private:
    using TokenMap = MQTTMap<MQTTString, MQTTString>;
    using NonceSet = TopicTreeSet<MQTTString>;

    MQTTAllocator* allocator_;
    int auth_timestamp_tolerance_ms_;
    int node_session_ttl_ms_;
    size_t max_nonce_count_;
    TokenMap token_map_;
    mutable std::mutex auth_mutex_;
    NonceSet recent_nonce_set_;
};

class MQTTRedoLogManager {
public:
    explicit MQTTRedoLogManager(MQTTAllocator* allocator, const MQTTRouterConfig& config);
    ~MQTTRedoLogManager();
    
    int initialize();
    int append_log_entry(const RouterLogEntry& entry);
    int flush_to_disk();
    int load_and_replay(MQTTPersistentTopicTree* topic_tree);
    int truncate_after_snapshot(uint64_t last_applied_sequence);
    
    uint64_t get_next_sequence_id();
    size_t get_pending_entries_count();
    
private:
    int write_log_entry_to_file(const RouterLogEntry& entry);
    int read_log_entry_from_file(std::ifstream& log_file, RouterLogEntry& entry);
    
    MQTTAllocator* allocator_;
    MQTTRouterConfig config_;
    std::string redo_log_path_;
    RouterLogEntryVector pending_entries_;
    std::atomic<uint64_t> next_sequence_id_;
    std::atomic<uint64_t> last_flushed_sequence_;
    std::mutex log_mutex_;
    std::unique_ptr<std::ofstream> log_file_;
    std::thread flush_thread_;
    std::atomic<bool> should_stop_flush_thread_;
};

class MQTTRouterRpcHandler {
public:
    explicit MQTTRouterRpcHandler(MQTTPersistentTopicTree* topic_tree, 
                                  RouterNodeRegistry* node_registry,
                                  RouterAuthStore* auth_store,
                                  MQTTRedoLogManager* redo_log_manager,
                                  MQTTAllocator* allocator);
    ~MQTTRouterRpcHandler();
    
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
    
    struct PublishRequest {
        MQTTString topic;
        MQTTByteVector payload;
        uint8_t qos;
        bool retain;
        MQTTString publisher_server_id;
        MQTTString publisher_client_id;
        uint64_t message_id;
        MQTTString trace_id;
        
        PublishRequest(MQTTAllocator* allocator = nullptr)
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
    
    struct ClientConnectRequest {
        MQTTString server_id;
        MQTTString client_id;
        
        ClientConnectRequest(MQTTAllocator* allocator = nullptr)
            : server_id(MQTTStrAllocator(allocator))
            , client_id(MQTTStrAllocator(allocator))
        {}
    };
    
    struct ClientDisconnectRequest {
        MQTTString server_id;
        MQTTString client_id;
        
        ClientDisconnectRequest(MQTTAllocator* allocator = nullptr)
            : server_id(MQTTStrAllocator(allocator))
            , client_id(MQTTStrAllocator(allocator))
        {}
    };
    
    struct RouteResponse {
        RouteTargetVector targets;
        int error_code;
        
        RouteResponse(MQTTAllocator* allocator = nullptr)
            : targets(MQTTSTLAllocator<RouteTarget>(allocator))
            , error_code(0)
        {}
    };
    
    int handle_subscribe(const SubscribeRequest& request);
    int handle_unsubscribe(const UnsubscribeRequest& request);
    int handle_client_connect(const ClientConnectRequest& request);
    int handle_client_disconnect(const ClientDisconnectRequest& request);
    int handle_publish_route(const PublishRequest& request, RouteResponse& response);
    int handle_authenticate_node(const MQTTRouterRpcClient::NodeAuthRequest& request,
                                 MQTTRouterRpcClient::NodeAuthResponse& response);
    int handle_heartbeat(const ServerInfo& server_info);
    
private:
    MQTTPersistentTopicTree* topic_tree_;
    RouterNodeRegistry* node_registry_;
    RouterAuthStore* auth_store_;
    MQTTRedoLogManager* redo_log_manager_;
    MQTTAllocator* allocator_;
};

class MQTTRouterService {
public:
    explicit MQTTRouterService(const MQTTRouterConfig& config);
    ~MQTTRouterService();
    
    int initialize();
    int start();
    int stop();
    
    int get_statistics(size_t& total_servers, size_t& total_clients, size_t& total_subscriptions);
    
private:
    struct RouterConnectionContext {
        bool authenticated;
        MQTTString authenticated_server_id;
        MQTTString session_id;

        explicit RouterConnectionContext(MQTTAllocator* allocator = nullptr)
            : authenticated(false)
            , authenticated_server_id(MQTTStrAllocator(allocator))
            , session_id(MQTTStrAllocator(allocator))
        {}
    };

    void worker_thread_func(int thread_id);
    void snapshot_thread_func();
    void handle_client_connection(int client_fd);
    
    static void* client_coroutine_func(void* arg);
    
    struct ClientContext {
        MQTTRouterService* service;
        int client_fd;
        int thread_id;
        stCoRoutine_t* coroutine;
        MQTTAllocator* allocator;
        RouterConnectionContext connection_ctx;
        
        ClientContext(MQTTRouterService* svc, int fd, int tid, MQTTAllocator* alloc)
            : service(svc)
            , client_fd(fd)
            , thread_id(tid)
            , coroutine(nullptr)
            , allocator(alloc)
            , connection_ctx(alloc)
        {}
    };
    
    MQTTRouterConfig config_;
    MQTTAllocator* root_allocator_;
    std::unique_ptr<MQTTPersistentTopicTree> topic_tree_;
    std::unique_ptr<RouterNodeRegistry> node_registry_;
    std::unique_ptr<RouterAuthStore> auth_store_;
    std::unique_ptr<MQTTRedoLogManager> redo_log_manager_;
    std::unique_ptr<MQTTRouterRpcHandler> rpc_handler_;
    
    std::vector<std::thread> worker_threads_;
    std::thread snapshot_thread_;
    std::atomic<bool> should_stop_;
    
    int listen_fd_;
    
    // Thread-local data
    static thread_local std::vector<ClientContext*> thread_local_clients_;
    static thread_local MQTTAllocator* thread_local_allocator_;
};

#endif // MQTT_ROUTER_SERVICE_H
