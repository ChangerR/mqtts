#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "src/mqtt_allocator.h"
#include "src/mqtt_define.h"
#include "src/mqtt_topic_tree.h"

using namespace mqtt;

// Test fixture class for topic tree tests
class TopicTreeTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    // Initialize allocator and tree for each test
    MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
    tree_allocator =
        root_allocator->create_child("test_topic_tree", MQTTMemoryTag::MEM_TAG_TOPIC_TREE, 0);
    tree = std::make_unique<ConcurrentTopicTree>(tree_allocator);

    // Check if tree initialized properly
    ASSERT_TRUE(tree->is_initialized()) << "Topic tree failed to initialize";
  }

  void TearDown() override { tree.reset(); }

  // Helper functions
  MQTTString create_mqtt_string(const std::string& str) { return to_mqtt_string(str, nullptr); }

  std::unique_ptr<ConcurrentTopicTree> tree;
  MQTTAllocator* tree_allocator;
};

// Test fixture for performance tests (separate to avoid interference)
class TopicTreePerfTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
    tree_allocator =
        root_allocator->create_child("perf_test_topic_tree", MQTTMemoryTag::MEM_TAG_TOPIC_TREE, 0);
    tree = std::make_unique<ConcurrentTopicTree>(tree_allocator);
    ASSERT_TRUE(tree->is_initialized());
  }

  void TearDown() override { tree.reset(); }

  MQTTString create_mqtt_string(const std::string& str) { return to_mqtt_string(str, nullptr); }

  std::unique_ptr<ConcurrentTopicTree> tree;
  MQTTAllocator* tree_allocator;
};

// Basic subscription tests
TEST_F(TopicTreeTest, BasicSubscribe)
{
  MQTTString topic = create_mqtt_string("sensor/temperature");
  MQTTString client_id = create_mqtt_string("client1");

  int result = tree->subscribe(topic, client_id, 1);
  EXPECT_EQ(result, MQ_SUCCESS);

  size_t subscriber_count, node_count;
  EXPECT_EQ(tree->get_total_subscribers(subscriber_count), MQ_SUCCESS);
  EXPECT_EQ(tree->get_total_nodes(node_count), MQ_SUCCESS);

  EXPECT_EQ(subscriber_count, 1);
  EXPECT_GT(node_count, 1);
}

TEST_F(TopicTreeTest, BasicUnsubscribe)
{
  MQTTString topic = create_mqtt_string("sensor/temperature");
  MQTTString client_id = create_mqtt_string("client1");

  // Subscribe first
  EXPECT_EQ(tree->subscribe(topic, client_id, 1), MQ_SUCCESS);

  size_t subscriber_count;
  EXPECT_EQ(tree->get_total_subscribers(subscriber_count), MQ_SUCCESS);
  EXPECT_EQ(subscriber_count, 1);

  // Then unsubscribe
  EXPECT_EQ(tree->unsubscribe(topic, client_id), MQ_SUCCESS);

  EXPECT_EQ(tree->get_total_subscribers(subscriber_count), MQ_SUCCESS);
  EXPECT_EQ(subscriber_count, 0);
}

// Topic matching tests
TEST_F(TopicTreeTest, ExactTopicMatch)
{
  MQTTString topic_filter = create_mqtt_string("sensor/temperature");
  MQTTString client_id = create_mqtt_string("client1");

  EXPECT_EQ(tree->subscribe(topic_filter, client_id, 1), MQ_SUCCESS);

  // Exact match
  MQTTString publish_topic = create_mqtt_string("sensor/temperature");
  TopicMatchResult result(tree_allocator);
  EXPECT_EQ(tree->find_subscribers(publish_topic, result), MQ_SUCCESS);

  EXPECT_EQ(result.total_count, 1);
  EXPECT_EQ(result.subscribers.size(), 1);
  EXPECT_EQ(from_mqtt_string(result.subscribers[0].client_id), "client1");
  EXPECT_EQ(result.subscribers[0].qos, 1);
}

