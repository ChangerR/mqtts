#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include "src/mqtt_allocator.h"
#include "src/mqtt_define.h"
#include "src/mqtt_packet.h"
#include "src/mqtt_parser.h"
#include "src/mqtt_serialize_buffer.h"

using namespace mqtt;

// Comprehensive MQTT v5 Serialization Test Class
class MQTTv5SerializationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Initialize memory allocator directly
        test_allocator = new MQTTAllocator("mqttv5_serialization_test", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
        ASSERT_NE(test_allocator, nullptr) << "Failed to create allocator";

        // Initialize parser
        parser = std::make_unique<MQTTParser>(test_allocator);
    }

    void TearDown() override
    {
        parser.reset();
        
        if (test_allocator) {
            delete test_allocator;
            test_allocator = nullptr;
        }
    }

    MQTTAllocator* test_allocator = nullptr;
    std::unique_ptr<MQTTParser> parser;
};

// ============= CONNECT Packet Serialization Tests =============

TEST_F(MQTTv5SerializationTest, ConnectPacketSerialization)
{
    // Create CONNECT packet
    ConnectPacket connect(test_allocator);
    connect.type = PacketType::CONNECT;
    connect.protocol_name = "MQTT";
    connect.protocol_version = 5;
    connect.flags.clean_start = true;
    connect.flags.username_flag = true;
    connect.flags.password_flag = true;
    connect.keep_alive = 60;
    connect.client_id = "test_client";
    connect.username = "user";
    connect.password = "pass";
    connect.properties.session_expiry_interval = 120;
    connect.properties.receive_maximum = 100;
    connect.properties.maximum_packet_size = 65536;

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_connect(&connect, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::CONNECT);

    // Verify parsed data
    ConnectPacket* parsed_connect = static_cast<ConnectPacket*>(parsed_packet);
    EXPECT_EQ(parsed_connect->protocol_name, "MQTT");
    EXPECT_EQ(parsed_connect->protocol_version, 5);
    EXPECT_TRUE(parsed_connect->flags.clean_start);
    EXPECT_TRUE(parsed_connect->flags.username_flag);
    EXPECT_TRUE(parsed_connect->flags.password_flag);
    EXPECT_EQ(parsed_connect->keep_alive, 60);
    EXPECT_EQ(parsed_connect->client_id, "test_client");
    EXPECT_EQ(parsed_connect->username, "user");
    EXPECT_EQ(parsed_connect->password, "pass");
    EXPECT_EQ(parsed_connect->properties.session_expiry_interval, 120);
    EXPECT_EQ(parsed_connect->properties.receive_maximum, 100);
    EXPECT_EQ(parsed_connect->properties.maximum_packet_size, 65536);
}

TEST_F(MQTTv5SerializationTest, ConnectPacketWithWillSerialization)
{
    // Create CONNECT packet with will
    ConnectPacket connect(test_allocator);
    connect.type = PacketType::CONNECT;
    connect.protocol_name = "MQTT";
    connect.protocol_version = 5;
    connect.flags.clean_start = true;
    connect.flags.will_flag = true;
    connect.flags.will_qos = 1;
    connect.flags.will_retain = true;
    connect.keep_alive = 60;
    connect.client_id = "test_client";
    connect.will_topic = "will/topic";
    connect.will_payload = "will message";
    connect.will_properties.will_delay_interval = 30;
    connect.will_properties.payload_format_indicator = 1;
    connect.will_properties.message_expiry_interval = 600;

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_connect(&connect, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::CONNECT);

    // Verify parsed data
    ConnectPacket* parsed_connect = static_cast<ConnectPacket*>(parsed_packet);
    EXPECT_TRUE(parsed_connect->flags.will_flag);
    EXPECT_EQ(parsed_connect->flags.will_qos, 1);
    EXPECT_TRUE(parsed_connect->flags.will_retain);
    EXPECT_EQ(parsed_connect->will_topic, "will/topic");
    EXPECT_EQ(parsed_connect->will_payload, "will message");
    EXPECT_EQ(parsed_connect->will_properties.will_delay_interval, 30);
    EXPECT_EQ(parsed_connect->will_properties.payload_format_indicator, 1);
    EXPECT_EQ(parsed_connect->will_properties.message_expiry_interval, 600);
}

