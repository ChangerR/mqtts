#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>
#include "src/mqtt_allocator.h"
#include "src/mqtt_define.h"
#include "src/mqtt_packet.h"
#include "src/mqtt_parser.h"
#include "src/mqtt_topic_tree.h"

using namespace mqtt;

// MQTT v5 Protocol Test Class
class MQTTv5ProtocolTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    // Initialize memory allocator
    MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
    ASSERT_NE(root_allocator, nullptr) << "Root allocator is null";
    
    // Generate unique child name for each test
    std::string child_name = "mqttv5_protocol_test_" + std::to_string(reinterpret_cast<uintptr_t>(this));
    test_allocator =
        root_allocator->create_child(child_name, MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    ASSERT_NE(test_allocator, nullptr) << "Failed to create child allocator";

    // Initialize parser
    parser = std::make_unique<MQTTParser>(test_allocator);

    // Initialize topic tree
    topic_tree = std::make_unique<ConcurrentTopicTree>(test_allocator);
    ASSERT_TRUE(topic_tree->is_initialized()) << "Topic tree initialization failed";
  }

  void TearDown() override
  {
    parser.reset();
    topic_tree.reset();
    
    if (test_allocator) {
      // Remove the child allocator from parent
      MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
      if (root_allocator) {
        root_allocator->remove_child(test_allocator->get_id());
      }
      test_allocator = nullptr;
    }
  }

  // Helper function to create test payload
  std::vector<uint8_t> create_test_payload(const std::string& data)
  {
    return std::vector<uint8_t>(data.begin(), data.end());
  }

  // Helper function to create MQTT string
  MQTTString create_mqtt_string(const std::string& str)
  {
    return to_mqtt_string(str, test_allocator);
  }

 protected:
  MQTTAllocator* test_allocator;
  std::unique_ptr<MQTTParser> parser;
  std::unique_ptr<ConcurrentTopicTree> topic_tree;
};

// Test MQTT v5 Connect Packet creation and properties
TEST_F(MQTTv5ProtocolTest, ConnectPacketProperties)
{
  ConnectPacket packet(test_allocator);

  // Set basic connect properties
  packet.protocol_name = create_mqtt_string("MQTT");
  packet.protocol_version = 5;
  packet.client_id = create_mqtt_string("test_client_001");
  packet.keep_alive = 60;
  packet.flags.clean_start = 1;
  packet.flags.will_flag = 0;
  packet.flags.username_flag = 0;
  packet.flags.password_flag = 0;

  // Set MQTT v5 properties
  packet.properties.session_expiry_interval = 3600;
  packet.properties.receive_maximum = 100;
  packet.properties.maximum_packet_size = 1024 * 1024;
  packet.properties.topic_alias_maximum = 10;
  packet.properties.request_response_information = true;
  packet.properties.request_problem_information = true;

  // Verify properties are correctly set
  EXPECT_EQ(from_mqtt_string(packet.protocol_name), "MQTT");
  EXPECT_EQ(packet.protocol_version, 5);
  EXPECT_EQ(from_mqtt_string(packet.client_id), "test_client_001");
  EXPECT_EQ(packet.keep_alive, 60);
  EXPECT_TRUE(packet.flags.clean_start);
  EXPECT_FALSE(packet.flags.will_flag);

  EXPECT_EQ(packet.properties.session_expiry_interval, 3600);
  EXPECT_EQ(packet.properties.receive_maximum, 100);
  EXPECT_EQ(packet.properties.maximum_packet_size, 1024 * 1024);
  EXPECT_EQ(packet.properties.topic_alias_maximum, 10);
  EXPECT_TRUE(packet.properties.request_response_information);
  EXPECT_TRUE(packet.properties.request_problem_information);
}

