#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "mqtt_allocator.h"
#include "mqtt_socket.h"

TEST(MQTTSocketStateTest, UnopenedSocketHasNoPeer)
{
  MQTTSocket socket;
  EXPECT_EQ(socket.get_fd(), -1);
  EXPECT_FALSE(socket.is_connected());
  EXPECT_TRUE(socket.get_peer_addr().empty());
  EXPECT_EQ(socket.get_peer_port(), 0);
}

TEST(MQTTSocketStateTest, InvalidListenerClearsAcceptOutput)
{
  MQTTSocket listener;
  MQTTSocket previous;
  MQTTSocket* accepted = &previous;
  EXPECT_EQ(listener.accept(accepted), MQ_ERR_SOCKET_ACCEPT);
  EXPECT_EQ(accepted, nullptr);
  EXPECT_FALSE(listener.is_connected());
}

TEST(MQTTSocketStateTest, SetPeerEndpointUsesAcceptAddress)
{
  MQTTSocket socket;
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(42001);
  ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
  ASSERT_TRUE(socket.set_peer_endpoint(reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));
  EXPECT_EQ(socket.get_peer_addr(), "127.0.0.1");
  EXPECT_EQ(socket.get_peer_port(), 42001);
}

TEST(MQTTSocketStateTest, AcceptCopiesPeerFromAcceptSyscall)
{
  MQTTSocket* listener = nullptr;
  ASSERT_EQ(MQTTSocket::create_tcp_socket(listener), MQ_SUCCESS);
  ASSERT_NE(listener, nullptr);
  ASSERT_EQ(listener->listen("127.0.0.1", 0, true, 16), MQ_SUCCESS);

  struct sockaddr_in bound = {};
  socklen_t bound_len = sizeof(bound);
  ASSERT_EQ(getsockname(listener->get_fd(), reinterpret_cast<struct sockaddr*>(&bound), &bound_len),
            0);
  ASSERT_GT(ntohs(bound.sin_port), 0);

  const int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(client_fd, 0);
  struct sockaddr_in dest = bound;
  ASSERT_EQ(::connect(client_fd, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)), 0);

  struct sockaddr_in client_bound = {};
  socklen_t client_len = sizeof(client_bound);
  ASSERT_EQ(getsockname(client_fd, reinterpret_cast<struct sockaddr*>(&client_bound), &client_len),
            0);

  MQTTSocket* accepted = nullptr;
  ASSERT_EQ(listener->accept(accepted), MQ_SUCCESS);
  ASSERT_NE(accepted, nullptr);
  EXPECT_EQ(accepted->get_peer_addr(), "127.0.0.1");
  EXPECT_EQ(accepted->get_peer_port(), ntohs(client_bound.sin_port));
  EXPECT_TRUE(accepted->is_connected());

  ::close(client_fd);
  MQTTAllocator* root = MQ_MEM_MANAGER.get_root_allocator();
  accepted->~MQTTSocket();
  root->deallocate(accepted, sizeof(MQTTSocket));
  listener->~MQTTSocket();
  root->deallocate(listener, sizeof(MQTTSocket));
}

TEST(MQTTSocketStateTest, SetPeerEndpointRejectsShortOrNonIpv4Address)
{
  MQTTSocket socket;
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(42001);
  ASSERT_EQ(inet_pton(AF_INET, "10.0.0.8", &addr.sin_addr), 1);
  ASSERT_TRUE(socket.set_peer_endpoint(reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));

  EXPECT_FALSE(socket.set_peer_endpoint(reinterpret_cast<struct sockaddr*>(&addr), 4));
  EXPECT_TRUE(socket.get_peer_addr().empty());
  EXPECT_EQ(socket.get_peer_port(), 0);

  addr.sin_family = AF_UNIX;
  EXPECT_FALSE(socket.set_peer_endpoint(reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));
  EXPECT_TRUE(socket.get_peer_addr().empty());
  EXPECT_EQ(socket.get_peer_port(), 0);
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
