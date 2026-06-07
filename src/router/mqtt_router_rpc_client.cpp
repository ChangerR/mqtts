#include "mqtt_router_rpc_client.h"
#include "logger.h"
#include "mqtt_router.pb.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>

namespace
{

int send_all_bytes(MQTTSocket* socket, const void* buf, size_t len)
{
    int ret = MQ_SUCCESS;
    int send_len = 0;
    const char* data = static_cast<const char*>(buf);

    if (MQ_ISNULL(socket) || MQ_ISNULL(buf)) {
        ret = MQ_ERR_INVALID_ARGS;
    } else if (len > static_cast<size_t>(std::numeric_limits<int>::max())) {
        ret = MQ_ERR_INVALID_ARGS;
    } else {
        send_len = static_cast<int>(len);
        ret = socket->send(reinterpret_cast<const uint8_t*>(data), send_len);
    }

    return ret;
}

int recv_all_bytes(MQTTSocket* socket, void* buf, size_t len)
{
    int ret = MQ_SUCCESS;
    size_t total_recv_len = 0;
    int recv_len = 0;
    char* data = static_cast<char*>(buf);

    if (MQ_ISNULL(socket) || MQ_ISNULL(buf)) {
        ret = MQ_ERR_INVALID_ARGS;
    } else if (len > static_cast<size_t>(std::numeric_limits<int>::max())) {
        ret = MQ_ERR_INVALID_ARGS;
    } else {
        while (MQ_SUCCESS == ret && total_recv_len < len) {
            recv_len = static_cast<int>(len - total_recv_len);
            ret = socket->recv(data + total_recv_len, recv_len);
            if (MQ_SUCC(ret)) {
                total_recv_len += static_cast<size_t>(recv_len);
            }
        }
    }

    return ret;
}

uint64_t current_time_ms()
{
    uint64_t now_ms = 0;
    now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return now_ms;
}

}  // namespace

MQTTRouterRpcClient::MQTTRouterRpcClient(MQTTAllocator* allocator, const RpcClientConfig& config)
    : allocator_(allocator)
    , config_(config)
    , socket_(NULL)
    , connected_(false)
    , should_stop_(false)
    , heartbeat_coroutine_(NULL)
    , next_request_id_(1)
    , last_error_time_(0)
    , last_error_code_(MQ_SUCCESS)
    , last_error_message_(MQTTStrAllocator(allocator))
    , total_requests_(0)
    , successful_requests_(0)
    , failed_requests_(0)
    , server_id_(MQTTStrAllocator(allocator))
    , server_token_(MQTTStrAllocator(allocator))
    , forwarding_host_(MQTTStrAllocator(allocator))
    , session_id_(MQTTStrAllocator(allocator))
    , forwarding_port_(0)
{
}

MQTTRouterRpcClient::~MQTTRouterRpcClient()
{
    (void)disconnect();
}

int MQTTRouterRpcClient::initialize()
{
    int ret = MQ_SUCCESS;
    return ret;
}

int MQTTRouterRpcClient::set_cluster_config(const ClusterConfig& config)
{
    int ret = MQ_SUCCESS;

    config_.router_host = config.router_host;
    config_.router_port = config.router_port;
    config_.request_timeout_ms = config.request_timeout_ms;
    config_.heartbeat_interval_ms = config.heartbeat_interval_ms;
    forwarding_port_ = config.forwarding_port;
    server_id_ = to_mqtt_string(config.server_id, allocator_);
    server_token_ = to_mqtt_string(config.server_token, allocator_);
    forwarding_host_ = to_mqtt_string(config.forwarding_host, allocator_);

    return ret;
}

int MQTTRouterRpcClient::connect()
{
    int ret = MQ_SUCCESS;
    stCoRoutineAttr_t attr;
    std::memset(&attr, 0, sizeof(attr));

    if (connected_.load()) {
        ret = MQ_SUCCESS;
    } else if (MQ_FAIL(connect_to_server())) {
        LOG_ERROR("Failed to connect router server, ret={}", ret);
    } else {
        connected_.store(true);
        should_stop_.store(false);
        if (config_.enable_heartbeat && NULL == heartbeat_coroutine_) {
            attr.stack_size = 128 * 1024;
            ret = co_create(&heartbeat_coroutine_, &attr, heartbeat_routine, this);
            if (MQ_FAIL(ret)) {
                LOG_ERROR("Failed to create router heartbeat coroutine, ret={}", ret);
                ret = MQ_ERR_MEMORY_ALLOC;
                connected_.store(false);
                destroy_socket();
            } else {
                co_resume(heartbeat_coroutine_);
            }
        }
    }

    return ret;
}

