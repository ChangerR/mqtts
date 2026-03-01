#include "mqtt_server.h"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_memory_tags.h"
#include "mqtt_protocol_handler.h"
#include "mqtt_session_manager_v2.h"
#include "mqtt_string_utils.h"
#include "websocket_protocol_handler.h"
#include "websocket_mqtt_bridge.h"
#include "websocket_handler_adapter.h"
using namespace mqtt;

namespace {
static std::string to_fixed_base62(uint64_t value, size_t width)
{
  static const char kAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  std::string encoded;
  do {
    encoded.push_back(kAlphabet[value % 62]);
    value /= 62;
  } while (value > 0);

  while (encoded.size() < width) {
    encoded.push_back('0');
  }

  if (encoded.size() > width) {
    encoded.resize(width);
  }
  std::reverse(encoded.begin(), encoded.end());
  return encoded;
}

static uint32_t encode_client_ip(const std::string& ip)
{
  struct in_addr addr;
  if (inet_pton(AF_INET, ip.c_str(), &addr) == 1) {
    return ntohl(addr.s_addr);
  }
  return static_cast<uint32_t>(std::hash<std::string>{}(ip));
}

static std::string generate_session_trace_id(const std::string& client_ip, int client_port)
{
  const uint64_t ts_us =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
  const uint32_t ip_code = encode_client_ip(client_ip);
  const uint16_t port_code = static_cast<uint16_t>(client_port & 0xFFFF);
  const uint64_t endpoint_code = (static_cast<uint64_t>(ip_code) << 16) | port_code;  // 48 bits

  // Session trace id based on time + client endpoint.
  // Y + ts_us(10) + ip_port(9) => 20 chars
  return "Y-" + to_fixed_base62(ts_us, 10) + to_fixed_base62(endpoint_code, 9) + "-0";
}
}  // namespace

MQTTServer::MQTTServer(const std::string& host, int port)
    : accept_co_(NULL),
      server_socket_(NULL),
      running_(false),
      host_(host),
      port_(port),
      current_connections_(0)
{
  // 使用默认配置
  server_config_.bind_address = host;
  server_config_.port = static_cast<uint16_t>(port);
  server_config_.max_connections = 1000;
  server_config_.backlog = 128;
  memory_config_.client_max_size = 1048576;
  mqtt_protocol_config_ = mqtt::MQTTProtocolConfig();
}

MQTTServer::MQTTServer(const mqtt::ServerConfig& config, const mqtt::MemoryConfig& memory_config,
                       const mqtt::MQTTProtocolConfig& mqtt_protocol_config)
    : accept_co_(NULL),
      server_socket_(NULL),
      running_(false),
      host_(config.bind_address),
      port_(config.port),
      server_config_(config),
      memory_config_(memory_config),
      mqtt_protocol_config_(mqtt_protocol_config),
      current_connections_(0)
{
}

MQTTServer::~MQTTServer()
{
  stop();
}

bool MQTTServer::can_accept_connection() const
{
  return current_connections_.load() < server_config_.max_connections;
}

int MQTTServer::add_connection()
{
  int ret = MQ_SUCCESS;

  if (current_connections_.load() >= server_config_.max_connections) {
    LOG_WARN("Cannot add connection: maximum connections ({}) reached",
             server_config_.max_connections);
    ret = MQ_ERR_CONNECT_SERVER_UNAVAILABLE;
  } else {
    current_connections_.fetch_add(1);
    LOG_DEBUG("新连接建立，当前连接数: {}/{}", current_connections_.load(),
              server_config_.max_connections);
  }

  return ret;
}

int MQTTServer::remove_connection()
{
  int ret = MQ_SUCCESS;

  if (current_connections_.load() <= 0) {
    LOG_WARN("Cannot remove connection: connection count is already 0");
    ret = MQ_ERR_INTERNAL;
  } else {
    current_connections_.fetch_sub(1);
    LOG_DEBUG("连接断开，当前连接数: {}/{}", current_connections_.load(),
              server_config_.max_connections);
  }

  return ret;
}

