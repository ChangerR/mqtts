#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_auth_interface.h"
#include "mqtt_config.h"
#include "mqtt_define.h"
#include "mqtt_parser.h"
#include "mqtt_runtime.h"
#include "mqtt_socket.h"

class MQTTServer;

struct ClientContext
{
  MQTTServer* server;  // Server instance pointer
  MQTTSocket* client;
  std::string client_id;
  std::string trace_id;
  std::string client_ip;
  int client_port;
  MQTTAllocator* allocator;  // Client-specific allocator

  // Run on the owning event thread, after the handler and socket are released.
  // The child is unregistered by the allocator id, not by ctx->client_id, so a
  // destroyed or moved-from context string cannot leave a registration behind.
  static void destroy(ClientContext* ctx)
  {
    if (MQ_ISNULL(ctx) || MQ_ISNULL(ctx->allocator)) {
      return;
    }
    MQTTAllocator* allocator = ctx->allocator;
    MQTTAllocator* parent = allocator->get_parent();
    const std::string id = allocator->get_id();
    ctx->~ClientContext();
    allocator->deallocate(ctx, sizeof(ClientContext));
    if (parent) {
      parent->remove_child(id);
    }
  }
};

class MQTTServer
{
 public:
  MQTTServer(const std::string& host, int port);
  MQTTServer(const mqtt::ServerConfig& config, const mqtt::MemoryConfig& memory_config,
             const mqtt::ProtocolLimits& protocol_limits);
  virtual ~MQTTServer();

  int start();
  void run();
  void stop();
  bool is_running() const { return running_; }
  void set_auth_manager(mqtt::auth::AuthManager* manager) { auth_manager_ = manager; }

  // 连接管理
  bool can_accept_connection() const;
  int add_connection();
  int remove_connection();
  int get_connection_count() const { return current_connections_.load(); }

 private:
  static void* accept_routine(void* arg);
  static void* client_routine(void* arg);
  static void handle_client(ClientContext* ctx);

  // Event loop callback for pending message processing
  static int eventloop_callback(void* arg);

 private:
  MQTTSocket* server_socket_;
  mutable bool running_;
  std::string host_;
  int port_;

  // 服务器配置
  mqtt::ServerConfig server_config_;
  mqtt::MemoryConfig memory_config_;
  mqtt::ProtocolLimits protocol_limits_;

  // 连接管理
  std::atomic<int> current_connections_;
  mqtt::auth::AuthManager* auth_manager_ = nullptr;

  // 多个事件线程抢同一个监听 fd 时，后醒来的线程会一直赢。按成功 accept 轮转，
  // 下一条连接交给另一个线程，跨线程接管才不会靠运气。
  std::atomic<uint32_t> accept_slot_next_{0};
  std::atomic<uint64_t> accept_turn_{0};
};