int MQTTRouterRpcClient::disconnect()
{
    int ret = MQ_SUCCESS;

    should_stop_.store(true);
    connected_.store(false);
    heartbeat_cond_.broadcast();

    destroy_socket();

    if (NULL != heartbeat_coroutine_) {
        co_release(heartbeat_coroutine_);
        heartbeat_coroutine_ = NULL;
    }

    return ret;
}

void MQTTRouterRpcClient::destroy_socket()
{
    MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();

    if (NULL != socket_) {
        socket_->close();
        socket_->~MQTTSocket();
        if (NULL != root_allocator) {
            root_allocator->deallocate(socket_, sizeof(MQTTSocket));
        }
        socket_ = NULL;
    }
}

int MQTTRouterRpcClient::connect_to_server()
{
    int ret = MQ_SUCCESS;
    MQTTSocket* socket = NULL;

    destroy_socket();
    if (MQ_FAIL(MQTTSocket::create_tcp_socket(socket))) {
        LOG_ERROR("Failed to create router socket, ret={}", ret);
    } else if (MQ_FAIL(socket->connect(config_.router_host.c_str(), config_.router_port))) {
        LOG_ERROR("Failed to connect router socket to {}:{}, ret={}", config_.router_host,
                  config_.router_port, ret);
        socket->~MQTTSocket();
        MQTTMemoryManager::get_instance().get_root_allocator()->deallocate(socket,
                                                                           sizeof(MQTTSocket));
    } else {
        socket_ = socket;
    }

    return ret;
}

int MQTTRouterRpcClient::send_message(const RpcMessage& message)
{
    int ret = MQ_SUCCESS;
    uint32_t payload_len_n = 0;

    if (!connected_.load() || NULL == socket_) {
        ret = MQ_ERR_ROUTER_NOT_CONNECTED;
    } else {
        payload_len_n = htonl(message.payload_size);
        if (MQ_FAIL(send_all_bytes(socket_, &payload_len_n, sizeof(payload_len_n)))) {
            LOG_ERROR("Failed to send router message header, ret={}", ret);
        } else if (message.payload_size > 0 &&
                   MQ_FAIL(send_all_bytes(socket_, message.payload.data(), message.payload_size))) {
            LOG_ERROR("Failed to send router message payload, ret={}", ret);
        }
    }

    return ret;
}

int MQTTRouterRpcClient::receive_message(RpcMessage& message)
{
    int ret = MQ_SUCCESS;
    uint32_t payload_len_n = 0;
    uint32_t payload_len = 0;

    if (!connected_.load() || NULL == socket_) {
        ret = MQ_ERR_ROUTER_NOT_CONNECTED;
    } else if (MQ_FAIL(recv_all_bytes(socket_, &payload_len_n, sizeof(payload_len_n)))) {
        LOG_ERROR("Failed to receive router message header, ret={}", ret);
    } else {
        payload_len = ntohl(payload_len_n);
        message.payload_size = payload_len;
        message.payload.clear();
        if (payload_len > 0) {
            message.payload.resize(payload_len);
            if (MQ_FAIL(recv_all_bytes(socket_, message.payload.data(), payload_len))) {
                LOG_ERROR("Failed to receive router message payload, ret={}", ret);
            }
        }
    }

    return ret;
}

int MQTTRouterRpcClient::send_request(MessageType type, const MQTTByteVector& payload, MQTTByteVector& response)
{
    int ret = MQ_SUCCESS;
    RpcMessage request_msg(allocator_);
    RpcMessage response_msg(allocator_);
    CoroLockGuard guard(&request_mutex_);

    if (!connected_.load()) {
        ret = MQ_ERR_ROUTER_NOT_CONNECTED;
        failed_requests_.fetch_add(1);
        total_requests_.fetch_add(1);
        last_error_code_.store(ret);
        last_error_time_.store(static_cast<uint64_t>(time(NULL)));
    } else {
        request_msg.type = type;
        request_msg.request_id = generate_request_id();
        request_msg.payload_size = static_cast<uint32_t>(payload.size());
        request_msg.payload = payload;

        if (MQ_FAIL(send_message(request_msg))) {
            failed_requests_.fetch_add(1);
        } else if (MQ_FAIL(receive_message(response_msg))) {
            failed_requests_.fetch_add(1);
        } else {
            response = response_msg.payload;
            successful_requests_.fetch_add(1);
        }
        total_requests_.fetch_add(1);
    }

    return ret;
}