int MQTTServer::start()
{
  int __mq_ret = 0;
  do {
    int ret = MQ_SUCCESS;
    if (running_) {
      LOG_WARN("Server is already running");
      __mq_ret = MQ_SUCCESS;
      break;
    }
  
    // Create server socket
    if (MQ_FAIL(MQTTSocket::create_tcp_socket(server_socket_))) {
      LOG_ERROR("Failed to create server socket for {}:{}", host_, port_);
      __mq_ret = MQ_ERR_SOCKET;
      break;
    }
  
    // Start listening - 使用配置中的backlog参数
    if (MQ_FAIL(server_socket_->listen(host_.c_str(), port_, true, server_config_.backlog))) {
      LOG_ERROR("Failed to start listening on {}:{}", host_, port_);
      MQTTAllocator* root = MQ_MEM_MANAGER.get_root_allocator();
      if (MQ_NOT_NULL(server_socket_)) {
        server_socket_->~MQTTSocket();  // Call destructor explicitly
        root->deallocate(server_socket_, sizeof(MQTTSocket));
        server_socket_ = NULL;
      }
      __mq_ret = MQ_ERR_SOCKET;
      break;
    }
  
    running_ = true;
    LOG_INFO("MQTT Server initialized on {}:{} (max_connections: {}, backlog: {})", host_, port_,
             server_config_.max_connections, server_config_.backlog);
    __mq_ret = MQ_SUCCESS;
    break;
  } while (false);

  return __mq_ret;
}

void MQTTServer::run()
{
  if (!running_) {
    LOG_WARN("Server is not running");
    return;
  }

  // Create accept coroutine
  co_create(&accept_co_, NULL, accept_routine, this);
  co_resume(accept_co_);

  LOG_INFO("MQTT Server running on {}:{}", host_, port_);

  // Use eventloop with callback for pending message processing
  co_eventloop(co_get_epoll_ct(), eventloop_callback, this);

  if (MQ_NOT_NULL(accept_co_)) {
    co_release(accept_co_);
    accept_co_ = NULL;
  }
}

void MQTTServer::stop()
{
  if (!running_) {
    return;
  }

  LOG_INFO("Stopping MQTT Server on {}:{}", host_, port_);
  running_ = false;

  if (MQ_NOT_NULL(server_socket_)) {
    server_socket_->close();
    MQTTAllocator* root = MQ_MEM_MANAGER.get_root_allocator();
    server_socket_->~MQTTSocket();  // Call destructor explicitly
    root->deallocate(server_socket_, sizeof(MQTTSocket));
    server_socket_ = NULL;
  }

  LOG_INFO("MQTT Server stopped");
}

void* MQTTServer::accept_routine(void* arg)
{
  MQTTServer* server = (MQTTServer*)arg;
  MQTTSocket* client = NULL;
  int ret = MQ_SUCCESS;

  if (MQ_ISNULL(server->server_socket_)) {
    LOG_ERROR("Server socket is NULL");
    return NULL;
  }

  LOG_INFO("Accept routine started, running: {}", server->running_);
  while (server->running_ && MQ_SUCC(ret)) {
    // 检查连接数限制
    if (!server->can_accept_connection()) {
      LOG_WARN("达到最大连接数限制 ({}), 等待连接释放", server->server_config_.max_connections);
      // 等待一段时间后继续检查
      struct pollfd pf = {0};
      pf.fd = server->server_socket_->get_fd();
      pf.events = (POLLIN | POLLERR | POLLHUP);
      co_poll(co_get_epoll_ct(), &pf, 1, 1000);  // 1秒超时
      continue;
    }

    // Use co_poll to wait for new connection
    struct pollfd pf = {0};
    pf.fd = server->server_socket_->get_fd();
    pf.events = (POLLIN | POLLERR | POLLHUP);
    co_poll(co_get_epoll_ct(), &pf, 1, 1000);  // 100ms timeout
    // Check if socket is still valid
    if (!server->server_socket_->is_connected()) {
      LOG_WARN("Server socket disconnected");
      ret = MQ_ERR_SOCKET;
      break;
    }
    ret = server->server_socket_->accept(client);
    if (MQ_FAIL(ret)) {
      if (ret == MQ_ERR_SOCKET_ACCEPT_WOULDBLOCK) {
        ret = MQ_SUCCESS;
        continue;
      }
      LOG_ERROR("Failed to accept connection");
      break;
    }

    // Create client-specific allocator with a memory limit
    std::string client_id =
        "client_" + client->get_peer_addr() + "_" +
        std::to_string(client->get_peer_port());  // Use IP and port for client ID
    MQTTAllocator* root = MQ_MEM_MANAGER.get_root_allocator();
    MQTTAllocator* client_allocator = root->create_child(
        client_id.c_str(), MQTTMemoryTag::MEM_TAG_CLIENT, server->memory_config_.client_max_size);
    if (MQ_ISNULL(client_allocator)) {
      LOG_ERROR("Failed to create client allocator");
      if (MQ_NOT_NULL(client)) {
        client->~MQTTSocket();  // Call destructor explicitly
        root->deallocate(client, sizeof(MQTTSocket));
      }
      continue;
    }

    // 增加连接计数
    server->add_connection();

    LOG_INFO("Accepted new connection from {}:{}", client->get_peer_addr(),
             client->get_peer_port());

    // Create client context using client's allocator
    void* ctx_mem = client_allocator->allocate(sizeof(ClientContext));
    if (MQ_ISNULL(ctx_mem)) {
      LOG_ERROR("Failed to allocate client context");
      root->remove_child(client_id);
      server->remove_connection();  // 减少连接计数
      if (MQ_NOT_NULL(client)) {
        client->~MQTTSocket();  // Call destructor explicitly
        root->deallocate(client, sizeof(MQTTSocket));
      }
      continue;
    }

    // Initialize client context using placement new
    ClientContext* ctx = new (ctx_mem) ClientContext();
    ctx->server = server;
    ctx->client = client;
    ctx->client_id = client_id;
    ctx->client_ip = client->get_peer_addr();
    ctx->client_port = client->get_peer_port();
    ctx->trace_id = generate_session_trace_id(ctx->client_ip, ctx->client_port);
    ctx->allocator = client_allocator;

    // Create client coroutine
    stCoRoutine_t* co = NULL;
    co_create(&co, NULL, client_routine, ctx);
    co_resume(co);
  }

  return NULL;
}

