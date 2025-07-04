#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "co_routine.h"
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_config.h"
#include "mqtt_define.h"
#include "mqtt_parser.h"
#include "mqtt_socket.h"

class MQTTServer;

struct ClientContext
{
  MQTTServer* server;  // Server instance pointer
  MQTTSocket* client;
  std::string client_id;
  std::string client_ip;
  int client_port;
  MQTTAllocator* allocator;  // Client-specific allocator
};

class MQTTServer
{
 public:
  MQTTServer(const std::string& host, int port);
  MQTTServer(const mqtt::ServerConfig& config, const mqtt::MemoryConfig& memory_config);
  virtual ~MQTTServer();

  int start();
  void run();
  void stop();
  bool is_running() const { return running_; }

  // 连接管理
  bool can_accept_connection() const;
  void add_connection();
  void remove_connection();
  int get_connection_count() const { return current_connections_.load(); }

 private:
  static void* accept_routine(void* arg);
  static void* client_routine(void* arg);
  static void handle_client(ClientContext* ctx);

  // Event loop callback for pending message processing
  static int eventloop_callback(void* arg);

 private:
  stCoRoutine_t* accept_co_;
  MQTTSocket* server_socket_;
  mutable bool running_;
  std::string host_;
  int port_;

  // 服务器配置
  mqtt::ServerConfig server_config_;
  mqtt::MemoryConfig memory_config_;

  // 连接管理
  std::atomic<int> current_connections_;
};