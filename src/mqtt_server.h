#pragma once
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "co_routine.h"
#include "logger.h"
#include "mqtt_allocator.h"
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
  virtual ~MQTTServer();

  int start();
  void run();
  void stop();
  bool is_running() const { return running_; }

 private:
  static void* accept_routine(void* arg);
  static void* client_routine(void* arg);
  static void handle_client(ClientContext* ctx);

 private:
  stCoRoutine_t* accept_co_;
  MQTTSocket* server_socket_;
  bool running_;
  std::string host_;
  int port_;
};