void* MQTTServer::client_routine(void* arg)
{
  ClientContext* ctx = (ClientContext*)arg;
  MQTTSocket* client = ctx->client;
  const std::string session_trace_id = ctx->trace_id;
  mqtt_log_ctx::bind_trace_id(session_trace_id);
  struct TraceCleanupGuard
  {
    ~TraceCleanupGuard() { mqtt_log_ctx::clear_trace_id(); }
  } trace_cleanup_guard;

  // Handle client connection
  handle_client(ctx);

  // Cleanup
  if (MQ_NOT_NULL(client)) {
    client->close();
    client->~MQTTSocket();  // Call destructor explicitly
    MQTTAllocator* root = MQ_MEM_MANAGER.get_root_allocator();
    root->deallocate(client, sizeof(MQTTSocket));
  }

  // 减少连接计数
  ctx->server->remove_connection();

  // Cleanup client allocator and context
  if (MQ_NOT_NULL(ctx->allocator)) {
    MQTTAllocator* root = MQ_MEM_MANAGER.get_root_allocator();
    // Free the context using the same allocator
    ctx->~ClientContext();  // Call destructor explicitly
    ctx->allocator->deallocate(ctx, sizeof(ClientContext));
    root->remove_child(ctx->client_id);
  }

  return NULL;
}