// ============= CONNACK Packet Serialization Tests =============

TEST_F(MQTTv5SerializationTest, ConnAckPacketSerialization)
{
    // Create CONNACK packet
    ConnAckPacket connack(test_allocator);
    connack.type = PacketType::CONNACK;
    connack.session_present = true;
    connack.reason_code = ReasonCode::Success;
    connack.properties.session_expiry_interval = 300;
    connack.properties.receive_maximum = 50;
    connack.properties.maximum_qos = 2;
    connack.properties.retain_available = true;
    connack.properties.maximum_packet_size = 32768;
    connack.properties.assigned_client_identifier = "assigned_client_123";
    connack.properties.topic_alias_maximum = 10;
    connack.properties.reason_string = "Connection accepted";
    connack.properties.wildcard_subscription_available = true;
    connack.properties.subscription_identifier_available = true;
    connack.properties.shared_subscription_available = true;
    connack.properties.server_keep_alive = 60;
    connack.properties.response_information = "response_info";
    connack.properties.server_reference = "server_ref";

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_connack(&connack, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::CONNACK);

    // Verify parsed data
    ConnAckPacket* parsed_connack = static_cast<ConnAckPacket*>(parsed_packet);
    EXPECT_TRUE(parsed_connack->session_present);
    EXPECT_EQ(parsed_connack->reason_code, ReasonCode::Success);
    EXPECT_EQ(parsed_connack->properties.session_expiry_interval, 300);
    EXPECT_EQ(parsed_connack->properties.receive_maximum, 50);
    EXPECT_EQ(parsed_connack->properties.maximum_qos, 2);
    EXPECT_TRUE(parsed_connack->properties.retain_available);
    EXPECT_EQ(parsed_connack->properties.maximum_packet_size, 32768);
    EXPECT_EQ(parsed_connack->properties.assigned_client_identifier, "assigned_client_123");
    EXPECT_EQ(parsed_connack->properties.topic_alias_maximum, 10);
    EXPECT_EQ(parsed_connack->properties.reason_string, "Connection accepted");
    EXPECT_TRUE(parsed_connack->properties.wildcard_subscription_available);
    EXPECT_TRUE(parsed_connack->properties.subscription_identifier_available);
    EXPECT_TRUE(parsed_connack->properties.shared_subscription_available);
    EXPECT_EQ(parsed_connack->properties.server_keep_alive, 60);
    EXPECT_EQ(parsed_connack->properties.response_information, "response_info");
    EXPECT_EQ(parsed_connack->properties.server_reference, "server_ref");
}

// ============= PUBLISH Packet Serialization Tests =============

TEST_F(MQTTv5SerializationTest, PublishPacketQoS0Serialization)
{
    // Create PUBLISH packet QoS 0
    PublishPacket publish(test_allocator);
    publish.type = PacketType::PUBLISH;
    publish.qos = 0;
    publish.retain = false;
    publish.dup = false;
    publish.topic_name = "test/topic";
    publish.payload = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
    publish.properties.payload_format_indicator = 1;
    publish.properties.message_expiry_interval = 60;
    publish.properties.content_type = "text/plain";
    publish.properties.response_topic = "response/topic";
    publish.properties.correlation_data = {0x01, 0x02, 0x03, 0x04};

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_publish(&publish, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::PUBLISH);

    // Verify parsed data
    PublishPacket* parsed_publish = static_cast<PublishPacket*>(parsed_packet);
    EXPECT_EQ(parsed_publish->qos, 0);
    EXPECT_FALSE(parsed_publish->retain);
    EXPECT_FALSE(parsed_publish->dup);
    EXPECT_EQ(parsed_publish->topic_name, "test/topic");
    EXPECT_EQ(parsed_publish->payload.size(), 11);
    EXPECT_EQ(std::string(parsed_publish->payload.begin(), parsed_publish->payload.end()), "Hello World");
    EXPECT_EQ(parsed_publish->properties.payload_format_indicator, 1);
    EXPECT_EQ(parsed_publish->properties.message_expiry_interval, 60);
    EXPECT_EQ(parsed_publish->properties.content_type, "text/plain");
    EXPECT_EQ(parsed_publish->properties.response_topic, "response/topic");
    EXPECT_EQ(parsed_publish->properties.correlation_data.size(), 4);
    EXPECT_EQ(parsed_publish->properties.correlation_data[0], 0x01);
    EXPECT_EQ(parsed_publish->properties.correlation_data[3], 0x04);
}

