#ifndef MQTT_ROUTER_SERVICE_H
#define MQTT_ROUTER_SERVICE_H

#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include "mqtt_allocator.h"
#include "mqtt_topic_tree.h"
#include "mqtt_event_types.h"
#include "mqtt_string_utils.h"
#include "mqtt_stl_allocator.h"
#include "co_routine.h"

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
    uint64_t last_heartbeat_ms;
    bool is_active;
    
    ServerInfo(MQTTAllocator* allocator = nullptr)
        : server_id(MQTTStrAllocator(allocator))
        , host(MQTTStrAllocator(allocator))
        , port(0)
        , last_heartbeat_ms(0)
        , is_active(false)
    {}
};

using ServerInfoMap = MQTTMap<MQTTString, ServerInfo>;

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
    
    int load_from_snapshot(const std::string& snapshot_path);
    int save_to_snapshot(const std::string& snapshot_path);
    
    int apply_redo_log_entry(const RouterLogEntry& entry);
    int get_statistics(size_t& total_servers, size_t& total_clients, size_t& total_subscriptions);
    
private:
    MQTTAllocator* allocator_;
    std::unique_ptr<ConcurrentTopicTree> topic_tree_;
    ServerInfoMap server_info_map_;
    std::mutex snapshot_mutex_;
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
        
        PublishRequest(MQTTAllocator* allocator = nullptr)
            : topic(MQTTStrAllocator(allocator))
            , payload(MQTTSTLAllocator<uint8_t>(allocator))
            , qos(0)
            , retain(false)
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
    
private:
    MQTTPersistentTopicTree* topic_tree_;
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
        
        ClientContext(MQTTRouterService* svc, int fd, int tid, MQTTAllocator* alloc)
            : service(svc), client_fd(fd), thread_id(tid), coroutine(nullptr), allocator(alloc) {}
    };
    
    MQTTRouterConfig config_;
    MQTTAllocator* root_allocator_;
    std::unique_ptr<MQTTPersistentTopicTree> topic_tree_;
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