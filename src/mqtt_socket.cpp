#include "mqtt_socket.h"
#include "logger.h"
#include "mqtt_allocator.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "co_routine.h"

int co_accept(int fd, struct sockaddr* addr, socklen_t* len);

int MQTTSocket::listen(const char* ip, int port, bool reuse)
{
  int ret = MQ_SUCCESS;
  struct sockaddr_in addr;
  int sock_ret = -1;

  // Set socket options
  if (MQ_FAIL(set_reuse_addr(reuse))) {
    LOG_ERROR("Failed to set SO_REUSEADDR option");
    return MQ_ERR_SOCKET;
  }

  if (MQ_FAIL(set_tcp_nodelay(true))) {
    LOG_WARN("Failed to set TCP_NODELAY option");
  }

  if (MQ_FAIL(set_buffer_size())) {
    LOG_WARN("Failed to set socket buffer size");
  }

  // Set socket to non-blocking mode
  if (MQ_FAIL(set_nonblocking(true))) {
    LOG_ERROR("Failed to set socket to non-blocking mode");
    return MQ_ERR_SOCKET;
  }

  set_addr(ip, port, addr);
  sock_ret = bind(fd_, (struct sockaddr*)&addr, sizeof(addr));
  if (sock_ret != 0) {
    LOG_ERROR("Failed to bind socket to {}:{} - {}", ip, port, strerror(errno));
    ret = MQ_ERR_SOCKET_BIND;
    close();
  }

  if (MQ_SUCC(ret)) {
    sock_ret = ::listen(fd_, 1024);
    if (sock_ret != 0) {
      LOG_ERROR("Failed to listen on socket - {}", strerror(errno));
      ret = MQ_ERR_SOCKET_LISTEN;
      close();
    }
  }

  if (MQ_SUCC(ret)) {
    co_enable_hook_sys();
    LOG_INFO("Socket listening on {}:{}", ip, port);
  }

  return ret;
}

int MQTTSocket::accept(MQTTSocket*& client)
{
  int ret = MQ_SUCCESS;
  struct sockaddr addr;
  socklen_t len = sizeof(struct sockaddr);

  while (true) {
    int fd = co_accept(fd_, (struct sockaddr*)&addr, &len);

    if (fd < 0) {
      if (errno == EINTR) {
        // System call was interrupted, try again
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        ret = MQ_ERR_SOCKET_ACCEPT_WOULDBLOCK;
      } else {
        LOG_ERROR("Failed to accept connection - {}", strerror(errno));
        ret = MQ_ERR_SOCKET_ACCEPT;
      }
      break;
    } else {
      // Use root allocator for client socket
      MQTTAllocator* root = MQ_MEM_MANAGER.get_root_allocator();
      if (!root) {
        LOG_ERROR("Failed to get root allocator");
        ret = MQ_ERR_SOCKET_ALLOC;
        ::close(fd);
        break;
      }

      client = (MQTTSocket*)root->allocate(sizeof(MQTTSocket));
      if (MQ_ISNULL(client)) {
        LOG_ERROR("Failed to allocate socket client");
        ret = MQ_ERR_SOCKET_ALLOC;
        ::close(fd);
        break;
      }

      new (client) MQTTSocket(fd);
      client->connected_ = true;
      client->update_peer_info();
      LOG_DEBUG("Accepted new connection from {}:{}", client->get_peer_addr(),
                client->get_peer_port());
      break;
    }
  }

  return ret;
}

int MQTTSocket::connect(const char* ip, int port)
{
  int ret = MQ_SUCCESS;
  struct sockaddr_in addr;

  set_addr(ip, port, addr);

  if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    LOG_ERROR("Failed to connect to {}:{} - {}", ip, port, strerror(errno));
    ret = MQ_ERR_SOCKET_CONNECT;
    close();
  } else {
    connected_ = true;
    update_peer_info();
    LOG_INFO("Connected to {}:{}", ip, port);
  }

  return ret;
}

int MQTTSocket::send(const uint8_t* buf, int len)
{
  int ret = MQ_SUCCESS;

  if (!connected_) {
    LOG_ERROR("Socket not connected");
    ret = MQ_ERR_SOCKET;
  } else {
    int total_sent = 0;
    while (total_sent < len && MQ_SUCC(ret)) {
      int sent = ::write(fd_, buf + total_sent, len - total_sent);
      if (sent < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // Use co_poll to wait for buffer space
          struct pollfd pf = {0};
          pf.fd = fd_;
          pf.events = (POLLOUT | POLLERR | POLLHUP);
          co_poll(co_get_epoll_ct(), &pf, 1, -1);  // Wait indefinitely
          continue;                                // Try sending again after poll
        } else {
          LOG_ERROR("Failed to send data - {}", strerror(errno));
          ret = MQ_ERR_SOCKET_SEND;
        }
      } else {
        total_sent += sent;
      }
    }
  }

  return ret;
}

int MQTTSocket::recv(char* buf, int& len)
{
  int ret = MQ_SUCCESS;

  if (!connected_) {
    LOG_ERROR("Socket not connected");
    ret = MQ_ERR_SOCKET;
  } else {
    while (MQ_SUCC(ret)) {
      int received = ::read(fd_, buf, len);
      if (received < 0) {
        if (errno == EINTR) {
          continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // Use co_poll to wait for data
          struct pollfd pf = {0};
          pf.fd = fd_;
          pf.events = (POLLIN | POLLERR | POLLHUP);
          co_poll(co_get_epoll_ct(), &pf, 1, -1);  // Wait indefinitely
          continue;
        } else {
          LOG_ERROR("Failed to receive data - {}", strerror(errno));
          ret = MQ_ERR_SOCKET_RECV;
        }
      } else if (received == 0) {
        LOG_INFO("Connection closed by peer");
        connected_ = false;
        ret = MQ_ERR_SOCKET;
      } else {
        len = received;
        ret = MQ_SUCCESS;
        break;  // Break after successful read
      }
    }
  }

  return ret;
}