TEST_F(MQTTv5SerializationTest, PublishPacketQoS1Serialization)
{
    // Create PUBLISH packet QoS 1
    PublishPacket publish(test_allocator);
    publish.type = PacketType::PUBLISH;
    publish.qos = 1;
    publish.retain = true;
    publish.dup = false;
    publish.topic_name = "test/topic";
    publish.packet_id = 123;
    publish.payload = {'T', 'e', 's', 't', ' ', 'P', 'a', 'y', 'l', 'o', 'a', 'd'};
    publish.properties.topic_alias = 5;
    publish.properties.subscription_identifier = 10;

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_publish(&publish, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::PUBLISH);

    // Verify parsed data
    PublishPacket* parsed_publish = static_cast<PublishPacket*>(parsed_packet);
    EXPECT_EQ(parsed_publish->qos, 1);
    EXPECT_TRUE(parsed_publish->retain);
    EXPECT_FALSE(parsed_publish->dup);
    EXPECT_EQ(parsed_publish->topic_name, "test/topic");
    EXPECT_EQ(parsed_publish->packet_id, 123);
    EXPECT_EQ(std::string(parsed_publish->payload.begin(), parsed_publish->payload.end()), "Test Payload");
    EXPECT_EQ(parsed_publish->properties.topic_alias, 5);
    EXPECT_EQ(parsed_publish->properties.subscription_identifier, 10);
}

// ============= SUBSCRIBE Packet Serialization Tests =============

TEST_F(MQTTv5SerializationTest, SubscribePacketSerialization)
{
    // Create SUBSCRIBE packet
    SubscribePacket subscribe(test_allocator);
    subscribe.type = PacketType::SUBSCRIBE;
    subscribe.packet_id = 456;
    subscribe.properties.subscription_identifier = 20;
    
    // Add subscriptions
    subscribe.subscriptions.push_back(std::make_pair(MQTTString("topic/filter1", MQTTStrAllocator(test_allocator)), 0));
    subscribe.subscriptions.push_back(std::make_pair(MQTTString("topic/filter2", MQTTStrAllocator(test_allocator)), 1));
    subscribe.subscriptions.push_back(std::make_pair(MQTTString("topic/filter3", MQTTStrAllocator(test_allocator)), 2));

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_subscribe(&subscribe, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::SUBSCRIBE);

    // Verify parsed data
    SubscribePacket* parsed_subscribe = static_cast<SubscribePacket*>(parsed_packet);
    EXPECT_EQ(parsed_subscribe->packet_id, 456);
    EXPECT_EQ(parsed_subscribe->properties.subscription_identifier, 20);
    EXPECT_EQ(parsed_subscribe->subscriptions.size(), 3);
    EXPECT_EQ(parsed_subscribe->subscriptions[0].first, "topic/filter1");
    EXPECT_EQ(parsed_subscribe->subscriptions[0].second, 0);
    EXPECT_EQ(parsed_subscribe->subscriptions[1].first, "topic/filter2");
    EXPECT_EQ(parsed_subscribe->subscriptions[1].second, 1);
    EXPECT_EQ(parsed_subscribe->subscriptions[2].first, "topic/filter3");
    EXPECT_EQ(parsed_subscribe->subscriptions[2].second, 2);
}

// ============= SUBACK Packet Serialization Tests =============

