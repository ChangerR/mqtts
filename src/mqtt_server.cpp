#include "mqtt_server.h"
#include "logger.h"
#include <arpa/inet.h>
#include "mqtt_allocator.h"

MQTTServer::MQTTServer() 
  : accept_co_(NULL), server_socket_(NULL), running_(false), server_port_(0) {}

MQTTServer::~MQTTServer() 
{
  stop();
}

int MQTTServer::start(const char* ip, int port) 
{
  int ret = MQ_SUCCESS;
  if (running_) {
    LOG->warn("Server is already running");
    return MQ_SUCCESS;
  }

  server_ip_ = ip;
  server_port_ = port;
  
  // Create server socket
  if (MQ_FAIL(MQTTSocket::create_tcp_socket(server_socket_))) {
    LOG->error("Failed to create server socket for {}:{}", ip, port);
    return MQ_ERR_SOCKET;
  }

  // Start listening
  if (MQ_FAIL(server_socket_->listen(ip, port, true))) {
    LOG->error("Failed to start listening on {}:{}", ip, port);
    MQTTAllocator* root = MQ_MEM_MANAGER.get_root_allocator();
    if (MQ_NOT_NULL(server_socket_)) {
      server_socket_->~MQTTSocket();  // Call destructor explicitly
      root->deallocate(server_socket_, sizeof(MQTTSocket));
      server_socket_ = NULL;
    }
    return MQ_ERR_SOCKET;
  }

  running_ = true;
  LOG->info("MQTT Server initialized on {}:{}", ip, port);
  return MQ_SUCCESS;
}

void MQTTServer::run()
{
  if (!running_) {
    LOG->warn("Server is not running");
    return;
  }

  // Create accept coroutine
  co_create(&accept_co_, NULL, accept_routine, this);
  co_resume(accept_co_);
  
  LOG->info("MQTT Server running on {}:{}", server_ip_, server_port_);

  co_eventloop(co_get_epoll_ct(), 0, 0);
}

void MQTTServer::stop()
{
  if (!running_) {
    return;
  }
  
  LOG->info("Stopping MQTT Server on {}:{}", server_ip_, server_port_);
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
  
  LOG->info("MQTT Server stopped");
}

void* MQTTServer::accept_routine(void* arg)
{
  MQTTServer* server = (MQTTServer*)arg;
  MQTTSocket* client = NULL;
  int ret = MQ_SUCCESS;
  
  if (MQ_ISNULL(server->server_socket_)) {
    LOG->error("Server socket is NULL");
    return NULL;
  }
  
  LOG->info("Accept routine started, running: {}", server->running_);
  while (server->running_ && MQ_SUCC(ret)) {
    // Use co_poll to wait for new connection
    struct pollfd pf = { 0 };
    pf.fd = server->server_socket_->get_fd();
    pf.events = (POLLIN | POLLERR | POLLHUP);
    co_poll(co_get_epoll_ct(), &pf, 1, 1000);  // 100ms timeout

    // Check if socket is still valid
    if (!server->server_socket_->is_connected()) {
      LOG->warn("Server socket disconnected");
      ret = MQ_ERR_SOCKET;
      break;
    }
    ret = server->server_socket_->accept(client);
    if (MQ_FAIL(ret)) {
      if (ret == MQ_ERR_SOCKET_ACCEPT_WOULDBLOCK) {
        ret = MQ_SUCCESS;
        continue;
      }
      LOG->error("Failed to accept connection");
      break;
    }
    
    // Create client-specific allocator with a memory limit
    std::string client_id = "client_" + std::to_string(time(NULL));  // Temporary client ID
    MQTTAllocator* root = MQ_MEM_MANAGER.get_root_allocator();
    MQTTAllocator* client_allocator = root->create_child(client_id, "client", 1024*1024);  // 1MB limit
    if (MQ_ISNULL(client_allocator)) {
      LOG->error("Failed to create client allocator");
      if (MQ_NOT_NULL(client)) {
        client->~MQTTSocket();  // Call destructor explicitly
        root->deallocate(client, sizeof(MQTTSocket));
      }
      continue;
    }
    LOG->info("Accepted new connection from {}:{}", client->get_peer_addr(), client->get_peer_port());
    
    // Create client context using client's allocator
    void* ctx_mem = client_allocator->allocate(sizeof(ClientContext));
    if (MQ_ISNULL(ctx_mem)) {
      LOG->error("Failed to allocate client context");
      root->remove_child(client_id);
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
  MQTTSocket* client = ctx->client;
  MQTTAllocator* allocator = ctx->allocator;
  const std::string& client_id = ctx->client_id;
  const std::string& client_ip = ctx->client_ip;
  int client_port = ctx->client_port;
  
  // Allocate buffer using client allocator
  char* buf = (char*)allocator->allocate(1024);
  if (!buf) {
    LOG->error("Failed to allocate buffer for client {}:{} (memory limit: {} bytes, used: {} bytes)", 
              client_ip, client_port, allocator->get_memory_limit(), 
              allocator->get_memory_usage());
    return;
  }
  
  int len = 1024;
  
  // Set client socket to non-blocking mode
  if (MQ_FAIL(client->set_nonblocking(true))) {
    LOG->error("Failed to set client socket to non-blocking mode");
    ret = MQ_ERR_SOCKET;
  }

  while (MQ_SUCC(ret)) {
    // Reset buffer length for each read
    len = 1024;

    // Use co_poll to wait for read event
    struct pollfd pf = { 0 };
    pf.fd = client->get_fd();
    pf.events = (POLLIN | POLLERR | POLLHUP);
    co_poll(co_get_epoll_ct(), &pf, 1, 100);  // 100ms timeout

    // Check if socket is still valid
    if (!client->is_connected()) {
      LOG->warn("Client {}:{} disconnected", client_ip, client_port);
      ret = MQ_ERR_SOCKET;
      break;
    }

    // Try to receive data
    ret = client->recv(buf, len);
    if (MQ_FAIL(ret)) {
      LOG->warn("Client {}:{} disconnected unexpectedly", client_ip, client_port);
      break;
    }
    
    LOG->debug("Received {} bytes from client {}:{}", len, client_ip, client_port);
    
    // TODO: Handle MQTT protocol messages
    // For now just echo back
    ret = client->send(buf, len);
    if (MQ_FAIL(ret)) {
      LOG->warn("Failed to send response to client {}:{}", client_ip, client_port);
      break;
    }
    LOG->debug("Sent {} bytes to client {}:{}", len, client_ip, client_port);
  }

  // Cleanup
  if (client) {
    client->close();
  }
  
  // Free the buffer using client allocator
  allocator->deallocate(buf, 1024);
}