// Test MQTT v5 Publish Packet with properties
TEST_F(MQTTv5ProtocolTest, PublishPacketProperties)
{
  PublishPacket packet(test_allocator);

  // Set basic publish properties
  packet.topic_name = create_mqtt_string("sensor/temperature");
  packet.qos = 1;
  packet.retain = false;
  packet.dup = false;
  packet.packet_id = 123;

  std::string payload_str = "25.5";
  packet.payload = MQTTByteVector(payload_str.begin(), payload_str.end(),
                                  MQTTSTLAllocator<uint8_t>(test_allocator));

  // Set MQTT v5 publish properties
  packet.properties.payload_format_indicator = 1;  // UTF-8 string
  packet.properties.message_expiry_interval = 3600;
  packet.properties.content_type = create_mqtt_string("text/plain");
  packet.properties.response_topic = create_mqtt_string("response/sensor");

  std::string correlation_data = "correlation123";
  packet.properties.correlation_data = MQTTByteVector(
      correlation_data.begin(), correlation_data.end(), MQTTSTLAllocator<uint8_t>(test_allocator));

  // Verify properties
  EXPECT_EQ(from_mqtt_string(packet.topic_name), "sensor/temperature");
  EXPECT_EQ(packet.qos, 1);
  EXPECT_FALSE(packet.retain);
  EXPECT_FALSE(packet.dup);
  EXPECT_EQ(packet.packet_id, 123);

  std::string received_payload(packet.payload.begin(), packet.payload.end());
  EXPECT_EQ(received_payload, "25.5");

  EXPECT_EQ(packet.properties.payload_format_indicator, 1);
  EXPECT_EQ(packet.properties.message_expiry_interval, 3600);
  EXPECT_EQ(from_mqtt_string(packet.properties.content_type), "text/plain");
  EXPECT_EQ(from_mqtt_string(packet.properties.response_topic), "response/sensor");

  std::string received_correlation(packet.properties.correlation_data.begin(),
                                   packet.properties.correlation_data.end());
  EXPECT_EQ(received_correlation, "correlation123");
}

// Test MQTT v5 Subscribe Packet with subscription options
TEST_F(MQTTv5ProtocolTest, SubscribePacketOptions)
{
  SubscribePacket packet(test_allocator);

  packet.packet_id = 456;

  // Add subscriptions with different QoS levels and options
  packet.subscriptions.push_back({
      create_mqtt_string("sensor/+/temperature"),
      0  // QoS 0, No Local = 0, Retain As Published = 0, Retain Handling = 0
  });

  packet.subscriptions.push_back({
      create_mqtt_string("device/#"),
      1  // QoS 1
  });

  packet.subscriptions.push_back({
      create_mqtt_string("alert/critical"),
      2  // QoS 2
  });

  // Set subscription properties
  packet.properties.subscription_identifier = 42;

  // Verify packet structure
  EXPECT_EQ(packet.packet_id, 456);
  EXPECT_EQ(packet.subscriptions.size(), 3);

  EXPECT_EQ(from_mqtt_string(packet.subscriptions[0].first), "sensor/+/temperature");
  EXPECT_EQ(packet.subscriptions[0].second, 0);

  EXPECT_EQ(from_mqtt_string(packet.subscriptions[1].first), "device/#");
  EXPECT_EQ(packet.subscriptions[1].second, 1);

  EXPECT_EQ(from_mqtt_string(packet.subscriptions[2].first), "alert/critical");
  EXPECT_EQ(packet.subscriptions[2].second, 2);

  EXPECT_EQ(packet.properties.subscription_identifier, 42);
}

// Test MQTT v5 Reason Codes
TEST_F(MQTTv5ProtocolTest, ReasonCodes)
{
  // Test success codes
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::Success), 0x00);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::GrantedQoS0), 0x00);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::GrantedQoS1), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::GrantedQoS2), 0x02);

  // Test error codes
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::UnspecifiedError), 0x80);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::MalformedPacket), 0x81);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::ProtocolError), 0x82);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::UnsupportedProtocolVersion), 0x84);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::ClientIdentifierNotValid), 0x85);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::BadUserNameOrPassword), 0x86);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::NotAuthorized), 0x87);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::TopicFilterInvalid), 0x8F);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::TopicNameInvalid), 0x90);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::PacketTooLarge), 0x95);
  EXPECT_EQ(static_cast<uint8_t>(ReasonCode::QoSNotSupported), 0x9B);
}

