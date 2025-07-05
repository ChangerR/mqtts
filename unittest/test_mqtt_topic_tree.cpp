#include <iostream>
#include <cassert>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <future>
#include <atomic>
#include <set>
#include <string>
#include "src/mqtt_topic_tree.h"
#include "src/mqtt_define.h"
#include "src/mqtt_allocator.h"

using namespace mqtt;

// 创建测试用的分配器
MQTTAllocator* create_test_allocator() {
    static MQTTAllocator allocator("test_topic_tree", MQTTMemoryTag::MEM_TAG_TOPIC_TREE, 0);
    return &allocator;
}

// 辅助函数：创建MQTTString
MQTTString create_mqtt_string(const std::string& str) {
    return to_mqtt_string(str, nullptr);
}

// 辅助函数：创建测试用的主题树
ConcurrentTopicTree* create_test_tree(const std::string& test_name) {
    MQTTAllocator* root_allocator = MQTTMemoryManager::get_instance().get_root_allocator();
    MQTTAllocator* tree_allocator = root_allocator->create_child(test_name, MQTTMemoryTag::MEM_TAG_TOPIC_TREE, 0);
    return new ConcurrentTopicTree(tree_allocator);
}

// 基础订阅测试
void test_basic_subscribe() {
    std::cout << "测试基本订阅功能..." << std::endl;
    
    std::unique_ptr<ConcurrentTopicTree> tree(create_test_tree("test_basic_subscribe"));
    MQTTString topic = create_mqtt_string("sensor/temperature");
    MQTTString client_id = create_mqtt_string("client1");
    
    int result = tree->subscribe(topic, client_id, 1);
    assert(result == MQ_SUCCESS);
    assert(tree->get_total_subscribers() == 1);
    assert(tree->get_total_nodes() > 1);
    
    std::cout << "基本订阅测试通过" << std::endl;
}

// 基础取消订阅测试
void test_basic_unsubscribe() {
    std::cout << "\n测试基本取消订阅功能..." << std::endl;
    
    std::unique_ptr<ConcurrentTopicTree> tree(create_test_tree("test_basic_unsubscribe"));
    MQTTString topic = create_mqtt_string("sensor/temperature");
    MQTTString client_id = create_mqtt_string("client1");
    
    // 先订阅
    assert(tree->subscribe(topic, client_id, 1) == MQ_SUCCESS);
    assert(tree->get_total_subscribers() == 1);
    
    // 再取消订阅
    assert(tree->unsubscribe(topic, client_id) == MQ_SUCCESS);
    assert(tree->get_total_subscribers() == 0);
    
    std::cout << "基本取消订阅测试通过" << std::endl;
}

// 精确主题匹配测试
void test_exact_topic_match() {
    std::cout << "\n测试精确主题匹配..." << std::endl;
    
    std::unique_ptr<ConcurrentTopicTree> tree(create_test_tree("test_exact_topic_match"));
    MQTTString topic_filter = create_mqtt_string("sensor/temperature");
    MQTTString client_id = create_mqtt_string("client1");
    
    tree->subscribe(topic_filter, client_id, 1);
    
    // 精确匹配
    MQTTString publish_topic = create_mqtt_string("sensor/temperature");
    TopicMatchResult result = tree->find_subscribers(publish_topic);
    
    assert(result.total_count == 1);
    assert(result.subscribers.size() == 1);
    assert(from_mqtt_string(result.subscribers[0].client_id) == "client1");
    assert(result.subscribers[0].qos == 1);
    
    std::cout << "精确主题匹配测试通过" << std::endl;
}

// 主题不匹配测试
void test_topic_no_match() {
    std::cout << "\n测试主题不匹配..." << std::endl;
    
    ConcurrentTopicTree tree(create_test_allocator());
    MQTTString topic_filter = create_mqtt_string("sensor/temperature");
    MQTTString client_id = create_mqtt_string("client1");
    
    tree.subscribe(topic_filter, client_id, 1);
    
    // 不匹配的主题
    MQTTString publish_topic = create_mqtt_string("sensor/humidity");
    TopicMatchResult result = tree.find_subscribers(publish_topic);
    
    assert(result.total_count == 0);
    assert(result.subscribers.size() == 0);
    
    std::cout << "主题不匹配测试通过" << std::endl;
}