TEST_F(TopicTreeTest, TopicNoMatch)
{
  MQTTString topic_filter = create_mqtt_string("sensor/temperature");
  MQTTString client_id = create_mqtt_string("client1");

  EXPECT_EQ(tree->subscribe(topic_filter, client_id, 1), MQ_SUCCESS);

  // Non-matching topic
  MQTTString publish_topic = create_mqtt_string("sensor/humidity");
  TopicMatchResult result(tree_allocator);
  EXPECT_EQ(tree->find_subscribers(publish_topic, result), MQ_SUCCESS);

  EXPECT_EQ(result.total_count, 0);
  EXPECT_EQ(result.subscribers.size(), 0);
}

// Wildcard tests
TEST_F(TopicTreeTest, SingleLevelWildcard)
{
  MQTTString topic_filter = create_mqtt_string("sensor/+/data");
  MQTTString client_id = create_mqtt_string("client1");

  EXPECT_EQ(tree->subscribe(topic_filter, client_id, 1), MQ_SUCCESS);

  // Should match
  std::vector<std::string> matching_topics = {"sensor/temperature/data", "sensor/humidity/data",
                                              "sensor/pressure/data"};

  for (const std::string& topic_str : matching_topics) {
    MQTTString topic = create_mqtt_string(topic_str);
    TopicMatchResult result(tree_allocator);
    EXPECT_EQ(tree->find_subscribers(topic, result), MQ_SUCCESS);
    EXPECT_EQ(result.total_count, 1) << "Failed to match topic: " << topic_str;
  }

  // Should not match
  std::vector<std::string> non_matching_topics = {
      "sensor/temperature", "sensor/temperature/data/extra", "device/temperature/data"};

  for (const std::string& topic_str : non_matching_topics) {
    MQTTString topic = create_mqtt_string(topic_str);
    TopicMatchResult result(tree_allocator);
    EXPECT_EQ(tree->find_subscribers(topic, result), MQ_SUCCESS);
    EXPECT_EQ(result.total_count, 0) << "Incorrectly matched topic: " << topic_str;
  }
}

TEST_F(TopicTreeTest, MultiLevelWildcard)
{
  MQTTString topic_filter = create_mqtt_string("sensor/#");
  MQTTString client_id = create_mqtt_string("client1");

  EXPECT_EQ(tree->subscribe(topic_filter, client_id, 1), MQ_SUCCESS);

  // Should match
  std::vector<std::string> matching_topics = {"sensor/temperature", "sensor/temperature/data",
                                              "sensor/humidity/room1/data",
                                              "sensor/pressure/device1/room1/data"};

  for (const std::string& topic_str : matching_topics) {
    MQTTString topic = create_mqtt_string(topic_str);
    TopicMatchResult result(tree_allocator);
    EXPECT_EQ(tree->find_subscribers(topic, result), MQ_SUCCESS);
    EXPECT_EQ(result.total_count, 1) << "Failed to match topic: " << topic_str;
  }

  // Should not match
  std::vector<std::string> non_matching_topics = {"device/temperature", "sensors/temperature"};

  for (const std::string& topic_str : non_matching_topics) {
    MQTTString topic = create_mqtt_string(topic_str);
    TopicMatchResult result(tree_allocator);
    EXPECT_EQ(tree->find_subscribers(topic, result), MQ_SUCCESS);
    EXPECT_EQ(result.total_count, 0) << "Incorrectly matched topic: " << topic_str;
  }
}

TEST_F(TopicTreeTest, RootLevelMultiWildcard)
{
  MQTTString topic_filter = create_mqtt_string("#");
  MQTTString client_id = create_mqtt_string("client1");

  EXPECT_EQ(tree->subscribe(topic_filter, client_id, 1), MQ_SUCCESS);

  // Should match all topics
  std::vector<std::string> matching_topics = {"sensor", "sensor/temperature",
                                              "device/control/power", "a/b/c/d/e/f/g"};

  for (const std::string& topic_str : matching_topics) {
    MQTTString topic = create_mqtt_string(topic_str);
    TopicMatchResult result(tree_allocator);
    EXPECT_EQ(tree->find_subscribers(topic, result), MQ_SUCCESS);
    EXPECT_EQ(result.total_count, 1) << "Failed to match topic: " << topic_str;
  }
}