TEST_F(MQTTv5SerializationTest, SubAckPacketSerialization)
{
    // Create SUBACK packet
    SubAckPacket suback(test_allocator);
    suback.type = PacketType::SUBACK;
    suback.packet_id = 456;
    suback.properties.reason_string = "Subscription successful";
    
    // Add reason codes
    suback.reason_codes.push_back(ReasonCode::GrantedQoS0);
    suback.reason_codes.push_back(ReasonCode::GrantedQoS1);
    suback.reason_codes.push_back(ReasonCode::GrantedQoS2);
    suback.reason_codes.push_back(ReasonCode::UnspecifiedError);

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_suback(&suback, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::SUBACK);

    // Verify parsed data
    SubAckPacket* parsed_suback = static_cast<SubAckPacket*>(parsed_packet);
    EXPECT_EQ(parsed_suback->packet_id, 456);
    EXPECT_EQ(parsed_suback->properties.reason_string, "Subscription successful");
    EXPECT_EQ(parsed_suback->reason_codes.size(), 4);
    EXPECT_EQ(parsed_suback->reason_codes[0], ReasonCode::GrantedQoS0);
    EXPECT_EQ(parsed_suback->reason_codes[1], ReasonCode::GrantedQoS1);
    EXPECT_EQ(parsed_suback->reason_codes[2], ReasonCode::GrantedQoS2);
    EXPECT_EQ(parsed_suback->reason_codes[3], ReasonCode::UnspecifiedError);
}

// ============= UNSUBSCRIBE Packet Serialization Tests =============

TEST_F(MQTTv5SerializationTest, UnsubscribePacketSerialization)
{
    // Create UNSUBSCRIBE packet
    UnsubscribePacket unsubscribe(test_allocator);
    unsubscribe.type = PacketType::UNSUBSCRIBE;
    unsubscribe.packet_id = 789;
    
    // Add topic filters
    unsubscribe.topic_filters.push_back(MQTTString("topic/filter1", MQTTStrAllocator(test_allocator)));
    unsubscribe.topic_filters.push_back(MQTTString("topic/filter2", MQTTStrAllocator(test_allocator)));
    unsubscribe.topic_filters.push_back(MQTTString("topic/filter3", MQTTStrAllocator(test_allocator)));

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_unsubscribe(&unsubscribe, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::UNSUBSCRIBE);

    // Verify parsed data
    UnsubscribePacket* parsed_unsubscribe = static_cast<UnsubscribePacket*>(parsed_packet);
    EXPECT_EQ(parsed_unsubscribe->packet_id, 789);
    EXPECT_EQ(parsed_unsubscribe->topic_filters.size(), 3);
    EXPECT_EQ(parsed_unsubscribe->topic_filters[0], "topic/filter1");
    EXPECT_EQ(parsed_unsubscribe->topic_filters[1], "topic/filter2");
    EXPECT_EQ(parsed_unsubscribe->topic_filters[2], "topic/filter3");
}

// ============= UNSUBACK Packet Serialization Tests =============

TEST_F(MQTTv5SerializationTest, UnsubAckPacketSerialization)
{
    // Create UNSUBACK packet
    UnsubAckPacket unsuback(test_allocator);
    unsuback.type = PacketType::UNSUBACK;
    unsuback.packet_id = 789;
    unsuback.properties.reason_string = "Unsubscription successful";
    
    // Add reason codes
    unsuback.reason_codes.push_back(ReasonCode::Success);
    unsuback.reason_codes.push_back(ReasonCode::NoSubscriptionExisted);
    unsuback.reason_codes.push_back(ReasonCode::UnspecifiedError);

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_unsuback(&unsuback, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::UNSUBACK);

    // Verify parsed data
    UnsubAckPacket* parsed_unsuback = static_cast<UnsubAckPacket*>(parsed_packet);
    EXPECT_EQ(parsed_unsuback->packet_id, 789);
    EXPECT_EQ(parsed_unsuback->properties.reason_string, "Unsubscription successful");
    EXPECT_EQ(parsed_unsuback->reason_codes.size(), 3);
    EXPECT_EQ(parsed_unsuback->reason_codes[0], ReasonCode::Success);
    EXPECT_EQ(parsed_unsuback->reason_codes[1], ReasonCode::NoSubscriptionExisted);
    EXPECT_EQ(parsed_unsuback->reason_codes[2], ReasonCode::UnspecifiedError);
}

// ============= QoS Response Packet Serialization Tests =============

