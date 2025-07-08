#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include "src/mqtt_allocator.h"
#include "src/mqtt_parser.h"

void test_parse_connect()
{
  std::cout << "测试CONNECT包解析..." << std::endl;

  // CONNECT包的十六进制数据
  uint8_t connect_data[] = {0x10, 0x2c, 0x00, 0x04, 0x4d, 0x51, 0x54, 0x54, 0x05, 0xc2, 0x00, 0x3c,
                            0x05, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x6d, 0x71, 0x74, 0x74,
                            0x78, 0x5f, 0x38, 0x30, 0x36, 0x39, 0x37, 0x31, 0x30, 0x65, 0x00, 0x04,
                            0x74, 0x65, 0x73, 0x74, 0x00, 0x04, 0x74, 0x65, 0x73, 0x74};

  MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::MQTTParser parser(&allocator);

  mqtt::Packet* packet = nullptr;
  int ret = parser.parse_packet(connect_data, sizeof(connect_data), &packet);

  assert(ret == 0);
  assert(packet != nullptr);
  assert(packet->type == mqtt::PacketType::CONNECT);

  mqtt::ConnectPacket* connect = static_cast<mqtt::ConnectPacket*>(packet);
  assert(connect->protocol_name == "MQTT");
  assert(connect->protocol_version == 5);
  assert(connect->keep_alive == 60);
  assert(connect->client_id == "mqttx_8069710e");
  assert(connect->flags.username_flag);
  assert(connect->flags.password_flag);
  assert(connect->username == "test");
  assert(connect->password == "test");

  std::cout << "CONNECT包解析测试通过" << std::endl;
}

void test_parse_publish()
{
  std::cout << "\n测试PUBLISH包解析..." << std::endl;

  // PUBLISH包的十六进制数据 (QoS 1, no properties)
  uint8_t publish_data[] = {0x32, 0x14, 0x00, 0x0a, 0x74, 0x65, 0x73, 0x74, 0x2f, 0x74,
                            0x6f, 0x70, 0x69, 0x63, 0x00, 0x01, 0x00, 0x48, 0x65, 0x6c, 
                            0x6c, 0x6f};
  

  MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::MQTTParser parser(&allocator);

  mqtt::Packet* packet = nullptr;
  int ret = parser.parse_packet(publish_data, sizeof(publish_data), &packet);

  assert(ret == 0);
  assert(packet != nullptr);
  assert(packet->type == mqtt::PacketType::PUBLISH);

  mqtt::PublishPacket* publish = static_cast<mqtt::PublishPacket*>(packet);
  assert(publish->topic_name == "test/topic");
  assert(publish->qos == 1);
  assert(publish->packet_id == 1);

  // 将payload转换为字符串进行比较
  std::string payload_str(publish->payload.begin(), publish->payload.end());
  assert(payload_str == "Hello");

  std::cout << "PUBLISH包解析测试通过" << std::endl;
}

void test_parse_subscribe()
{
  std::cout << "\n测试SUBSCRIBE包解析..." << std::endl;

  // SUBSCRIBE包的十六进制数据 (fixed packet format)
  // 0x82 = SUBSCRIBE with required flags, 0x10 = remaining length
  // 0x00, 0x01 = packet ID, 0x00 = properties length
  // 0x00, 0x0a = topic length, "test/topic" = topic, 0x01 = QoS
  uint8_t subscribe_data[] = {0x82, 0x10, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x74, 0x65, 0x73, 0x74, 
                              0x2f, 0x74, 0x6f, 0x70, 0x69, 0x63, 0x01};

  MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::MQTTParser parser(&allocator);

  mqtt::Packet* packet = nullptr;
  int ret = parser.parse_packet(subscribe_data, sizeof(subscribe_data), &packet);

  assert(ret == 0);
  assert(packet != nullptr);
  assert(packet->type == mqtt::PacketType::SUBSCRIBE);

  mqtt::SubscribePacket* subscribe = static_cast<mqtt::SubscribePacket*>(packet);
  assert(subscribe->packet_id == 1);
  assert(subscribe->subscriptions.size() == 1);
  assert(subscribe->subscriptions[0].first == "test/topic");
  assert(subscribe->subscriptions[0].second == 1);

  std::cout << "SUBSCRIBE包解析测试通过" << std::endl;
}

int main()
{
  std::cout << "开始MQTT解析器测试\n" << std::endl;

  try {
    test_parse_connect();
    test_parse_publish();
    test_parse_subscribe();

    std::cout << "\n所有测试通过！" << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "\n测试失败: " << e.what() << std::endl;
    return 1;
  }
}