// 单级通配符测试
void test_single_level_wildcard() {
    std::cout << "\n测试单级通配符 (+)..." << std::endl;
    
    std::unique_ptr<ConcurrentTopicTree> tree(create_test_tree("test_single_level_wildcard"));
    MQTTString topic_filter = create_mqtt_string("sensor/+/data");
    MQTTString client_id = create_mqtt_string("client1");
    
    tree->subscribe(topic_filter, client_id, 1);
    
    // 应该匹配的主题
    std::vector<std::string> matching_topics = {
        "sensor/temperature/data",
        "sensor/humidity/data",
        "sensor/pressure/data"
    };
    
    for (const std::string& topic_str : matching_topics) {
        MQTTString topic = create_mqtt_string(topic_str);
        TopicMatchResult result = tree->find_subscribers(topic);
        assert(result.total_count == 1);
        std::cout << "  匹配主题: " << topic_str << " ✓" << std::endl;
    }
    
    // 不应该匹配的主题
    std::vector<std::string> non_matching_topics = {
        "sensor/temperature",
        "sensor/temperature/data/extra",
        "device/temperature/data"
    };
    
    for (const std::string& topic_str : non_matching_topics) {
        MQTTString topic = create_mqtt_string(topic_str);
        TopicMatchResult result = tree->find_subscribers(topic);
        assert(result.total_count == 0);
        std::cout << "  不匹配主题: " << topic_str << " ✓" << std::endl;
    }
    
    std::cout << "单级通配符测试通过" << std::endl;
}

// 多级通配符测试
void test_multi_level_wildcard() {
    std::cout << "\n测试多级通配符 (#)..." << std::endl;
    
    ConcurrentTopicTree tree(create_test_allocator());
    MQTTString topic_filter = create_mqtt_string("sensor/#");
    MQTTString client_id = create_mqtt_string("client1");
    
    tree.subscribe(topic_filter, client_id, 1);
    
    // 应该匹配的主题
    std::vector<std::string> matching_topics = {
        "sensor/temperature",
        "sensor/temperature/data",
        "sensor/humidity/room1/data",
        "sensor/pressure/device1/room1/data"
    };
    
    for (const std::string& topic_str : matching_topics) {
        MQTTString topic = create_mqtt_string(topic_str);
        TopicMatchResult result = tree.find_subscribers(topic);
        assert(result.total_count == 1);
        std::cout << "  匹配主题: " << topic_str << " ✓" << std::endl;
    }
    
    // 不应该匹配的主题
    std::vector<std::string> non_matching_topics = {
        "device/temperature",
        "sensors/temperature"
    };
    
    for (const std::string& topic_str : non_matching_topics) {
        MQTTString topic = create_mqtt_string(topic_str);
        TopicMatchResult result = tree.find_subscribers(topic);
        assert(result.total_count == 0);
        std::cout << "  不匹配主题: " << topic_str << " ✓" << std::endl;
    }
    
    std::cout << "多级通配符测试通过" << std::endl;
}

// 根级别多级通配符测试
void test_root_level_multi_wildcard() {
    std::cout << "\n测试根级别多级通配符..." << std::endl;
    
    ConcurrentTopicTree tree(create_test_allocator());
    MQTTString topic_filter = create_mqtt_string("#");
    MQTTString client_id = create_mqtt_string("client1");
    
    tree.subscribe(topic_filter, client_id, 1);
    
    // 应该匹配所有主题
    std::vector<std::string> matching_topics = {
        "sensor",
        "sensor/temperature",
        "device/control/power",
        "a/b/c/d/e/f/g"
    };
    
    for (const std::string& topic_str : matching_topics) {
        MQTTString topic = create_mqtt_string(topic_str);
        TopicMatchResult result = tree.find_subscribers(topic);
        assert(result.total_count == 1);
        std::cout << "  匹配主题: " << topic_str << " ✓" << std::endl;
    }
    
    std::cout << "根级别多级通配符测试通过" << std::endl;
}

