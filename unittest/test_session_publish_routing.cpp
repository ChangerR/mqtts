#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "coroutine_test_helper.h"
#include "logger.h"
#include "mqtt_allocator.h"
#include "mqtt_define.h"
#include "mqtt_memory_tags.h"
#include "mqtt_packet.h"
#include "mqtt_protocol_handler.h"
#include "mqtt_session_manager_v2.h"

using namespace mqtt;

namespace {

class RoutingMockHandler : public MQTTProtocolHandler
{
 public:
  explicit RoutingMockHandler(const std::string& client_id)
      : MQTTProtocolHandler(MQTTMemoryManager::get_instance().get_root_allocator())
  {
    set_client_id(to_mqtt_string(client_id, MQTTMemoryManager::get_instance().get_root_allocator()));
  }
};

class PublishRoutingTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    coro_scope_.reset(new mqtt::test::CoroutineTestScope());
    if (!coro_scope_->is_available()) {
      GTEST_SKIP() << "Coroutine runtime is not available in this environment";
    }

    manager_.reset(new GlobalSessionManager());
    ASSERT_EQ(MQ_SUCCESS, manager_->pre_register_threads(1, 100));

    thread_manager_ = manager_->register_thread_manager(std::this_thread::get_id());
    ASSERT_NE(nullptr, thread_manager_);
    ASSERT_EQ(MQ_SUCCESS, manager_->finalize_thread_registration());

    allocator_ = thread_manager_->get_allocator();
  }

  void TearDown() override
  {
    handlers_.clear();
    manager_.reset();
    coro_scope_.reset();
    MQTTMemoryManager::cleanup_thread_local();
  }

  void add_subscriber(const std::string& client_id, const std::string& topic_filter)
  {
    handlers_.push_back(
        std::unique_ptr<RoutingMockHandler>(new RoutingMockHandler(client_id)));

    MQTTString mqtt_client_id = to_mqtt_string(client_id, allocator_);
    ASSERT_EQ(MQ_SUCCESS, manager_->register_session(mqtt_client_id, handlers_.back().get()));
    ASSERT_EQ(MQ_SUCCESS,
              manager_->subscribe_topic(to_mqtt_string(topic_filter, allocator_), mqtt_client_id, 0));
  }

  int publish(const std::string& topic, const std::string& sender_client_id)
  {
    PublishPacket packet(allocator_);
    packet.type = PacketType::PUBLISH;
    packet.topic_name = to_mqtt_string(topic, allocator_);
    packet.qos = 0;

    return manager_->forward_publish_by_topic(packet.topic_name, packet,
                                              to_mqtt_string(sender_client_id, allocator_));
  }

  std::unique_ptr<mqtt::test::CoroutineTestScope> coro_scope_;
  std::unique_ptr<GlobalSessionManager> manager_;
  ThreadLocalSessionManager* thread_manager_ = nullptr;
  MQTTAllocator* allocator_ = nullptr;
  std::vector<std::unique_ptr<RoutingMockHandler>> handlers_;
};

// 发布者自己订阅了目标主题时必须收到自己发布的消息：MQTT 3.1.1 没有例外，MQTT 5 也
// 只有显式设置 No Local 才不投递。
TEST_F(PublishRoutingTest, PublisherReceivesItsOwnMessageWhenSubscribed)
{
  add_subscriber("self_publisher", "routing/self");

  size_t before = thread_manager_->get_pending_message_count();
  EXPECT_EQ(1, publish("routing/self", "self_publisher"));
  EXPECT_EQ(before + 1, thread_manager_->get_pending_message_count());
}

TEST_F(PublishRoutingTest, DeliversToEverySubscriberIncludingThePublisher)
{
  add_subscriber("publisher", "routing/shared");
  add_subscriber("listener", "routing/shared");

  size_t before = thread_manager_->get_pending_message_count();
  EXPECT_EQ(2, publish("routing/shared", "publisher"));
  EXPECT_EQ(before + 2, thread_manager_->get_pending_message_count());
}

TEST_F(PublishRoutingTest, ReportsZeroWhenNobodySubscribed)
{
  add_subscriber("listener", "routing/other");

  size_t before = thread_manager_->get_pending_message_count();
  EXPECT_EQ(0, publish("routing/unmatched", "publisher"));
  EXPECT_EQ(before, thread_manager_->get_pending_message_count());
}

}  // namespace

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
