#include "mqtt_router_service.h"
#include "logger.h"
#include <atomic>
#include <chrono>

namespace
{

uint64_t get_current_time_ms()
{
    uint64_t now_ms = 0;
    now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return now_ms;
}

}  // namespace

RouterAuthStore::RouterAuthStore(MQTTAllocator* allocator)
    : allocator_(allocator)
    , auth_timestamp_tolerance_ms_(30000)
    , node_session_ttl_ms_(60000)
    , max_nonce_count_(1024)
    , token_map_(MQTTSTLAllocator<std::pair<const MQTTString, MQTTString> >(allocator))
    , auth_mutex_()
    , recent_nonce_set_(MQTTSTLAllocator<MQTTString>(allocator))
{
}

RouterAuthStore::~RouterAuthStore()
{
}

int RouterAuthStore::initialize(const MQTTRouterConfig& config)
{
    int ret = MQ_SUCCESS;
    size_t token_count = 0;

    auth_timestamp_tolerance_ms_ = config.auth_timestamp_tolerance_ms;
    node_session_ttl_ms_ = config.node_session_ttl_ms;
    token_map_.clear();
    recent_nonce_set_.clear();

    for (std::unordered_map<std::string, std::string>::const_iterator it = config.node_token_map.begin();
         MQ_SUCCESS == ret && it != config.node_token_map.end();
         ++it) {
        MQTTString server_id = to_mqtt_string(it->first, allocator_);
        MQTTString token = to_mqtt_string(it->second, allocator_);
        token_map_[server_id] = token;
    }

    token_count = config.node_token_map.size();
    max_nonce_count_ = (token_count > 0) ? (token_count * 16) : 1024;
    if (max_nonce_count_ < 1024) {
        max_nonce_count_ = 1024;
    }

    return ret;
}

int RouterAuthStore::validate_node_token(const MQTTRouterRpcClient::NodeAuthRequest& request,
                                         MQTTString& session_id)
{
    int ret = MQ_SUCCESS;
    uint64_t now_ms = 0;
    uint64_t request_ts = request.timestamp_ms;
    uint64_t delta_ms = 0;
    TokenMap::const_iterator token_it;
    static std::atomic<uint64_t> session_seq(1);
    uint64_t local_seq = 0;

    std::lock_guard<std::mutex> guard(auth_mutex_);

    now_ms = get_current_time_ms();
    delta_ms = (now_ms >= request_ts) ? (now_ms - request_ts) : (request_ts - now_ms);

    if (request.server_id.empty() || request.token.empty() || request.nonce.empty()) {
        ret = MQ_ERR_INVALID_ARGS;
        LOG_WARN("Router auth request has empty mandatory fields");
    } else if (delta_ms > static_cast<uint64_t>(auth_timestamp_tolerance_ms_)) {
        ret = MQ_ERR_AUTH_TOKEN_EXPIRED;
        LOG_WARN("Router auth request timestamp expired, server_id={}", from_mqtt_string(request.server_id));
    } else if (recent_nonce_set_.find(request.nonce) != recent_nonce_set_.end()) {
        ret = MQ_ERR_AUTH_NONCE_REPLAY;
        LOG_WARN("Router auth nonce replay detected, server_id={}", from_mqtt_string(request.server_id));
    } else {
        token_it = token_map_.find(request.server_id);
        if (token_it == token_map_.end()) {
            ret = MQ_ERR_ROUTER_NODE_NOT_FOUND;
            LOG_WARN("Router auth server id not found, server_id={}", from_mqtt_string(request.server_id));
        } else if (token_it->second != request.token) {
            ret = MQ_ERR_AUTH_TOKEN_INVALID;
            LOG_WARN("Router auth token mismatch, server_id={}", from_mqtt_string(request.server_id));
        } else {
            if (recent_nonce_set_.size() >= max_nonce_count_) {
                recent_nonce_set_.clear();
            }
            recent_nonce_set_.insert(request.nonce);
            local_seq = session_seq.fetch_add(1);
            session_id = to_mqtt_string(
                from_mqtt_string(request.server_id) + "#" + std::to_string(local_seq), allocator_);
        }
    }

    return ret;
}

int RouterAuthStore::get_session_expires_at_ms(uint64_t now_ms, uint64_t& expires_at_ms) const
{
    int ret = MQ_SUCCESS;

    if (node_session_ttl_ms_ <= 0) {
        ret = MQ_ERR_INVALID_STATE;
    } else {
        expires_at_ms = now_ms + static_cast<uint64_t>(node_session_ttl_ms_);
    }

    return ret;
}

int RouterAuthStore::validate_session_request(const MQTTString& authenticated_server_id,
                                              const MQTTString& request_server_id) const
{
    int ret = MQ_SUCCESS;

    if (authenticated_server_id.empty() || request_server_id.empty()) {
        ret = MQ_ERR_ROUTER_SESSION_INVALID;
    } else if (authenticated_server_id != request_server_id) {
        ret = MQ_ERR_ROUTER_SERVER_ID_MISMATCH;
    }

    return ret;
}