// 多客户端订阅相同主题测试
void test_multiple_subscribers_to_same_topic() {
    std::cout << "\n测试多客户端订阅相同主题..." << std::endl;
    
    ConcurrentTopicTree tree(create_test_allocator());
    MQTTString topic = create_mqtt_string("sensor/temperature");
    
    std::vector<std::string> client_ids = {"client1", "client2", "client3"};
    
    for (size_t i = 0; i < client_ids.size(); ++i) {
        MQTTString client_id = create_mqtt_string(client_ids[i]);
        uint8_t qos = static_cast<uint8_t>(i);
        tree.subscribe(topic, client_id, qos);
    }
    
    assert(tree.get_total_subscribers() == 3);
    
    TopicMatchResult result = tree.find_subscribers(topic);
    assert(result.total_count == 3);
    assert(result.subscribers.size() == 3);
    
    // 验证所有客户端都被找到
    std::set<std::string> found_clients;
    for (const auto& subscriber : result.subscribers) {
        found_clients.insert(from_mqtt_string(subscriber.client_id));
    }
    
    for (const std::string& expected_client : client_ids) {
        assert(found_clients.find(expected_client) != found_clients.end());
        std::cout << "  找到客户端: " << expected_client << " ✓" << std::endl;
    }
    
    std::cout << "多客户端订阅测试通过" << std::endl;
}

// QoS更新测试
void test_qos_update() {
    std::cout << "\n测试QoS更新..." << std::endl;
    
    ConcurrentTopicTree tree(create_test_allocator());
    MQTTString topic = create_mqtt_string("sensor/temperature");
    MQTTString client_id = create_mqtt_string("client1");
    
    // 初始订阅 QoS 0
    tree.subscribe(topic, client_id, 0);
    
    TopicMatchResult result1 = tree.find_subscribers(topic);
    assert(result1.total_count == 1);
    assert(result1.subscribers[0].qos == 0);
    std::cout << "  初始QoS: " << static_cast<int>(result1.subscribers[0].qos) << " ✓" << std::endl;
    
    // 更新为 QoS 2
    tree.subscribe(topic, client_id, 2);
    
    TopicMatchResult result2 = tree.find_subscribers(topic);
    assert(result2.total_count == 1);  // 仍然只有一个订阅者
    assert(result2.subscribers[0].qos == 2);  // QoS已更新
    std::cout << "  更新后QoS: " << static_cast<int>(result2.subscribers[0].qos) << " ✓" << std::endl;
    
    std::cout << "QoS更新测试通过" << std::endl;
}

// 获取客户端订阅列表测试
void test_get_client_subscriptions() {
    std::cout << "\n测试获取客户端订阅列表..." << std::endl;
    
    ConcurrentTopicTree tree(create_test_allocator());
    MQTTString client_id = create_mqtt_string("client1");
    
    std::vector<std::string> topics = {
        "sensor/temperature",
        "sensor/humidity", 
        "device/+/status",
        "logs/#"
    };
    
    for (const std::string& topic_str : topics) {
        MQTTString topic = create_mqtt_string(topic_str);
        tree.subscribe(topic, client_id, 1);
    }
    
    std::vector<MQTTString> subscriptions = tree.get_client_subscriptions(client_id);
    assert(subscriptions.size() == topics.size());
    
    std::set<std::string> found_topics;
    for (const MQTTString& subscription : subscriptions) {
        found_topics.insert(from_mqtt_string(subscription));
    }
    
    for (const std::string& expected_topic : topics) {
        assert(found_topics.find(expected_topic) != found_topics.end());
        std::cout << "  找到订阅: " << expected_topic << " ✓" << std::endl;
    }
    
    std::cout << "获取客户端订阅列表测试通过" << std::endl;
}

