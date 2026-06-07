#include "mqtt_forwarding_rpc_client.h"
#include <arpa/inet.h>
#include <limits>

namespace mqtt
{

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
    if (MQ_FAIL(ret)) {
      ret = MQ_ERR_FORWARD_SEND;
    }
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
      } else {
        ret = MQ_ERR_FORWARD_RECV;
      }
    }
  }

  return ret;
}

}  // namespace

MQTTForwardingRpcClient::MQTTForwardingRpcClient(MQTTAllocator* allocator,
                                                 ForwardingConnectionPool* pool)
    : allocator_(allocator), pool_(pool)
{
}

MQTTForwardingRpcClient::~MQTTForwardingRpcClient()
{
}

void MQTTForwardingRpcClient::destroy_socket(MQTTSocket*& socket)
{
  MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();

  if (NULL != socket) {
    socket->close();
    socket->~MQTTSocket();
    if (NULL != root_allocator) {
      root_allocator->deallocate(socket, sizeof(MQTTSocket));
    }
    socket = NULL;
  }
}

int MQTTForwardingRpcClient::forward_publish(const std::string& host,
                                             int port,
                                             const MQTTString& origin_server_id,
                                             const MQTTString& origin_client_id,
                                             const MQTTString& topic,
                                             const MQTTByteVector& payload,
                                             uint8_t qos,
                                             bool retain,
                                             uint64_t message_id,
                                             const MQTTString& trace_id,
                                             int& delivered_count)
{
  int ret = MQ_SUCCESS;
  MQTTSocket* socket = NULL;
  uint32_t payload_len_n = 0;
  ::mqtt::forwarding::ForwardingEnvelope request_envelope;
  ::mqtt::forwarding::ForwardingEnvelope response_envelope;
  std::string request_data;
  std::string response_data;

  delivered_count = 0;
  bool is_pooled = false;
  if (host.empty() || port <= 0) {
    ret = MQ_ERR_FORWARD_TARGET_INVALID;
  } else if (pool_ != NULL) {
    ret = pool_->acquire(host, port, socket, is_pooled);
  } else if (MQ_FAIL(MQTTSocket::create_tcp_socket(socket))) {
    ret = MQ_ERR_FORWARD_CONNECT;
  } else if (MQ_FAIL(socket->connect(host.c_str(), port))) {
    ret = MQ_ERR_FORWARD_CONNECT;
  }

  if (MQ_SUCC(ret)) {
    request_envelope.set_request_id(message_id);
    ::mqtt::forwarding::ForwardPublishRequest* request =
        request_envelope.mutable_forward_publish_request();
    request->set_origin_server_id(from_mqtt_string(origin_server_id));
    request->set_origin_client_id(from_mqtt_string(origin_client_id));
    request->set_topic(from_mqtt_string(topic));
    request->set_qos(qos);
    request->set_retain(retain);
    request->set_message_id(message_id);
    request->set_trace_id(from_mqtt_string(trace_id));
    request->set_hop_count(1);
    if (!payload.empty()) {
      request->set_payload(payload.data(), payload.size());
    }
    if (!request_envelope.SerializeToString(&request_data)) {
      ret = MQ_ERR_FORWARD;
    } else {
      payload_len_n = htonl(static_cast<uint32_t>(request_data.size()));
      if (MQ_FAIL(send_all_bytes(socket, &payload_len_n, sizeof(payload_len_n)))) {
      } else if (MQ_FAIL(send_all_bytes(socket, request_data.data(), request_data.size()))) {
      } else if (MQ_FAIL(recv_all_bytes(socket, &payload_len_n, sizeof(payload_len_n)))) {
      } else {
        response_data.resize(ntohl(payload_len_n));
        if (response_data.empty()) {
          ret = MQ_ERR_FORWARD_RECV;
        } else if (MQ_FAIL(recv_all_bytes(socket, &response_data[0], response_data.size()))) {
        } else if (!response_envelope.ParseFromArray(response_data.data(),
                                                     static_cast<int>(response_data.size()))) {
          ret = MQ_ERR_FORWARD;
        } else if (!response_envelope.has_forward_publish_response()) {
          ret = MQ_ERR_FORWARD;
        } else {
          ret = response_envelope.forward_publish_response().error_code();
          delivered_count = response_envelope.forward_publish_response().delivered_count();
        }
      }
    }
  }

  const bool has_error = MQ_FAIL(ret);
  if (pool_ != NULL) {
    pool_->release(host, port, socket, is_pooled, has_error);
  } else {
    destroy_socket(socket);
  }

  return ret;
}

}  // namespace mqtt
