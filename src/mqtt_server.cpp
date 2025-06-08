#include "mqtt_server.h"
#include <arpa/inet.h>
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_memory_tags.h"
#include "mqtt_protocol_handler.h"
using namespace mqtt;

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
}

MQTTServer::MQTTServer(const mqtt::ServerConfig& config, const mqtt::MemoryConfig& memory_config)
    : accept_co_(NULL),
      server_socket_(NULL),
      running_(false),
      host_(config.bind_address),
      port_(config.port),
      server_config_(config),
      memory_config_(memory_config),
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

void MQTTServer::add_connection()
{
  current_connections_.fetch_add(1);
  LOG_DEBUG("新连接建立，当前连接数: {}/{}", current_connections_.load(),
            server_config_.max_connections);
}

void MQTTServer::remove_connection()
{
  current_connections_.fetch_sub(1);
  LOG_DEBUG("连接断开，当前连接数: {}/{}", current_connections_.load(),
            server_config_.max_connections);
}

int MQTTServer::start()
{
  int ret = MQ_SUCCESS;
  if (running_) {
    LOG_WARN("Server is already running");
    return MQ_SUCCESS;
  }

  // Create server socket
  if (MQ_FAIL(MQTTSocket::create_tcp_socket(server_socket_))) {
    LOG_ERROR("Failed to create server socket for {}:{}", host_, port_);
    return MQ_ERR_SOCKET;
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
    return MQ_ERR_SOCKET;
  }

  running_ = true;
  LOG_INFO("MQTT Server initialized on {}:{} (max_connections: {}, backlog: {})", host_, port_,
           server_config_.max_connections, server_config_.backlog);
  return MQ_SUCCESS;
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

  co_eventloop(co_get_epoll_ct(), 0, 0);
}

void MQTTServer::stop()
{
  if (!running_) {
    return;
  }

  LOG_INFO("Stopping MQTT Server on {}:{}", host_, port_);
  running_ = false;

  if (MQ_NOT_NULL(accept_co_)) {
    co_release(accept_co_);
    accept_co_ = NULL;
  }

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
  // Create protocol handler
  MQTTProtocolHandler* handler = new (ctx->allocator->allocate(sizeof(MQTTProtocolHandler)))
      MQTTProtocolHandler(ctx->allocator);

  // Initialize handler
  handler->init(ctx->client, ctx->client_ip, ctx->client_port);

  // Process packets until client disconnects
  int ret = handler->process();
  if (ret != 0) {
    LOG_WARN("Client {}:{} disconnected with error: {}", ctx->client_ip, ctx->client_port, ret);
  }

  // Cleanup
  handler->~MQTTProtocolHandler();
  ctx->allocator->deallocate(handler, sizeof(MQTTProtocolHandler));
}