int MQTTRouterRpcClient::send_request_async(MessageType type, const MQTTByteVector& payload)
{
    int ret = MQ_SUCCESS;
    MQTTByteVector response{MQTTSTLAllocator<uint8_t>(allocator_)};
    ret = send_request(type, payload, response);
    return ret;
}

void* MQTTRouterRpcClient::heartbeat_routine(void* arg)
{
    MQTTRouterRpcClient* client = static_cast<MQTTRouterRpcClient*>(arg);

    if (NULL != client) {
        client->heartbeat_coroutine_main();
    }

    return NULL;
}

void MQTTRouterRpcClient::heartbeat_coroutine_main()
{
    while (!should_stop_.load()) {
        (void)heartbeat_cond_.wait(config_.heartbeat_interval_ms);
        if (!should_stop_.load() && connected_.load()) {
            (void)heartbeat();
        }
    }
}

uint32_t MQTTRouterRpcClient::generate_request_id()
{
    return next_request_id_.fetch_add(1);
}

int MQTTRouterRpcClient::serialize_subscribe_request(const SubscribeRequest& request, MQTTByteVector& payload)
{
    int ret = MQ_SUCCESS;
    ::mqtt::router::RouterEnvelope envelope;
    envelope.set_request_id(generate_request_id());
    ::mqtt::router::SubscribeRequest* subscribe_request = envelope.mutable_subscribe_request();
    std::string data;

    subscribe_request->set_server_id(from_mqtt_string(request.server_id));
    subscribe_request->set_client_id(from_mqtt_string(request.client_id));
    subscribe_request->set_topic_filter(from_mqtt_string(request.topic_filter));
    subscribe_request->set_qos(request.qos);

    if (!envelope.SerializeToString(&data)) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        payload.assign(data.begin(), data.end());
    }

    return ret;
}

int MQTTRouterRpcClient::serialize_unsubscribe_request(const UnsubscribeRequest& request, MQTTByteVector& payload)
{
    int ret = MQ_SUCCESS;
    ::mqtt::router::RouterEnvelope envelope;
    ::mqtt::router::UnsubscribeRequest* unsubscribe_request = envelope.mutable_unsubscribe_request();
    std::string data;

    envelope.set_request_id(generate_request_id());
    unsubscribe_request->set_server_id(from_mqtt_string(request.server_id));
    unsubscribe_request->set_client_id(from_mqtt_string(request.client_id));
    unsubscribe_request->set_topic_filter(from_mqtt_string(request.topic_filter));

    if (!envelope.SerializeToString(&data)) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        payload.assign(data.begin(), data.end());
    }

    return ret;
}

int MQTTRouterRpcClient::serialize_client_connect_request(const ClientConnectRequest& request, MQTTByteVector& payload)
{
    int ret = MQ_SUCCESS;
    ::mqtt::router::RouterEnvelope envelope;
    ::mqtt::router::ClientConnectRequest* client_connect_request = envelope.mutable_client_connect_request();
    std::string data;

    envelope.set_request_id(generate_request_id());
    client_connect_request->set_server_id(from_mqtt_string(request.server_id));
    client_connect_request->set_client_id(from_mqtt_string(request.client_id));
    client_connect_request->set_username(from_mqtt_string(request.username));
    client_connect_request->set_protocol_version(request.protocol_version);
    client_connect_request->set_keep_alive(request.keep_alive);
    client_connect_request->set_clean_session(request.clean_session);

    if (!envelope.SerializeToString(&data)) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        payload.assign(data.begin(), data.end());
    }

    return ret;
}

int MQTTRouterRpcClient::serialize_client_disconnect_request(const ClientDisconnectRequest& request,
                                                             MQTTByteVector& payload)
{
    int ret = MQ_SUCCESS;
    ::mqtt::router::RouterEnvelope envelope;
    ::mqtt::router::ClientDisconnectRequest* client_disconnect_request =
        envelope.mutable_client_disconnect_request();
    std::string data;

    envelope.set_request_id(generate_request_id());
    client_disconnect_request->set_server_id(from_mqtt_string(request.server_id));
    client_disconnect_request->set_client_id(from_mqtt_string(request.client_id));
    client_disconnect_request->set_disconnect_reason(from_mqtt_string(request.disconnect_reason));

    if (!envelope.SerializeToString(&data)) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        payload.assign(data.begin(), data.end());
    }

    return ret;
}

