#include "mqtt_forwarding_service.h"
#include <arpa/inet.h>
#include <limits>
#include <poll.h>
#include "logger.h"
#include "mqtt_forwarding.pb.h"
#include "mqtt_packet.h"
#include "mqtt_session_manager_v2.h"

namespace mqtt
{

namespace
{

struct ForwardingClientContext
{
  MQTTForwardingService* service;
  MQTTSocket* client;
};

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

void destroy_client_socket(MQTTSocket*& client)
{
  MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();

  if (NULL != client) {
    client->close();
    client->~MQTTSocket();
    if (NULL != root_allocator) {
      root_allocator->deallocate(client, sizeof(MQTTSocket));
    }
    client = NULL;
  }
}

}  // namespace

MQTTForwardingService::MQTTForwardingService(MQTTAllocator* allocator,
                                             GlobalSessionManager* session_manager,
                                             const std::string& host,
                                             int port)
    : allocator_(allocator)
    , session_manager_(session_manager)
    , host_(host)
    , port_(port)
    , server_socket_(NULL)
    , accept_coroutine_(NULL)
    , should_stop_(false)
    , running_(false)
    , worker_thread_()
{
}

MQTTForwardingService::~MQTTForwardingService()
{
  (void)stop();
}

int MQTTForwardingService::start()
{
  int ret = MQ_SUCCESS;

  if (worker_thread_.joinable()) {
    ret = MQ_SUCCESS;
  } else if (MQ_FAIL(MQTTSocket::create_tcp_socket(server_socket_))) {
    ret = MQ_ERR_FORWARD_CONNECT;
  } else if (MQ_FAIL(server_socket_->listen(host_.c_str(), port_, true, 256))) {
    ret = MQ_ERR_FORWARD_CONNECT;
    destroy_server_socket();
  } else {
    should_stop_.store(false);
    running_.store(true);
    worker_thread_ = std::thread(&MQTTForwardingService::worker_main, this);
  }

  return ret;
}

int MQTTForwardingService::stop()
{
  int ret = MQ_SUCCESS;

  should_stop_.store(true);
  running_.store(false);
  if (NULL != server_socket_) {
    server_socket_->close();
  }
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  destroy_server_socket();

  return ret;
}

int MQTTForwardingService::is_running(bool& running) const
{
  int ret = MQ_SUCCESS;

  running = running_.load();

  return ret;
}

void MQTTForwardingService::destroy_server_socket()
{
  MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();

  if (NULL != server_socket_) {
    server_socket_->close();
    server_socket_->~MQTTSocket();
    if (NULL != root_allocator) {
      root_allocator->deallocate(server_socket_, sizeof(MQTTSocket));
    }
    server_socket_ = NULL;
  }
}

void MQTTForwardingService::worker_main()
{
  if (MQ_NOT_NULL(server_socket_)) {
    (void)co_create(&accept_coroutine_, NULL, accept_routine, this);
    if (MQ_NOT_NULL(accept_coroutine_)) {
      co_resume(accept_coroutine_);
      co_eventloop(co_get_epoll_ct(), eventloop_callback, this);
      co_release(accept_coroutine_);
      accept_coroutine_ = NULL;
    }
  }
}

void* MQTTForwardingService::accept_routine(void* arg)
{
  MQTTForwardingService* service = static_cast<MQTTForwardingService*>(arg);
  int ret = MQ_SUCCESS;

  if (MQ_ISNULL(service) || MQ_ISNULL(service->server_socket_)) {
    ret = MQ_ERR_INVALID_ARGS;
  }

  while (MQ_SUCC(ret) && !service->should_stop_.load()) {
    struct pollfd pf = {0};
    MQTTSocket* client = NULL;

    pf.fd = service->server_socket_->get_fd();
    pf.events = (POLLIN | POLLERR | POLLHUP);
    (void)co_poll(co_get_epoll_ct(), &pf, 1, 1000);

    if (service->should_stop_.load() || !service->server_socket_->is_connected()) {
      break;
    }

    ret = service->server_socket_->accept(client);
    if (MQ_ERR_SOCKET_ACCEPT_WOULDBLOCK == ret) {
      ret = MQ_SUCCESS;
    } else if (MQ_FAIL(ret)) {
      LOG_WARN("Forwarding service accept failed, ret={}", ret);
    } else {
      ForwardingClientContext* ctx = NULL;
      stCoRoutine_t* client_co = NULL;

      ctx = static_cast<ForwardingClientContext*>(service->allocator_->allocate(
          sizeof(ForwardingClientContext)));
      if (MQ_ISNULL(ctx)) {
        destroy_client_socket(client);
        ret = MQ_ERR_MEMORY_ALLOC;
      } else {
        ctx->service = service;
        ctx->client = client;
        (void)co_create(&client_co, NULL, client_routine, ctx);
        if (MQ_ISNULL(client_co)) {
          service->allocator_->deallocate(ctx, sizeof(ForwardingClientContext));
          destroy_client_socket(client);
          ret = MQ_ERR_MEMORY_ALLOC;
        } else {
          co_resume(client_co);
          ret = MQ_SUCCESS;
        }
      }
    }
  }

  return NULL;
}

void* MQTTForwardingService::client_routine(void* arg)
{
  ForwardingClientContext* ctx = static_cast<ForwardingClientContext*>(arg);

  if (NULL != ctx) {
    (void)ctx->service->handle_client(ctx->client);
    destroy_client_socket(ctx->client);
    ctx->service->allocator_->deallocate(ctx, sizeof(ForwardingClientContext));
  }

  return NULL;
}

int MQTTForwardingService::eventloop_callback(void* arg)
{
  MQTTForwardingService* service = static_cast<MQTTForwardingService*>(arg);
  int ret = 0;

  if (NULL != service && service->should_stop_.load()) {
    ret = -1;
  }

  return ret;
}

int MQTTForwardingService::handle_client(MQTTSocket* client)
{
  int ret = MQ_SUCCESS;
  uint32_t payload_len_n = 0;
  std::string request_data;
  ::mqtt::forwarding::ForwardingEnvelope request_envelope;
  ::mqtt::forwarding::ForwardingEnvelope response_envelope;
  int delivered_count = 0;
  bool response_sent = false;

  if (MQ_ISNULL(client)) {
    ret = MQ_ERR_INVALID_ARGS;
  } else if (MQ_FAIL(recv_all_bytes(client, &payload_len_n, sizeof(payload_len_n)))) {
  } else {
    request_data.resize(ntohl(payload_len_n));
    if (request_data.empty()) {
      ret = MQ_ERR_FORWARD_RECV;
    } else if (MQ_FAIL(recv_all_bytes(client, &request_data[0], request_data.size()))) {
    } else if (!request_envelope.ParseFromArray(request_data.data(),
                                                static_cast<int>(request_data.size()))) {
      ret = MQ_ERR_FORWARD;
    } else if (!request_envelope.has_forward_publish_request()) {
      ret = MQ_ERR_FORWARD;
    } else if (request_envelope.forward_publish_request().hop_count() > 1) {
      ret = MQ_ERR_FORWARD_LOOP;
    } else {
      PublishPacket packet(allocator_);
      packet.topic_name =
          to_mqtt_string(request_envelope.forward_publish_request().topic(), allocator_);
      packet.qos = static_cast<uint8_t>(request_envelope.forward_publish_request().qos());
      packet.retain = request_envelope.forward_publish_request().retain();
      packet.dup = false;
      if (!request_envelope.forward_publish_request().payload().empty()) {
        const std::string& payload = request_envelope.forward_publish_request().payload();
        packet.payload.assign(payload.begin(), payload.end());
      }
      delivered_count = session_manager_->forward_publish_by_topic(
          packet.topic_name, packet,
          to_mqtt_string(request_envelope.forward_publish_request().origin_client_id(),
                         allocator_));
      if (delivered_count < 0) {
        ret = delivered_count;
      } else {
        response_envelope.set_request_id(request_envelope.request_id());
        response_envelope.mutable_forward_publish_response()->set_error_code(MQ_SUCCESS);
        response_envelope.mutable_forward_publish_response()->set_delivered_count(delivered_count);
        request_data.clear();
        if (!response_envelope.SerializeToString(&request_data)) {
          ret = MQ_ERR_FORWARD;
        } else {
          payload_len_n = htonl(static_cast<uint32_t>(request_data.size()));
          if (MQ_FAIL(send_all_bytes(client, &payload_len_n, sizeof(payload_len_n)))) {
          } else if (MQ_FAIL(send_all_bytes(client, request_data.data(), request_data.size()))) {
          } else {
            response_sent = true;
          }
        }
      }
    }
  }

  if (!response_sent && MQ_NOT_NULL(client)) {
    request_data.clear();
    response_envelope.Clear();
    response_envelope.set_request_id(request_envelope.request_id());
    response_envelope.mutable_forward_publish_response()->set_error_code(ret);
    response_envelope.mutable_forward_publish_response()->set_delivered_count(0);
    if (response_envelope.SerializeToString(&request_data)) {
      payload_len_n = htonl(static_cast<uint32_t>(request_data.size()));
      (void)send_all_bytes(client, &payload_len_n, sizeof(payload_len_n));
      (void)send_all_bytes(client, request_data.data(), request_data.size());
    }
  }

  return ret;
}

}  // namespace mqtt