// Multi-client tests
TEST_F(TopicTreeTest, MultipleSubscribersToSameTopic)
{
  MQTTString topic = create_mqtt_string("sensor/temperature");
  std::vector<std::string> client_ids = {"client1", "client2", "client3"};

  for (size_t i = 0; i < client_ids.size(); ++i) {
    MQTTString client_id = create_mqtt_string(client_ids[i]);
    uint8_t qos = static_cast<uint8_t>(i);
    EXPECT_EQ(tree->subscribe(topic, client_id, qos), MQ_SUCCESS);
  }

  size_t subscriber_count;
  EXPECT_EQ(tree->get_total_subscribers(subscriber_count), MQ_SUCCESS);
  EXPECT_EQ(subscriber_count, 3);

  TopicMatchResult result(tree_allocator);
  EXPECT_EQ(tree->find_subscribers(topic, result), MQ_SUCCESS);
  EXPECT_EQ(result.total_count, 3);
  EXPECT_EQ(result.subscribers.size(), 3);

  // Verify all clients are found
  std::set<std::string> found_clients;
  for (const auto& subscriber : result.subscribers) {
    found_clients.insert(from_mqtt_string(subscriber.client_id));
  }

  for (const std::string& expected_client : client_ids) {
    EXPECT_NE(found_clients.find(expected_client), found_clients.end())
        << "Client not found: " << expected_client;
  }
}

// QoS tests
TEST_F(TopicTreeTest, QoSUpdate)
{
  MQTTString topic = create_mqtt_string("sensor/temperature");
  MQTTString client_id = create_mqtt_string("client1");

  // Initial subscription with QoS 0
  EXPECT_EQ(tree->subscribe(topic, client_id, 0), MQ_SUCCESS);

  TopicMatchResult result1(tree_allocator);
  EXPECT_EQ(tree->find_subscribers(topic, result1), MQ_SUCCESS);
  EXPECT_EQ(result1.total_count, 1);
  EXPECT_EQ(result1.subscribers[0].qos, 0);

  // Update to QoS 2
  EXPECT_EQ(tree->subscribe(topic, client_id, 2), MQ_SUCCESS);

  TopicMatchResult result2(tree_allocator);
  EXPECT_EQ(tree->find_subscribers(topic, result2), MQ_SUCCESS);
  EXPECT_EQ(result2.total_count, 1);         // Still only one subscriber
  EXPECT_EQ(result2.subscribers[0].qos, 2);  // QoS updated
}

// Subscription management tests
TEST_F(TopicTreeTest, GetClientSubscriptions)
{
  MQTTString client_id = create_mqtt_string("client1");

  std::vector<std::string> topics = {"sensor/temperature", "sensor/humidity", "device/+/status",
                                     "logs/#"};

  for (const std::string& topic_str : topics) {
    MQTTString topic = create_mqtt_string(topic_str);
    EXPECT_EQ(tree->subscribe(topic, client_id, 1), MQ_SUCCESS);
  }

  std::vector<MQTTString> subscriptions;
  EXPECT_EQ(tree->get_client_subscriptions(client_id, subscriptions), MQ_SUCCESS);
  EXPECT_EQ(subscriptions.size(), topics.size());

  std::set<std::string> found_topics;
  for (const MQTTString& subscription : subscriptions) {
    found_topics.insert(from_mqtt_string(subscription));
  }

  for (const std::string& expected_topic : topics) {
    EXPECT_NE(found_topics.find(expected_topic), found_topics.end())
        << "Topic not found: " << expected_topic;
  }
}

