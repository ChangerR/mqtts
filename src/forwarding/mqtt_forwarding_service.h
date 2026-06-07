#pragma once

#include <atomic>
#include <string>
#include <thread>
#include "co_routine.h"
#include "mqtt_allocator.h"
#include "mqtt_define.h"
#include "mqtt_socket.h"

namespace mqtt
{

class GlobalSessionManager;

class MQTTForwardingService
{
 public:
  MQTTForwardingService(MQTTAllocator* allocator,
                        GlobalSessionManager* session_manager,
                        const std::string& host,
                        int port);
  ~MQTTForwardingService();

 int start();
  int stop();
  int is_running(bool& running) const;

 private:
  static void* accept_routine(void* arg);
  static void* client_routine(void* arg);
  static int eventloop_callback(void* arg);

  void worker_main();
  int handle_client(MQTTSocket* client);
  void destroy_server_socket();

  MQTTAllocator* allocator_;
  GlobalSessionManager* session_manager_;
  std::string host_;
  int port_;
  MQTTSocket* server_socket_;
  stCoRoutine_t* accept_coroutine_;
  std::atomic<bool> should_stop_;
  std::atomic<bool> running_;
  std::thread worker_thread_;
};

}  // namespace mqtt