// 取消所有订阅测试
void test_unsubscribe_all() {
    std::cout << "\n测试取消所有订阅..." << std::endl;
    
    ConcurrentTopicTree tree(create_test_allocator());
    MQTTString client_id = create_mqtt_string("client1");
    
    std::vector<std::string> topics = {
        "sensor/temperature",
        "sensor/humidity", 
        "device/+/status"
    };
    
    for (const std::string& topic_str : topics) {
        MQTTString topic = create_mqtt_string(topic_str);
        tree.subscribe(topic, client_id, 1);
    }
    
    assert(tree.get_total_subscribers() == topics.size());
    std::cout << "  订阅了 " << topics.size() << " 个主题" << std::endl;
    
    int unsubscribed_count = tree.unsubscribe_all(client_id);
    assert(unsubscribed_count == static_cast<int>(topics.size()));
    assert(tree.get_total_subscribers() == 0);
    std::cout << "  取消订阅了 " << unsubscribed_count << " 个主题" << std::endl;
    
    // 验证客户端没有剩余订阅
    std::vector<MQTTString> remaining_subscriptions = tree.get_client_subscriptions(client_id);
    assert(remaining_subscriptions.size() == 0);
    
    std::cout << "取消所有订阅测试通过" << std::endl;
}

// 参数验证测试
void test_parameter_validation() {
    std::cout << "\n测试参数验证..." << std::endl;
    
    ConcurrentTopicTree tree(create_test_allocator());
    MQTTString empty_topic = create_mqtt_string("");
    MQTTString empty_client = create_mqtt_string("");
    MQTTString valid_topic = create_mqtt_string("sensor/temperature");
    MQTTString valid_client = create_mqtt_string("client1");
    
    // 空主题测试
    assert(tree.subscribe(empty_topic, valid_client) == MQ_ERR_PARAM_V2);
    assert(tree.unsubscribe(empty_topic, valid_client) == MQ_ERR_PARAM_V2);
    std::cout << "  空主题验证通过 ✓" << std::endl;
    
    // 空客户端测试
    assert(tree.subscribe(valid_topic, empty_client) == MQ_ERR_PARAM_V2);
    assert(tree.unsubscribe(valid_topic, empty_client) == MQ_ERR_PARAM_V2);
    std::cout << "  空客户端验证通过 ✓" << std::endl;
    
    // 无效主题过滤器格式测试
    MQTTString invalid_filter1 = create_mqtt_string("sensor/#/temperature");  // # 不在末尾
    MQTTString invalid_filter2 = create_mqtt_string("sensor/temp#");  // # 前面不是 /
    
    assert(tree.subscribe(invalid_filter1, valid_client) == MQ_ERR_PARAM_V2);
    assert(tree.subscribe(invalid_filter2, valid_client) == MQ_ERR_PARAM_V2);
    std::cout << "  无效过滤器格式验证通过 ✓" << std::endl;
    
    std::cout << "参数验证测试通过" << std::endl;
}