void MQTTServer::handle_client(ClientContext* ctx)
{
  int ret = MQ_SUCCESS;

  // Detect if this is a WebSocket connection
  bool is_websocket = ctx->client->is_websocket_upgrade_request();

  if (is_websocket) {
    LOG_INFO("Detected WebSocket upgrade request from {}:{}", ctx->client_ip, ctx->client_port);

    // Create WebSocket protocol handler
    websocket::WebSocketProtocolHandler* ws_handler =
        new (ctx->allocator->allocate(sizeof(websocket::WebSocketProtocolHandler)))
        websocket::WebSocketProtocolHandler(ctx->allocator);

    // Create WebSocket-MQTT bridge
    websocket::WebSocketMQTTBridge* bridge =
        new (ctx->allocator->allocate(sizeof(websocket::WebSocketMQTTBridge)))
        websocket::WebSocketMQTTBridge(ctx->allocator, websocket::MessageFormat::JSON);
    bridge->set_allow_mqtt3x(ctx->server->mqtt_protocol_config_.allow_mqtt3x);

    // Create adapter to bridge WebSocket and MQTT protocol handlers
    websocket::WebSocketHandlerAdapter* adapter =
        new (ctx->allocator->allocate(sizeof(websocket::WebSocketHandlerAdapter)))
        websocket::WebSocketHandlerAdapter(ctx->allocator, bridge, ws_handler, ctx->client_id);

    // Initialize bridge with session manager
    mqtt::GlobalSessionManager& session_manager = mqtt::GlobalSessionManagerInstance::instance();
    if (MQ_SUCC(bridge->init(&session_manager))) {
      // Initialize WebSocket handler
      if (MQ_SUCC(ws_handler->init(ctx->client, ctx->client_ip, ctx->client_port))) {
        // Set the client ID for WebSocket handler
        ws_handler->set_client_id(ctx->client_id);

        // Register handler with bridge
        bridge->register_handler(ctx->client_id, ws_handler);

        // Set bridge reference in handler
        ws_handler->set_bridge(bridge);

        // Register the adapter with the session manager for message forwarding
        mqtt::MQTTString client_id_mqtt = mqtt::to_mqtt_string(ctx->client_id, ctx->allocator);
        if (MQ_SUCC(session_manager.register_session(client_id_mqtt, adapter))) {
          LOG_INFO("WebSocket handler adapter registered for client {}", ctx->client_id);

          // Process WebSocket connection
          ret = ws_handler->process();
          if (ret != 0) {
            LOG_WARN("WebSocket client {}:{} disconnected with error: {}",
                     ctx->client_ip, ctx->client_port, ret);
          }

        } else {
          LOG_ERROR("Failed to register WebSocket adapter with session manager");
        }

        // Unregister handler from bridge
        bridge->unregister_handler(ctx->client_id);
      } else {
        LOG_ERROR("Failed to initialize WebSocket handler");
      }
    } else {
      LOG_ERROR("Failed to initialize WebSocket-MQTT bridge");
    }

    // Cleanup
    adapter->~WebSocketHandlerAdapter();
    ctx->allocator->deallocate(adapter, sizeof(websocket::WebSocketHandlerAdapter));

    ws_handler->~WebSocketProtocolHandler();
    ctx->allocator->deallocate(ws_handler, sizeof(websocket::WebSocketProtocolHandler));

    bridge->~WebSocketMQTTBridge();
    ctx->allocator->deallocate(bridge, sizeof(websocket::WebSocketMQTTBridge));

  } else {
    // Regular MQTT connection
    LOG_INFO("Regular MQTT connection from {}:{}", ctx->client_ip, ctx->client_port);

    // Create protocol handler
    MQTTProtocolHandler* handler = new (ctx->allocator->allocate(sizeof(MQTTProtocolHandler)))
        MQTTProtocolHandler(ctx->allocator);
    handler->set_allow_mqtt3x(ctx->server->mqtt_protocol_config_.allow_mqtt3x);

    // Initialize handler
    if (MQ_FAIL(handler->init(ctx->client, ctx->client_ip, ctx->client_port))) {
      LOG_WARN("failed to init handler, error: {}", ret);
    } else {
      // Set session manager reference
      mqtt::GlobalSessionManager& session_manager = mqtt::GlobalSessionManagerInstance::instance();
      handler->set_session_manager(&session_manager);

      // Process packets until client disconnects
      ret = handler->process();
      if (ret != 0) {
        LOG_WARN("Client {}:{} disconnected with error: {}", ctx->client_ip, ctx->client_port, ret);
      }
    }

    // Cleanup
    handler->~MQTTProtocolHandler();
    ctx->allocator->deallocate(handler, sizeof(MQTTProtocolHandler));
  }
}

int MQTTServer::eventloop_callback(void* arg)
{
  int __mq_ret = 0;
  do {
    MQTTServer* server = (MQTTServer*)arg;
  
    // Only process if server is still running
    if (!server->running_) {
      __mq_ret = 0;
      break;
    }
  
    mqtt::GlobalSessionManager& session_manager = mqtt::GlobalSessionManagerInstance::instance();
  
    // Get current thread's session manager
    mqtt::ThreadLocalSessionManager* local_manager = session_manager.get_thread_manager();
    if (local_manager) {
      // Process pending messages without blocking (process up to 50 messages per callback)
      int processed = local_manager->process_pending_messages_nowait(50);
      if (processed > 0) {
        LOG_DEBUG("Processed {} pending messages in eventloop callback", processed);
      }
    }
  
    __mq_ret = 0; // Continue eventloop
    break;
  } while (false);

  return __mq_ret;
}