int MQTTRouterRpcClient::serialize_route_publish_request(const RoutePublishRequest& request, MQTTByteVector& payload)
{
    int ret = MQ_SUCCESS;
    ::mqtt::router::RouterEnvelope envelope;
    ::mqtt::router::RoutePublishRequest* route_request = envelope.mutable_route_publish_request();
    std::string data;

    envelope.set_request_id(generate_request_id());
    route_request->set_topic(from_mqtt_string(request.topic));
    route_request->set_qos(request.qos);
    route_request->set_retain(request.retain);
    route_request->set_publisher_server_id(from_mqtt_string(request.publisher_server_id));
    route_request->set_publisher_client_id(from_mqtt_string(request.publisher_client_id));
    route_request->set_message_id(request.message_id);
    route_request->set_trace_id(from_mqtt_string(request.trace_id));
    if (!request.payload.empty()) {
        route_request->set_payload(request.payload.data(), request.payload.size());
    }

    if (!envelope.SerializeToString(&data)) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        payload.assign(data.begin(), data.end());
    }

    return ret;
}

int MQTTRouterRpcClient::serialize_authenticate_node_request(const NodeAuthRequest& request, MQTTByteVector& payload)
{
    int ret = MQ_SUCCESS;
    ::mqtt::router::RouterEnvelope envelope;
    ::mqtt::router::AuthenticateNodeRequest* auth_request = envelope.mutable_authenticate_node_request();
    std::string data;

    envelope.set_request_id(generate_request_id());
    auth_request->set_server_id(from_mqtt_string(request.server_id));
    auth_request->set_token(from_mqtt_string(request.token));
    auth_request->set_timestamp_ms(request.timestamp_ms);
    auth_request->set_nonce(from_mqtt_string(request.nonce));
    auth_request->set_host(from_mqtt_string(request.host));
    auth_request->set_port(request.port);
    auth_request->set_forwarding_port(request.forwarding_port);

    if (!envelope.SerializeToString(&data)) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        payload.assign(data.begin(), data.end());
    }

    return ret;
}

