#pragma once
#include <string>
#include "logger.h"
#include "mqtt_define.h"

class MQTTSocket
{
 public:
  MQTTSocket(int fd = -1) : fd_(fd), connected_(true) {}
  virtual ~MQTTSocket() { close(); }

  // Socket operations
  int listen(const char* ip, int port, bool reuse = true);
  int accept(MQTTSocket*& client);  // 返回具体错误码
  int connect(const char* ip, int port);
  int send(const char* buf, int len);
  int recv(char* buf, int& len);
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
  void update_peer_info();
};