// 并发订阅测试
void test_concurrent_subscriptions() {
    std::cout << "\n测试并发订阅..." << std::endl;
    
    ConcurrentTopicTree tree(create_test_allocator());
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
                
                if (tree.subscribe(topic, client_id, 1) == MQ_SUCCESS) {
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
    
    assert(success_count.load() == num_threads * subscriptions_per_thread);
    assert(error_count.load() == 0);
    assert(tree.get_total_subscribers() == static_cast<size_t>(num_threads * subscriptions_per_thread));
    
    std::cout << "  并发订阅 " << success_count.load() << " 个主题，耗时 " << duration.count() << " ms" << std::endl;
    std::cout << "  订阅速率: " << (success_count.load() * 1000 / duration.count()) << " ops/sec" << std::endl;
    std::cout << "并发订阅测试通过" << std::endl;
}

// 并发查找测试
void test_concurrent_find_subscribers() {
    std::cout << "\n测试并发查找订阅者..." << std::endl;
    
    ConcurrentTopicTree tree(create_test_allocator());
    const int num_clients = 100;
    
    // 先创建一些订阅
    for (int i = 0; i < num_clients; ++i) {
        std::string client_str = "client" + std::to_string(i);
        MQTTString client_id = create_mqtt_string(client_str);
        
        MQTTString topic1 = create_mqtt_string("sensor/+");
        MQTTString topic2 = create_mqtt_string("device/#");
        
        tree.subscribe(topic1, client_id, 1);
        tree.subscribe(topic2, client_id, 2);
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
                TopicMatchResult result = tree.find_subscribers(search_topic);
                total_found.fetch_add(static_cast<int>(result.total_count));
                
                // 验证结果一致性
                assert(result.total_count == static_cast<size_t>(num_clients));
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    assert(total_found.load() == num_threads * searches_per_thread * num_clients);
    
    int total_searches = num_threads * searches_per_thread;
    std::cout << "  并发执行 " << total_searches << " 次查找，耗时 " << duration.count() << " ms" << std::endl;
    std::cout << "  查找速率: " << (total_searches * 1000 / duration.count()) << " ops/sec" << std::endl;
    std::cout << "并发查找测试通过" << std::endl;
}

// 性能基准测试
void test_performance_benchmark() {
    std::cout << "\n执行性能基准测试..." << std::endl;
    
    ConcurrentTopicTree tree(create_test_allocator());
    const int num_subscriptions = 10000;
    const int num_searches = 1000;
    
    // 创建大量订阅
    auto start_subscribe = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < num_subscriptions; ++i) {
        std::string topic_str = "sensor/device" + std::to_string(i % 100) + "/data" + std::to_string(i % 10);
        std::string client_str = "client" + std::to_string(i);
        
        MQTTString topic = create_mqtt_string(topic_str);
        MQTTString client_id = create_mqtt_string(client_str);
        
        tree.subscribe(topic, client_id, 1);
    }
    
    auto end_subscribe = std::chrono::high_resolution_clock::now();
    auto subscribe_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_subscribe - start_subscribe);
    
    std::cout << "  订阅 " << num_subscriptions << " 个主题，耗时 " << subscribe_duration.count() << " ms" << std::endl;
    std::cout << "  订阅速率: " << (num_subscriptions * 1000 / subscribe_duration.count()) << " ops/sec" << std::endl;
    
    // 执行搜索
    auto start_search = std::chrono::high_resolution_clock::now();
    
    int total_matches = 0;
    for (int i = 0; i < num_searches; ++i) {
        std::string search_topic_str = "sensor/device" + std::to_string(i % 100) + "/data" + std::to_string(i % 10);
        MQTTString search_topic = create_mqtt_string(search_topic_str);
        
        TopicMatchResult result = tree.find_subscribers(search_topic);
        total_matches += static_cast<int>(result.total_count);
    }
    
    auto end_search = std::chrono::high_resolution_clock::now();
    auto search_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_search - start_search);
    
    std::cout << "  执行 " << num_searches << " 次搜索，耗时 " << search_duration.count() << " ms" << std::endl;
    std::cout << "  搜索速率: " << (num_searches * 1000 / search_duration.count()) << " ops/sec" << std::endl;
    std::cout << "  总匹配数: " << total_matches << std::endl;
    
    std::cout << "  最终统计 - 订阅者: " << tree.get_total_subscribers() 
              << ", 节点数: " << tree.get_total_nodes() << std::endl;
    
    std::cout << "性能基准测试完成" << std::endl;
}

int main() {
    std::cout << "开始MQTT主题匹配树测试\n" << std::endl;
    
    try {
        test_basic_subscribe();
        test_basic_unsubscribe();
        test_exact_topic_match();
        test_topic_no_match();
        test_single_level_wildcard();
        test_multi_level_wildcard();
        test_root_level_multi_wildcard();
        test_multiple_subscribers_to_same_topic();
        test_qos_update();
        test_get_client_subscriptions();
        test_unsubscribe_all();
        test_parameter_validation();
        test_concurrent_subscriptions();
        test_concurrent_find_subscribers();
        test_performance_benchmark();
        
        std::cout << "\n🎉 所有测试通过！" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ 测试失败: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ 未知错误导致测试失败" << std::endl;
        return 1;
    }
}