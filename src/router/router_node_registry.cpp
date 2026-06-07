#include "mqtt_router_service.h"

namespace
{

uint64_t get_registry_current_time_ms()
{
    uint64_t now_ms = 0;
    now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return now_ms;
}

}  // namespace

RouterNodeRegistry::RouterNodeRegistry(MQTTAllocator* allocator)
    : allocator_(allocator)
    , node_session_ttl_ms_(120000)
    , server_info_map_(MQTTSTLAllocator<std::pair<const MQTTString, ServerInfo> >(allocator))
    , registry_mutex_()
{
}

RouterNodeRegistry::~RouterNodeRegistry()
{
}

int RouterNodeRegistry::initialize(int node_session_ttl_ms)
{
    int ret = MQ_SUCCESS;

    if (node_session_ttl_ms <= 0) {
        ret = MQ_ERR_INVALID_ARGS;
    } else {
        node_session_ttl_ms_ = node_session_ttl_ms;
    }

    return ret;
}

int RouterNodeRegistry::upsert_node(const ServerInfo& server_info)
{
    int ret = MQ_SUCCESS;
    std::lock_guard<std::mutex> guard(registry_mutex_);
    server_info_map_[server_info.server_id] = server_info;
    return ret;
}

int RouterNodeRegistry::remove_node(const MQTTString& server_id)
{
    int ret = MQ_SUCCESS;
    std::lock_guard<std::mutex> guard(registry_mutex_);

    if (server_info_map_.erase(server_id) <= 0) {
        ret = MQ_ERR_NOT_FOUND_V2;
    }

    return ret;
}

int RouterNodeRegistry::get_node(const MQTTString& server_id, ServerInfo& server_info)
{
    int ret = MQ_SUCCESS;
    uint64_t now_ms = 0;
    ServerInfoMap::iterator it;
    std::lock_guard<std::mutex> guard(registry_mutex_);

    now_ms = get_registry_current_time_ms();
    it = server_info_map_.find(server_id);
    if (it == server_info_map_.end()) {
        ret = MQ_ERR_NOT_FOUND_V2;
    } else if (MQ_FAIL(check_node_expired_(it->second, now_ms))) {
        if (MQ_ERR_ROUTER_NODE_INACTIVE == ret) {
            server_info = it->second;
        }
    } else {
        server_info = it->second;
    }

    return ret;
}

int RouterNodeRegistry::get_active_nodes(ServerInfoMap& server_info_map)
{
    int ret = MQ_SUCCESS;
    uint64_t now_ms = 0;
    std::lock_guard<std::mutex> guard(registry_mutex_);

    now_ms = get_registry_current_time_ms();
    server_info_map.clear();
    for (ServerInfoMap::iterator it = server_info_map_.begin();
         it != server_info_map_.end();
         ++it) {
        if (MQ_SUCCESS == check_node_expired_(it->second, now_ms)) {
            server_info_map[it->first] = it->second;
        }
    }

    return ret;
}

int RouterNodeRegistry::mark_node_inactive(const MQTTString& server_id)
{
    int ret = MQ_SUCCESS;
    ServerInfoMap::iterator it;
    std::lock_guard<std::mutex> guard(registry_mutex_);

    it = server_info_map_.find(server_id);
    if (it == server_info_map_.end()) {
        ret = MQ_ERR_NOT_FOUND_V2;
    } else {
        it->second.is_active = false;
    }

    return ret;
}

int RouterNodeRegistry::get_node_count(size_t& node_count) const
{
    int ret = MQ_SUCCESS;
    std::lock_guard<std::mutex> guard(registry_mutex_);
    node_count = server_info_map_.size();
    return ret;
}

int RouterNodeRegistry::check_node_expired_(ServerInfo& server_info, uint64_t now_ms) const
{
    int ret = MQ_SUCCESS;
    uint64_t delta_ms = 0;

    if (!server_info.is_active) {
        ret = MQ_ERR_ROUTER_NODE_INACTIVE;
    } else if (server_info.last_heartbeat_ms <= 0) {
        server_info.is_active = false;
        ret = MQ_ERR_ROUTER_NODE_INACTIVE;
    } else {
        delta_ms = (now_ms >= server_info.last_heartbeat_ms)
                     ? (now_ms - server_info.last_heartbeat_ms)
                     : 0;
        if (delta_ms > static_cast<uint64_t>(node_session_ttl_ms_)) {
            server_info.is_active = false;
            ret = MQ_ERR_ROUTER_NODE_INACTIVE;
        }
    }

    return ret;
}