TEST_F(MQTTv5SerializationTest, PubAckPacketSerialization)
{
    // Create PUBACK packet
    PubAckPacket puback(test_allocator);
    puback.type = PacketType::PUBACK;
    puback.packet_id = 123;
    puback.reason_code = ReasonCode::Success;
    puback.properties.reason_string = "Publication acknowledged";

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_puback(&puback, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::PUBACK);

    // Verify parsed data
    PubAckPacket* parsed_puback = static_cast<PubAckPacket*>(parsed_packet);
    EXPECT_EQ(parsed_puback->packet_id, 123);
    EXPECT_EQ(parsed_puback->reason_code, ReasonCode::Success);
    EXPECT_EQ(parsed_puback->properties.reason_string, "Publication acknowledged");
}

// ============= DISCONNECT Packet Serialization Tests =============

TEST_F(MQTTv5SerializationTest, DisconnectPacketSerialization)
{
    // Create DISCONNECT packet
    DisconnectPacket disconnect(test_allocator);
    disconnect.type = PacketType::DISCONNECT;
    disconnect.reason_code = ReasonCode::NormalDisconnection;
    disconnect.properties.session_expiry_interval = 0;
    disconnect.properties.reason_string = "Normal disconnection";
    disconnect.properties.server_reference = "other.server.com";

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_disconnect(&disconnect, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::DISCONNECT);

    // Verify parsed data
    DisconnectPacket* parsed_disconnect = static_cast<DisconnectPacket*>(parsed_packet);
    EXPECT_EQ(parsed_disconnect->reason_code, ReasonCode::NormalDisconnection);
    EXPECT_EQ(parsed_disconnect->properties.session_expiry_interval, 0);
    EXPECT_EQ(parsed_disconnect->properties.reason_string, "Normal disconnection");
    EXPECT_EQ(parsed_disconnect->properties.server_reference, "other.server.com");
}

// ============= AUTH Packet Serialization Tests =============

TEST_F(MQTTv5SerializationTest, AuthPacketSerialization)
{
    // Create AUTH packet
    AuthPacket auth(test_allocator);
    auth.type = PacketType::AUTH;
    auth.reason_code = ReasonCode::ContinueAuthentication;
    auth.properties.authentication_method = "SCRAM-SHA-256";
    auth.properties.authentication_data = {0x01, 0x02, 0x03, 0x04, 0x05};
    auth.properties.reason_string = "Continue authentication";

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_auth(&auth, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::AUTH);

    // Verify parsed data
    AuthPacket* parsed_auth = static_cast<AuthPacket*>(parsed_packet);
    EXPECT_EQ(parsed_auth->reason_code, ReasonCode::ContinueAuthentication);
    EXPECT_EQ(parsed_auth->properties.authentication_method, "SCRAM-SHA-256");
    EXPECT_EQ(parsed_auth->properties.authentication_data.size(), 5);
    EXPECT_EQ(parsed_auth->properties.authentication_data[0], 0x01);
    EXPECT_EQ(parsed_auth->properties.authentication_data[4], 0x05);
    EXPECT_EQ(parsed_auth->properties.reason_string, "Continue authentication");
}

// ============= PING Packet Serialization Tests =============

TEST_F(MQTTv5SerializationTest, PingReqPacketSerialization)
{
    // Create PINGREQ packet
    PingReqPacket pingreq(test_allocator);
    pingreq.type = PacketType::PINGREQ;

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_pingreq(&pingreq, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::PINGREQ);
}

TEST_F(MQTTv5SerializationTest, PingRespPacketSerialization)
{
    // Create PINGRESP packet
    PingRespPacket pingresp(test_allocator);
    pingresp.type = PacketType::PINGRESP;

    // Serialize packet
    MQTTSerializeBuffer buffer(test_allocator);
    int ret = parser->serialize_pingresp(&pingresp, buffer);
    ASSERT_EQ(ret, MQ_SUCCESS);

    // Parse serialized data back
    Packet* parsed_packet = nullptr;
    ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(parsed_packet, nullptr);
    ASSERT_EQ(parsed_packet->type, PacketType::PINGRESP);
}