int MQTTSocket::close()
{
  if (fd_ >= 0) {
    if (connected_) {
      LOG_DEBUG("Closing connection to {}:{}", get_peer_addr(), get_peer_port());
    }
    ::close(fd_);
    fd_ = -1;
    connected_ = false;
  }
  return MQ_SUCCESS;
}

int MQTTSocket::set_nonblocking(bool nonblocking)
{
  int ret = MQ_SUCCESS;
  int flags = fcntl(fd_, F_GETFL, 0);

  if (flags < 0) {
    LOG_ERROR("Failed to get socket flags - {}", strerror(errno));
    ret = MQ_ERR_SOCKET;
  } else {
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(fd_, F_SETFL, flags) < 0) {
      LOG_ERROR("Failed to set socket flags - {}", strerror(errno));
      ret = MQ_ERR_SOCKET;
    }
  }

  return ret;
}

int MQTTSocket::set_keepalive(bool keepalive, int idle, int interval, int count)
{
  int ret = MQ_SUCCESS;
  int optval = keepalive ? 1 : 0;

  if (setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0) {
    LOG_ERROR("Failed to set SO_KEEPALIVE - {}", strerror(errno));
    ret = MQ_ERR_SOCKET;
  } else if (keepalive) {
    if (setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) < 0) {
      LOG_ERROR("Failed to set TCP_KEEPIDLE - {}", strerror(errno));
      ret = MQ_ERR_SOCKET;
    } else if (setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) < 0) {
      LOG_ERROR("Failed to set TCP_KEEPINTVL - {}", strerror(errno));
      ret = MQ_ERR_SOCKET;
    } else if (setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0) {
      LOG_ERROR("Failed to set TCP_KEEPCNT - {}", strerror(errno));
      ret = MQ_ERR_SOCKET;
    }
  }

  return ret;
}

int MQTTSocket::set_buffer_size(int rcvbuf, int sndbuf)
{
  int ret = MQ_SUCCESS;

  if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
    LOG_ERROR("Failed to set SO_RCVBUF - {}", strerror(errno));
    ret = MQ_ERR_SOCKET;
  } else if (setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
    LOG_ERROR("Failed to set SO_SNDBUF - {}", strerror(errno));
    ret = MQ_ERR_SOCKET;
  }

  return ret;
}

int MQTTSocket::set_reuse_addr(bool reuse)
{
  int ret = MQ_SUCCESS;
  int optval = reuse ? 1 : 0;

  if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
    LOG_ERROR("Failed to set SO_REUSEADDR - {}", strerror(errno));
    ret = MQ_ERR_SOCKET;
  }

  return ret;
}

int MQTTSocket::set_tcp_nodelay(bool nodelay)
{
  int ret = MQ_SUCCESS;
  int optval = nodelay ? 1 : 0;

  if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) < 0) {
    LOG_ERROR("Failed to set TCP_NODELAY - {}", strerror(errno));
    ret = MQ_ERR_SOCKET;
  }

  return ret;
}

std::string MQTTSocket::get_peer_addr() const
{
  return peer_addr_;
}

int MQTTSocket::get_peer_port() const
{
  return peer_port_;
}

void MQTTSocket::update_peer_info()
{
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);

  if (getpeername(fd_, (struct sockaddr*)&addr, &len) == 0) {
    peer_addr_ = inet_ntoa(addr.sin_addr);
    peer_port_ = ntohs(addr.sin_port);
  }
}

int MQTTSocket::check_socket_error()
{
  int ret = MQ_SUCCESS;
  int error = 0;
  socklen_t len = sizeof(error);

  if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
    LOG_ERROR("Failed to get socket error - {}", strerror(errno));
    ret = MQ_ERR_SOCKET;
  } else if (error != 0) {
    LOG_ERROR("Socket error: {}", strerror(error));
    ret = MQ_ERR_SOCKET;
  }

  return ret;
}

int MQTTSocket::create_tcp_socket(MQTTSocket*& out_sock)
{
  int ret = MQ_SUCCESS;
  MQTTSocket* sock = NULL;
  int fd = -1;

  if ((fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
    LOG_ERROR("Failed to create socket - {}", strerror(errno));
    ret = MQ_ERR_SOCKET;
  } else {
    // Use root allocator for socket allocation
    MQTTAllocator* root = MQ_MEM_MANAGER.get_root_allocator();
    if (!root) {
      LOG_ERROR("Failed to get root allocator");
      ret = MQ_ERR_SOCKET_ALLOC;
      ::close(fd);
    } else {
      sock = (MQTTSocket*)root->allocate(sizeof(MQTTSocket));
      if (MQ_ISNULL(sock)) {
        LOG_ERROR("Failed to allocate socket object");
        ret = MQ_ERR_SOCKET_ALLOC;
        ::close(fd);
      } else {
        new (sock) MQTTSocket(fd);
        out_sock = sock;
      }
    }
  }

  return ret;
}

void MQTTSocket::set_addr(const char* ip, const unsigned short port, struct sockaddr_in& addr)
{
  int n_ip = 0;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if (!ip || '\0' == *ip || 0 == strcmp(ip, "0") || 0 == strcmp(ip, "0.0.0.0") ||
      0 == strcmp(ip, "*")) {
    n_ip = htonl(INADDR_ANY);
  } else {
    n_ip = inet_addr(ip);
  }
  addr.sin_addr.s_addr = n_ip;
}