TEST_F(TopicTreeTest, UnsubscribeAll)
{
  MQTTString client_id = create_mqtt_string("client1");

  std::vector<std::string> topics = {"sensor/temperature", "sensor/humidity", "device/+/status"};

  for (const std::string& topic_str : topics) {
    MQTTString topic = create_mqtt_string(topic_str);
    EXPECT_EQ(tree->subscribe(topic, client_id, 1), MQ_SUCCESS);
  }

  size_t subscriber_count;
  EXPECT_EQ(tree->get_total_subscribers(subscriber_count), MQ_SUCCESS);
  EXPECT_EQ(subscriber_count, topics.size());

  int unsubscribed_count = tree->unsubscribe_all(client_id);
  EXPECT_EQ(unsubscribed_count, static_cast<int>(topics.size()));

  EXPECT_EQ(tree->get_total_subscribers(subscriber_count), MQ_SUCCESS);
  EXPECT_EQ(subscriber_count, 0);

  // Verify client has no remaining subscriptions
  std::vector<MQTTString> remaining_subscriptions;
  EXPECT_EQ(tree->get_client_subscriptions(client_id, remaining_subscriptions), MQ_SUCCESS);
  EXPECT_EQ(remaining_subscriptions.size(), 0);
}

// Parameter validation tests
TEST_F(TopicTreeTest, ParameterValidation)
{
  MQTTString empty_topic = create_mqtt_string("");
  MQTTString empty_client = create_mqtt_string("");
  MQTTString valid_topic = create_mqtt_string("sensor/temperature");
  MQTTString valid_client = create_mqtt_string("client1");

  // Empty topic tests
  EXPECT_EQ(tree->subscribe(empty_topic, valid_client), MQ_ERR_PARAM_V2);
  EXPECT_EQ(tree->unsubscribe(empty_topic, valid_client), MQ_ERR_PARAM_V2);

  // Empty client tests
  EXPECT_EQ(tree->subscribe(valid_topic, empty_client), MQ_ERR_PARAM_V2);
  EXPECT_EQ(tree->unsubscribe(valid_topic, empty_client), MQ_ERR_PARAM_V2);

  // Invalid topic filter format tests
  MQTTString invalid_filter1 = create_mqtt_string("sensor/#/temperature");  // # not at end
  MQTTString invalid_filter2 = create_mqtt_string("sensor/temp#");          // # not preceded by /

  EXPECT_EQ(tree->subscribe(invalid_filter1, valid_client), MQ_ERR_PARAM_V2);
  EXPECT_EQ(tree->subscribe(invalid_filter2, valid_client), MQ_ERR_PARAM_V2);
}

// Concurrent tests
TEST_F(TopicTreePerfTest, ConcurrentSubscriptions)
{
  const int num_threads = 10;
  const int subscriptions_per_thread = 100;

  std::vector<std::thread> threads;
  std::atomic<int> success_count(0);
  std::atomic<int> error_count(0);

  auto start_time = std::chrono::high_resolution_clock::now();

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < subscriptions_per_thread; ++i) {
        std::string topic_str = "sensor/thread" + std::to_string(t) + "/item" + std::to_string(i);
        std::string client_str = "client_t" + std::to_string(t) + "_i" + std::to_string(i);

        MQTTString topic = create_mqtt_string(topic_str);
        MQTTString client_id = create_mqtt_string(client_str);

        if (tree->subscribe(topic, client_id, 1) == MQ_SUCCESS) {
          success_count.fetch_add(1);
        } else {
          error_count.fetch_add(1);
        }
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  EXPECT_EQ(success_count.load(), num_threads * subscriptions_per_thread);
  EXPECT_EQ(error_count.load(), 0);

  size_t subscriber_count;
  EXPECT_EQ(tree->get_total_subscribers(subscriber_count), MQ_SUCCESS);
  EXPECT_EQ(subscriber_count, static_cast<size_t>(num_threads * subscriptions_per_thread));

  std::cout << "  Concurrent subscriptions: " << success_count.load() << " topics in "
            << duration.count() << " ms" << std::endl;
  std::cout << "  Subscription rate: " << (success_count.load() * 1000 / duration.count())
            << " ops/sec" << std::endl;
}