// ============= Round-trip Tests =============

TEST_F(MQTTv5SerializationTest, RoundTripAllPacketTypes)
{
    // Test all packet types in a round-trip scenario
    std::vector<PacketType> packet_types = {
        PacketType::CONNECT, PacketType::CONNACK, PacketType::PUBLISH, PacketType::PUBACK,
        PacketType::PUBREC, PacketType::PUBREL, PacketType::PUBCOMP, PacketType::SUBSCRIBE,
        PacketType::SUBACK, PacketType::UNSUBSCRIBE, PacketType::UNSUBACK, PacketType::PINGREQ,
        PacketType::PINGRESP, PacketType::DISCONNECT, PacketType::AUTH
    };

    for (const auto& type : packet_types) {
        // Create a basic packet of each type
        std::unique_ptr<Packet> packet;
        
        switch (type) {
            case PacketType::CONNECT: {
                auto connect = std::make_unique<ConnectPacket>(test_allocator);
                connect->type = type;
                connect->protocol_name = "MQTT";
                connect->protocol_version = 5;
                connect->client_id = "test";
                packet = std::move(connect);
                break;
            }
            case PacketType::CONNACK: {
                auto connack = std::make_unique<ConnAckPacket>(test_allocator);
                connack->type = type;
                connack->reason_code = ReasonCode::Success;
                packet = std::move(connack);
                break;
            }
            case PacketType::PUBLISH: {
                auto publish = std::make_unique<PublishPacket>(test_allocator);
                publish->type = type;
                publish->topic_name = "test";
                publish->qos = 0;
                packet = std::move(publish);
                break;
            }
            case PacketType::PUBACK: {
                auto puback = std::make_unique<PubAckPacket>(test_allocator);
                puback->type = type;
                puback->packet_id = 1;
                puback->reason_code = ReasonCode::Success;
                packet = std::move(puback);
                break;
            }
            case PacketType::PUBREC: {
                auto pubrec = std::make_unique<PubRecPacket>(test_allocator);
                pubrec->type = type;
                pubrec->packet_id = 1;
                pubrec->reason_code = ReasonCode::Success;
                packet = std::move(pubrec);
                break;
            }
            case PacketType::PUBREL: {
                auto pubrel = std::make_unique<PubRelPacket>(test_allocator);
                pubrel->type = type;
                pubrel->packet_id = 1;
                pubrel->reason_code = ReasonCode::Success;
                packet = std::move(pubrel);
                break;
            }
            case PacketType::PUBCOMP: {
                auto pubcomp = std::make_unique<PubCompPacket>(test_allocator);
                pubcomp->type = type;
                pubcomp->packet_id = 1;
                pubcomp->reason_code = ReasonCode::Success;
                packet = std::move(pubcomp);
                break;
            }
            case PacketType::SUBSCRIBE: {
                auto subscribe = std::make_unique<SubscribePacket>(test_allocator);
                subscribe->type = type;
                subscribe->packet_id = 1;
                subscribe->subscriptions.push_back(std::make_pair(MQTTString("test", MQTTStrAllocator(test_allocator)), 0));
                packet = std::move(subscribe);
                break;
            }
            case PacketType::SUBACK: {
                auto suback = std::make_unique<SubAckPacket>(test_allocator);
                suback->type = type;
                suback->packet_id = 1;
                suback->reason_codes.push_back(ReasonCode::GrantedQoS0);
                packet = std::move(suback);
                break;
            }
            case PacketType::UNSUBSCRIBE: {
                auto unsubscribe = std::make_unique<UnsubscribePacket>(test_allocator);
                unsubscribe->type = type;
                unsubscribe->packet_id = 1;
                unsubscribe->topic_filters.push_back(MQTTString("test", MQTTStrAllocator(test_allocator)));
                packet = std::move(unsubscribe);
                break;
            }
            case PacketType::UNSUBACK: {
                auto unsuback = std::make_unique<UnsubAckPacket>(test_allocator);
                unsuback->type = type;
                unsuback->packet_id = 1;
                unsuback->reason_codes.push_back(ReasonCode::Success);
                packet = std::move(unsuback);
                break;
            }
            case PacketType::PINGREQ: {
                auto pingreq = std::make_unique<PingReqPacket>(test_allocator);
                pingreq->type = type;
                packet = std::move(pingreq);
                break;
            }
            case PacketType::PINGRESP: {
                auto pingresp = std::make_unique<PingRespPacket>(test_allocator);
                pingresp->type = type;
                packet = std::move(pingresp);
                break;
            }
            case PacketType::DISCONNECT: {
                auto disconnect = std::make_unique<DisconnectPacket>(test_allocator);
                disconnect->type = type;
                disconnect->reason_code = ReasonCode::NormalDisconnection;
                packet = std::move(disconnect);
                break;
            }
            case PacketType::AUTH: {
                auto auth = std::make_unique<AuthPacket>(test_allocator);
                auth->type = type;
                auth->reason_code = ReasonCode::ContinueAuthentication;
                packet = std::move(auth);
                break;
            }
        }

        // Serialize and parse back
        MQTTSerializeBuffer buffer(test_allocator);
        int ret = MQ_SUCCESS;
        
        switch (type) {
            case PacketType::CONNECT:
                ret = parser->serialize_connect(static_cast<const ConnectPacket*>(packet.get()), buffer);
                break;
            case PacketType::CONNACK:
                ret = parser->serialize_connack(static_cast<const ConnAckPacket*>(packet.get()), buffer);
                break;
            case PacketType::PUBLISH:
                ret = parser->serialize_publish(static_cast<const PublishPacket*>(packet.get()), buffer);
                break;
            case PacketType::PUBACK:
                ret = parser->serialize_puback(static_cast<const PubAckPacket*>(packet.get()), buffer);
                break;
            case PacketType::PUBREC:
                ret = parser->serialize_pubrec(static_cast<const PubRecPacket*>(packet.get()), buffer);
                break;
            case PacketType::PUBREL:
                ret = parser->serialize_pubrel(static_cast<const PubRelPacket*>(packet.get()), buffer);
                break;
            case PacketType::PUBCOMP:
                ret = parser->serialize_pubcomp(static_cast<const PubCompPacket*>(packet.get()), buffer);
                break;
            case PacketType::SUBSCRIBE:
                ret = parser->serialize_subscribe(static_cast<const SubscribePacket*>(packet.get()), buffer);
                break;
            case PacketType::SUBACK:
                ret = parser->serialize_suback(static_cast<const SubAckPacket*>(packet.get()), buffer);
                break;
            case PacketType::UNSUBSCRIBE:
                ret = parser->serialize_unsubscribe(static_cast<const UnsubscribePacket*>(packet.get()), buffer);
                break;
            case PacketType::UNSUBACK:
                ret = parser->serialize_unsuback(static_cast<const UnsubAckPacket*>(packet.get()), buffer);
                break;
            case PacketType::PINGREQ:
                ret = parser->serialize_pingreq(static_cast<const PingReqPacket*>(packet.get()), buffer);
                break;
            case PacketType::PINGRESP:
                ret = parser->serialize_pingresp(static_cast<const PingRespPacket*>(packet.get()), buffer);
                break;
            case PacketType::DISCONNECT:
                ret = parser->serialize_disconnect(static_cast<const DisconnectPacket*>(packet.get()), buffer);
                break;
            case PacketType::AUTH:
                ret = parser->serialize_auth(static_cast<const AuthPacket*>(packet.get()), buffer);
                break;
        }
        
        ASSERT_EQ(ret, MQ_SUCCESS) << "Serialization failed for packet type " << static_cast<int>(type);

        // Parse back
        Packet* parsed_packet = nullptr;
        ret = parser->parse_packet(buffer.data(), buffer.size(), &parsed_packet);
        ASSERT_EQ(ret, MQ_SUCCESS) << "Parsing failed for packet type " << static_cast<int>(type);
        ASSERT_NE(parsed_packet, nullptr) << "Parsed packet is null for packet type " << static_cast<int>(type);
        ASSERT_EQ(parsed_packet->type, type) << "Packet type mismatch for packet type " << static_cast<int>(type);
    }
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}