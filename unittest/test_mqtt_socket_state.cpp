#include <gtest/gtest.h>
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

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