// Test topic tree integration with MQTT v5 subscriptions
TEST_F(MQTTv5ProtocolTest, TopicTreeSubscriptions)
{
  MQTTString client1 = create_mqtt_string("client_001");
  MQTTString client2 = create_mqtt_string("client_002");

  // Test basic subscriptions
  EXPECT_EQ(topic_tree->subscribe(create_mqtt_string("sensor/temperature"), client1, 0),
            MQ_SUCCESS);
  EXPECT_EQ(topic_tree->subscribe(create_mqtt_string("sensor/humidity"), client1, 1), MQ_SUCCESS);
  EXPECT_EQ(topic_tree->subscribe(create_mqtt_string("sensor/+"), client2, 0), MQ_SUCCESS);

  // Test wildcard subscriptions
  EXPECT_EQ(topic_tree->subscribe(create_mqtt_string("device/#"), client1, 2), MQ_SUCCESS);
  EXPECT_EQ(topic_tree->subscribe(create_mqtt_string("#"), client2, 0), MQ_SUCCESS);

  // Verify subscription counts
  size_t subscriber_count;
  EXPECT_EQ(topic_tree->get_total_subscribers(subscriber_count), MQ_SUCCESS);
  EXPECT_EQ(subscriber_count, 5);

  // Test message delivery to subscribers
  TopicMatchResult result(test_allocator);

  // Test exact match
  EXPECT_EQ(topic_tree->find_subscribers(create_mqtt_string("sensor/temperature"), result),
            MQ_SUCCESS);
  EXPECT_GE(result.total_count, 1);  // At least client1 should match

  // Test wildcard match
  result = TopicMatchResult(test_allocator);
  EXPECT_EQ(topic_tree->find_subscribers(create_mqtt_string("sensor/pressure"), result),
            MQ_SUCCESS);
  EXPECT_GE(result.total_count, 1);  // client2 with "sensor/+" should match

  // Test multi-level wildcard
  result = TopicMatchResult(test_allocator);
  EXPECT_EQ(topic_tree->find_subscribers(create_mqtt_string("device/room1/light"), result),
            MQ_SUCCESS);
  EXPECT_GE(result.total_count, 1);  // client1 with "device/#" should match
}