TEST_F(TopicTreePerfTest, ConcurrentFindSubscribers)
{
  const int num_clients = 100;

  // Create some subscriptions first
  for (int i = 0; i < num_clients; ++i) {
    std::string client_str = "client" + std::to_string(i);
    MQTTString client_id = create_mqtt_string(client_str);

    MQTTString topic1 = create_mqtt_string("sensor/+");
    MQTTString topic2 = create_mqtt_string("device/#");

    EXPECT_EQ(tree->subscribe(topic1, client_id, 1), MQ_SUCCESS);
    EXPECT_EQ(tree->subscribe(topic2, client_id, 2), MQ_SUCCESS);
  }

  const int num_threads = 8;
  const int searches_per_thread = 1000;

  std::vector<std::thread> threads;
  std::atomic<int> total_found(0);

  auto start_time = std::chrono::high_resolution_clock::now();

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < searches_per_thread; ++i) {
        MQTTString search_topic = create_mqtt_string("sensor/temperature");
        TopicMatchResult result(tree_allocator);
        EXPECT_EQ(tree->find_subscribers(search_topic, result), MQ_SUCCESS);
        total_found.fetch_add(static_cast<int>(result.total_count));

        // Verify result consistency
        EXPECT_EQ(result.total_count, static_cast<size_t>(num_clients));
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  EXPECT_EQ(total_found.load(), num_threads * searches_per_thread * num_clients);

  int total_searches = num_threads * searches_per_thread;
  std::cout << "  Concurrent searches: " << total_searches << " in " << duration.count() << " ms"
            << std::endl;
  std::cout << "  Search rate: " << (total_searches * 1000 / duration.count()) << " ops/sec"
            << std::endl;
}

// Performance benchmark test
TEST_F(TopicTreePerfTest, PerformanceBenchmark)
{
  const int num_subscriptions = 10000;
  const int num_searches = 1000;

  // Create large number of subscriptions
  auto start_subscribe = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < num_subscriptions; ++i) {
    std::string topic_str =
        "sensor/device" + std::to_string(i % 100) + "/data" + std::to_string(i % 10);
    std::string client_str = "client" + std::to_string(i);

    MQTTString topic = create_mqtt_string(topic_str);
    MQTTString client_id = create_mqtt_string(client_str);

    EXPECT_EQ(tree->subscribe(topic, client_id, 1), MQ_SUCCESS);
  }

  auto end_subscribe = std::chrono::high_resolution_clock::now();
  auto subscribe_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_subscribe - start_subscribe);

  std::cout << "  Subscribed " << num_subscriptions << " topics in " << subscribe_duration.count()
            << " ms" << std::endl;
  std::cout << "  Subscription rate: " << (num_subscriptions * 1000 / subscribe_duration.count())
            << " ops/sec" << std::endl;

  // Perform searches
  auto start_search = std::chrono::high_resolution_clock::now();

  int total_matches = 0;
  for (int i = 0; i < num_searches; ++i) {
    std::string search_topic_str =
        "sensor/device" + std::to_string(i % 100) + "/data" + std::to_string(i % 10);
    MQTTString search_topic = create_mqtt_string(search_topic_str);

    TopicMatchResult result(tree_allocator);
    EXPECT_EQ(tree->find_subscribers(search_topic, result), MQ_SUCCESS);
    total_matches += static_cast<int>(result.total_count);
  }

  auto end_search = std::chrono::high_resolution_clock::now();
  auto search_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_search - start_search);

  std::cout << "  Executed " << num_searches << " searches in " << search_duration.count() << " ms"
            << std::endl;
  std::cout << "  Search rate: " << (num_searches * 1000 / search_duration.count()) << " ops/sec"
            << std::endl;
  std::cout << "  Total matches: " << total_matches << std::endl;

  size_t subscriber_count, node_count;
  EXPECT_EQ(tree->get_total_subscribers(subscriber_count), MQ_SUCCESS);
  EXPECT_EQ(tree->get_total_nodes(node_count), MQ_SUCCESS);

  std::cout << "  Final stats - Subscribers: " << subscriber_count << ", Nodes: " << node_count
            << std::endl;
}

// Google Test main function
int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}