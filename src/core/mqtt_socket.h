#pragma once
#include <cstdint>
#include <string>
#include "mqtt_buffer.h"
#include "logger.h"
#include "mqtt_define.h"

class MQTTSocket
{
 public:
  MQTTSocket(int fd = -1) : fd_(fd), connected_(fd >= 0), peer_port_(0) {}
  virtual ~MQTTSocket() { close(); }

  // Socket operations
  int listen(const char* ip, int port, bool reuse = true, int backlog = 128);
  int accept(MQTTSocket*& client);  // 返回具体错误码
  int connect(const char* ip, int port);
  int send(const uint8_t* buf, int len, int timeout_ms = 5000);
  int send(const mqtt::MQTTBuffer& buffer);
  // 一次系统调用提交头部和载荷。拆成两次 send 时，TCP_NODELAY 会把头部单独推出去，
  // 对端一次 recv 就只看到没有载荷的 PUBLISH。
  int send_parts(const uint8_t* head, int head_len, const uint8_t* body, int body_len,
                 int timeout_ms = 5000);
  int recv(char* buf, int& len);
  // deadline_ms 是 steady_clock 的绝对毫秒。小于 0 表示一直等到有数据或对端关闭。
  // 截止时间到了仍没有字节时返回 MQ_ERR_SOCKET_TIMEOUT，不把套接字标成断开。
  int recv(char* buf, int& len, int64_t deadline_ms);
  int recv(mqtt::MQTTBuffer& buffer, int& len);
  bool is_websocket_upgrade_request(int timeout_ms = 1000);
  int close();

  // Socket configuration
  int set_nonblocking(bool nonblocking = true);
  int set_keepalive(bool keepalive = true, int idle = 60, int interval = 10, int count = 3);
  int set_buffer_size(int rcvbuf = 64 * 1024, int sndbuf = 64 * 1024);
  int set_reuse_addr(bool reuse = true);
  int set_tcp_nodelay(bool nodelay = true);

  // Status check
  bool is_connected() const { return connected_; }
  int get_fd() const { return fd_; }
  std::string get_peer_addr() const;
  int get_peer_port() const;

  // Static methods
  static int create_tcp_socket(MQTTSocket*& out_socket);
  static void set_addr(const char* ip, const unsigned short port, struct sockaddr_in& addr);

 private:
  int fd_;
  bool connected_;
  std::string peer_addr_;
  int peer_port_;

  int check_socket_error();
  bool update_peer_info();
};