int MQTTRouterRpcClient::deserialize_route_targets(const MQTTByteVector& payload, RouteTargetVector& targets)
{
    int ret = MQ_SUCCESS;
    ::mqtt::router::RouterEnvelope envelope;

    targets.clear();
    if (!envelope.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else if (!envelope.has_route_publish_response()) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else if (envelope.route_publish_response().error_code() != MQ_SUCCESS) {
        ret = envelope.route_publish_response().error_code();
    } else {
        for (int64_t i = 0; MQ_SUCCESS == ret && i < envelope.route_publish_response().targets_size(); ++i) {
            const ::mqtt::router::RouteTarget& proto_target = envelope.route_publish_response().targets(i);
            RouteTarget target(allocator_);
            target.server_id = to_mqtt_string(proto_target.server_id(), allocator_);
            target.client_id = to_mqtt_string(proto_target.client_id(), allocator_);
            target.host = to_mqtt_string(proto_target.host(), allocator_);
            target.port = static_cast<int>(proto_target.port());
            target.forwarding_port = static_cast<int>(proto_target.forwarding_port());
            target.qos = static_cast<uint8_t>(proto_target.qos());
            targets.push_back(target);
        }
    }

    return ret;
}

int MQTTRouterRpcClient::deserialize_statistics(const MQTTByteVector& payload, RouterStatistics& stats)
{
    int ret = MQ_SUCCESS;
    ::mqtt::router::RouterEnvelope envelope;

    if (!envelope.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else if (!envelope.has_get_statistics_response()) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else if (envelope.get_statistics_response().error_code() != MQ_SUCCESS) {
        ret = envelope.get_statistics_response().error_code();
    } else {
        stats.total_servers = envelope.get_statistics_response().total_servers();
        stats.total_clients = envelope.get_statistics_response().total_clients();
        stats.total_subscriptions = envelope.get_statistics_response().total_subscriptions();
        stats.total_routes = envelope.get_statistics_response().total_routes();
        stats.memory_usage_bytes = envelope.get_statistics_response().memory_usage_bytes();
        stats.redo_log_entries = envelope.get_statistics_response().redo_log_entries();
        stats.last_snapshot_time = envelope.get_statistics_response().last_snapshot_time();
    }

    return ret;
}

int MQTTRouterRpcClient::deserialize_authenticate_node_response(const MQTTByteVector& payload,
                                                                NodeAuthResponse& response)
{
    int ret = MQ_SUCCESS;
    ::mqtt::router::RouterEnvelope envelope;

    if (!envelope.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else if (!envelope.has_authenticate_node_response()) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        response.error_code = envelope.authenticate_node_response().error_code();
        response.session_id = to_mqtt_string(envelope.authenticate_node_response().session_id(), allocator_);
        response.expires_at_ms = envelope.authenticate_node_response().expires_at_ms();
        ret = response.error_code;
    }

    return ret;
}

int MQTTRouterRpcClient::authenticate_node(const NodeAuthRequest& request, NodeAuthResponse& response)
{
    int ret = MQ_SUCCESS;
    MQTTByteVector payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    MQTTByteVector response_payload{MQTTSTLAllocator<uint8_t>(allocator_)};

    if (MQ_FAIL(serialize_authenticate_node_request(request, payload))) {
    } else if (MQ_FAIL(send_request(MessageType::AUTHENTICATE_NODE, payload, response_payload))) {
    } else if (MQ_FAIL(deserialize_authenticate_node_response(response_payload, response))) {
    } else {
        session_id_ = response.session_id;
    }

    return ret;
}

int MQTTRouterRpcClient::heartbeat()
{
    int ret = MQ_SUCCESS;
    MQTTByteVector payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    MQTTByteVector response_payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    ::mqtt::router::RouterEnvelope envelope;
    ::mqtt::router::HeartbeatRequest* heartbeat_request = envelope.mutable_heartbeat_request();
    std::string data;

    envelope.set_request_id(generate_request_id());
    heartbeat_request->set_server_id(from_mqtt_string(server_id_));
    heartbeat_request->set_session_id(from_mqtt_string(session_id_));
    heartbeat_request->set_host(from_mqtt_string(forwarding_host_));
    heartbeat_request->set_port(static_cast<uint32_t>(config_.router_port));
    heartbeat_request->set_forwarding_port(static_cast<uint32_t>(forwarding_port_));
    heartbeat_request->set_client_count(0);
    heartbeat_request->set_subscription_count(0);

    if (!envelope.SerializeToString(&data)) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        payload.assign(data.begin(), data.end());
        ret = send_request(MessageType::HEARTBEAT, payload, response_payload);
    }

    return ret;
}

int MQTTRouterRpcClient::subscribe(const SubscribeRequest& request)
{
    int ret = MQ_SUCCESS;
    MQTTByteVector payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    MQTTByteVector response_payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    ::mqtt::router::RouterEnvelope envelope;

    if (MQ_FAIL(serialize_subscribe_request(request, payload))) {
    } else if (MQ_FAIL(send_request(MessageType::SUBSCRIBE, payload, response_payload))) {
    } else if (!envelope.ParseFromArray(response_payload.data(), static_cast<int>(response_payload.size()))) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else if (!envelope.has_subscribe_response()) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        ret = envelope.subscribe_response().error_code();
    }

    return ret;
}

int MQTTRouterRpcClient::unsubscribe(const UnsubscribeRequest& request)
{
    int ret = MQ_SUCCESS;
    MQTTByteVector payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    MQTTByteVector response_payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    ::mqtt::router::RouterEnvelope envelope;

    if (MQ_FAIL(serialize_unsubscribe_request(request, payload))) {
    } else if (MQ_FAIL(send_request(MessageType::UNSUBSCRIBE, payload, response_payload))) {
    } else if (!envelope.ParseFromArray(response_payload.data(), static_cast<int>(response_payload.size()))) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else if (!envelope.has_unsubscribe_response()) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        ret = envelope.unsubscribe_response().error_code();
    }

    return ret;
}

int MQTTRouterRpcClient::client_connect(const ClientConnectRequest& request)
{
    int ret = MQ_SUCCESS;
    MQTTByteVector payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    MQTTByteVector response_payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    ::mqtt::router::RouterEnvelope envelope;

    if (MQ_FAIL(serialize_client_connect_request(request, payload))) {
    } else if (MQ_FAIL(send_request(MessageType::CLIENT_CONNECT, payload, response_payload))) {
    } else if (!envelope.ParseFromArray(response_payload.data(), static_cast<int>(response_payload.size()))) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else if (!envelope.has_client_connect_response()) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        ret = envelope.client_connect_response().error_code();
    }

    return ret;
}