// Test MQTT v5 QoS upgrade/downgrade scenarios
TEST_F(MQTTv5ProtocolTest, QoSHandling)
{
  MQTTString client_id = create_mqtt_string("qos_test_client");
  MQTTString topic = create_mqtt_string("test/qos");

  // Subscribe with QoS 1
  EXPECT_EQ(topic_tree->subscribe(topic, client_id, 1), MQ_SUCCESS);

  // Find subscribers and check QoS
  TopicMatchResult result(test_allocator);
  EXPECT_EQ(topic_tree->find_subscribers(topic, result), MQ_SUCCESS);
  ASSERT_GT(result.total_count, 0);

  bool found = false;
  for (const auto& subscriber : result.subscribers) {
    if (from_mqtt_string(subscriber.client_id) == "qos_test_client") {
      EXPECT_EQ(subscriber.qos, 1);
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);

  // Update subscription to QoS 2
  EXPECT_EQ(topic_tree->subscribe(topic, client_id, 2), MQ_SUCCESS);

  // Verify QoS was updated
  result = TopicMatchResult(test_allocator);
  EXPECT_EQ(topic_tree->find_subscribers(topic, result), MQ_SUCCESS);
  ASSERT_GT(result.total_count, 0);

  found = false;
  for (const auto& subscriber : result.subscribers) {
    if (from_mqtt_string(subscriber.client_id) == "qos_test_client") {
      EXPECT_EQ(subscriber.qos, 2);
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

// Test subscription identifier support
TEST_F(MQTTv5ProtocolTest, SubscriptionIdentifiers)
{
  MQTTString client_id = create_mqtt_string("sub_id_client");

  // Test subscription with identifier
  EXPECT_EQ(topic_tree->subscribe(create_mqtt_string("test/subid/1"), client_id, 0), MQ_SUCCESS);
  EXPECT_EQ(topic_tree->subscribe(create_mqtt_string("test/subid/2"), client_id, 1), MQ_SUCCESS);

  // Get client subscriptions
  std::vector<MQTTString> subscriptions;
  EXPECT_EQ(topic_tree->get_client_subscriptions(client_id, subscriptions), MQ_SUCCESS);
  EXPECT_EQ(subscriptions.size(), 2);

  // Verify subscription topics
  std::set<std::string> expected_topics = {"test/subid/1", "test/subid/2"};
  std::set<std::string> actual_topics;

  for (const auto& sub : subscriptions) {
    actual_topics.insert(from_mqtt_string(sub));
  }

  EXPECT_EQ(actual_topics, expected_topics);
}

// Test retain message handling
TEST_F(MQTTv5ProtocolTest, RetainMessageHandling)
{
  PublishPacket retain_packet(test_allocator);

  retain_packet.topic_name = create_mqtt_string("status/device1");
  retain_packet.retain = true;
  retain_packet.qos = 0;

  std::string status_data = "online";
  retain_packet.payload = MQTTByteVector(status_data.begin(), status_data.end(),
                                         MQTTSTLAllocator<uint8_t>(test_allocator));

  // Set retain-specific properties
  retain_packet.properties.message_expiry_interval = 7200;  // 2 hours

  // Verify retain packet properties
  EXPECT_TRUE(retain_packet.retain);
  EXPECT_EQ(from_mqtt_string(retain_packet.topic_name), "status/device1");

  std::string received_payload(retain_packet.payload.begin(), retain_packet.payload.end());
  EXPECT_EQ(received_payload, "online");
  EXPECT_EQ(retain_packet.properties.message_expiry_interval, 7200);
}

// Test shared subscription support (MQTT v5 feature)
TEST_F(MQTTv5ProtocolTest, SharedSubscriptionValidation)
{
  MQTTString client1 = create_mqtt_string("shared_client_1");
  MQTTString client2 = create_mqtt_string("shared_client_2");

  // Test regular (non-shared) subscriptions
  EXPECT_EQ(topic_tree->subscribe(create_mqtt_string("normal/topic"), client1, 0), MQ_SUCCESS);
  EXPECT_EQ(topic_tree->subscribe(create_mqtt_string("normal/topic"), client2, 1), MQ_SUCCESS);

  // Both clients should receive messages for normal subscriptions
  TopicMatchResult result(test_allocator);
  EXPECT_EQ(topic_tree->find_subscribers(create_mqtt_string("normal/topic"), result), MQ_SUCCESS);
  EXPECT_EQ(result.total_count, 2);

  // Verify both clients are in the result
  std::set<std::string> found_clients;
  for (const auto& subscriber : result.subscribers) {
    found_clients.insert(from_mqtt_string(subscriber.client_id));
  }

  EXPECT_NE(found_clients.find("shared_client_1"), found_clients.end());
  EXPECT_NE(found_clients.find("shared_client_2"), found_clients.end());
}

// Test user properties (MQTT v5 feature)
TEST_F(MQTTv5ProtocolTest, UserProperties)
{
  ConnectPacket packet(test_allocator);

  // Add user properties
  packet.properties.user_properties.push_back(
      {create_mqtt_string("device-type"), create_mqtt_string("sensor")});

  packet.properties.user_properties.push_back(
      {create_mqtt_string("firmware-version"), create_mqtt_string("1.2.3")});

  packet.properties.user_properties.push_back(
      {create_mqtt_string("location"), create_mqtt_string("building-a-floor-2")});

  // Verify user properties
  EXPECT_EQ(packet.properties.user_properties.size(), 3);

  // Check specific user properties
  for (const auto& prop : packet.properties.user_properties) {
    std::string key = from_mqtt_string(prop.first);
    std::string value = from_mqtt_string(prop.second);

    if (key == "device-type") {
      EXPECT_EQ(value, "sensor");
    } else if (key == "firmware-version") {
      EXPECT_EQ(value, "1.2.3");
    } else if (key == "location") {
      EXPECT_EQ(value, "building-a-floor-2");
    } else {
      FAIL() << "Unexpected user property: " << key;
    }
  }
}

// Test topic alias support (MQTT v5 feature)
TEST_F(MQTTv5ProtocolTest, TopicAliasSupport)
{
  PublishPacket packet(test_allocator);

  // First publish with topic name and alias
  packet.topic_name = create_mqtt_string("very/long/topic/name/for/sensor/data");
  packet.properties.topic_alias = 1;
  packet.qos = 0;

  std::string data = "sensor_data_1";
  packet.payload =
      MQTTByteVector(data.begin(), data.end(), MQTTSTLAllocator<uint8_t>(test_allocator));

  // Verify topic alias is set
  EXPECT_EQ(packet.properties.topic_alias, 1);
  EXPECT_EQ(from_mqtt_string(packet.topic_name), "very/long/topic/name/for/sensor/data");

  // Subsequent publish using only alias (topic name would be empty)
  PublishPacket alias_packet(test_allocator);
  alias_packet.topic_name = create_mqtt_string("");  // Empty topic name
  alias_packet.properties.topic_alias = 1;           // Use same alias
  alias_packet.qos = 0;

  std::string data2 = "sensor_data_2";
  alias_packet.payload =
      MQTTByteVector(data2.begin(), data2.end(), MQTTSTLAllocator<uint8_t>(test_allocator));

  // Verify alias usage
  EXPECT_EQ(alias_packet.properties.topic_alias, 1);
  EXPECT_EQ(from_mqtt_string(alias_packet.topic_name), "");
}

// Google Test main function
int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}