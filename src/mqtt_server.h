#pragma once
#include "mqtt_define.h"
#include "co_routine.h"
#include "mqtt_socket.h"
#include "mqtt_allocator.h"
#include "logger.h"
#include <string>

class MQTTServer;

struct ClientContext {
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
  MQTTServer();
  virtual ~MQTTServer();

  int start(const char* ip, int port);
  void run();
  void stop();
  bool is_running() const { return running_; }

private:
  static void* accept_routine(void* arg);
  static void* client_routine(void* arg);
  static void handle_client(ClientContext* ctx);

private:
  stCoRoutine_t *accept_co_;
  MQTTSocket* server_socket_;
  bool running_;
  std::string server_ip_;
  int server_port_;
};