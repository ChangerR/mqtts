#pragma once

#include <string>
#include "forwarding_connection_pool.h"
#include "mqtt_allocator.h"
#include "mqtt_define.h"
#include "mqtt_forwarding.pb.h"
#include "mqtt_socket.h"
#include "mqtt_string_utils.h"
#include "mqtt_stl_allocator.h"

namespace mqtt
{

class MQTTForwardingRpcClient
{
 public:
  /**
   * @param allocator  内存分配器
   * @param pool       连接池（可为 nullptr，为 nullptr 时退化为原有逐次建连行为）
   */
  MQTTForwardingRpcClient(MQTTAllocator* allocator, ForwardingConnectionPool* pool);
  ~MQTTForwardingRpcClient();

  int forward_publish(const std::string& host,
                      int port,
                      const MQTTString& origin_server_id,
                      const MQTTString& origin_client_id,
                      const MQTTString& topic,
                      const MQTTByteVector& payload,
                      uint8_t qos,
                      bool retain,
                      uint64_t message_id,
                      const MQTTString& trace_id,
                      int& delivered_count);

 private:
  void destroy_socket(MQTTSocket*& socket);

  MQTTAllocator*           allocator_;
  ForwardingConnectionPool* pool_;  // 不拥有，生命周期由 GlobalSessionManager 管理
};

}  // namespace mqtt