int MQTTRouterRpcClient::client_disconnect(const ClientDisconnectRequest& request)
{
    int ret = MQ_SUCCESS;
    MQTTByteVector payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    MQTTByteVector response_payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    ::mqtt::router::RouterEnvelope envelope;

    if (MQ_FAIL(serialize_client_disconnect_request(request, payload))) {
    } else if (MQ_FAIL(send_request(MessageType::CLIENT_DISCONNECT, payload, response_payload))) {
    } else if (!envelope.ParseFromArray(response_payload.data(), static_cast<int>(response_payload.size()))) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else if (!envelope.has_client_disconnect_response()) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        ret = envelope.client_disconnect_response().error_code();
    }

    return ret;
}

int MQTTRouterRpcClient::route_publish(const RoutePublishRequest& request, RouteTargetVector& targets)
{
    int ret = MQ_SUCCESS;
    MQTTByteVector payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    MQTTByteVector response_payload{MQTTSTLAllocator<uint8_t>(allocator_)};

    if (MQ_FAIL(serialize_route_publish_request(request, payload))) {
    } else if (MQ_FAIL(send_request(MessageType::ROUTE_PUBLISH, payload, response_payload))) {
    } else if (MQ_FAIL(deserialize_route_targets(response_payload, targets))) {
    }

    return ret;
}

int MQTTRouterRpcClient::get_statistics(RouterStatistics& stats)
{
    int ret = MQ_SUCCESS;
    MQTTByteVector payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    MQTTByteVector response_payload{MQTTSTLAllocator<uint8_t>(allocator_)};
    ::mqtt::router::RouterEnvelope envelope;
    std::string data;

    envelope.set_request_id(generate_request_id());
    envelope.mutable_get_statistics_request();

    if (!envelope.SerializeToString(&data)) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        payload.assign(data.begin(), data.end());
        if (MQ_FAIL(send_request(MessageType::GET_STATISTICS, payload, response_payload))) {
        } else if (MQ_FAIL(deserialize_statistics(response_payload, stats))) {
        }
    }

    return ret;
}

int MQTTRouterRpcClient::subscribe_async(const SubscribeRequest& request)
{
    int ret = MQ_SUCCESS;
    ret = subscribe(request);
    return ret;
}

int MQTTRouterRpcClient::unsubscribe_async(const UnsubscribeRequest& request)
{
    int ret = MQ_SUCCESS;
    ret = unsubscribe(request);
    return ret;
}

int MQTTRouterRpcClient::client_connect_async(const ClientConnectRequest& request)
{
    int ret = MQ_SUCCESS;
    ret = client_connect(request);
    return ret;
}

int MQTTRouterRpcClient::client_disconnect_async(const ClientDisconnectRequest& request)
{
    int ret = MQ_SUCCESS;
    ret = client_disconnect(request);
    return ret;
}

int MQTTRouterRpcClient::batch_subscribe(const MQTTVector<SubscribeRequest>& requests)
{
    int ret = MQ_SUCCESS;

    for (size_t i = 0; MQ_SUCCESS == ret && i < requests.size(); ++i) {
        ret = subscribe(requests[i]);
    }

    return ret;
}

int MQTTRouterRpcClient::batch_unsubscribe(const MQTTVector<UnsubscribeRequest>& requests)
{
    int ret = MQ_SUCCESS;

    for (size_t i = 0; MQ_SUCCESS == ret && i < requests.size(); ++i) {
        ret = unsubscribe(requests[i]);
    }

    return ret;
}

bool MQTTRouterRpcClient::is_connected() const
{
    return connected_.load();
}

int MQTTRouterRpcClient::get_connection_state(bool& is_connected) const
{
    int ret = MQ_SUCCESS;
    is_connected = connected_.load();
    return ret;
}

uint64_t MQTTRouterRpcClient::get_last_error_time() const
{
    return last_error_time_.load();
}

int MQTTRouterRpcClient::get_last_error_code() const
{
    return last_error_code_.load();
}

const MQTTString& MQTTRouterRpcClient::get_last_error_message() const
{
    return last_error_message_;
}

uint64_t MQTTRouterRpcClient::get_total_requests() const
{
    return total_requests_.load();
}

uint64_t MQTTRouterRpcClient::get_successful_requests() const
{
    return successful_requests_.load();
}

uint64_t MQTTRouterRpcClient::get_failed_requests() const
{
    return failed_requests_.load();
}
