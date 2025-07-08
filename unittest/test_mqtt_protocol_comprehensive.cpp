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

// Comprehensive MQTT v5 Protocol Test Class
class MQTTv5ComprehensiveTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Initialize memory allocator directly
        test_allocator = new MQTTAllocator("mqttv5_comprehensive_test", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
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

// ============= CONNECT Packet Tests =============

TEST_F(MQTTv5ComprehensiveTest, ConnectPacketBasic)
{
    // Test basic CONNECT packet parsing
    uint8_t connect_data[] = {
        0x10, 0x2c,  // Fixed header: type=CONNECT, remaining length=44
        0x00, 0x04, 'M', 'Q', 'T', 'T',  // Protocol name
        0x05,  // Protocol version (5)
        0xc2,  // Connect flags: username, password, clean start
        0x00, 0x3c,  // Keep alive (60)
        0x05,  // Properties length
        0x11, 0x00, 0x00, 0x00, 0x00,  // Session expiry interval = 0
        0x00, 0x0e, 'm', 'q', 't', 't', 'x', '_', '8', '0', '6', '9', '7', '1', '0', 'e',  // Client ID
        0x00, 0x04, 't', 'e', 's', 't',  // Username
        0x00, 0x04, 't', 'e', 's', 't'   // Password
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(connect_data, sizeof(connect_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::CONNECT);

    ConnectPacket* connect = static_cast<ConnectPacket*>(packet);
    EXPECT_EQ(connect->protocol_name, "MQTT");
    EXPECT_EQ(connect->protocol_version, 5);
    EXPECT_EQ(connect->keep_alive, 60);
    EXPECT_EQ(connect->client_id, "mqttx_8069710e");
    EXPECT_TRUE(connect->flags.username_flag);
    EXPECT_TRUE(connect->flags.password_flag);
    EXPECT_TRUE(connect->flags.clean_start);
    EXPECT_EQ(connect->username, "test");
    EXPECT_EQ(connect->password, "test");
}

TEST_F(MQTTv5ComprehensiveTest, ConnectPacketWithWillMessage)
{
    // Test CONNECT packet with will message
    uint8_t connect_data[] = {
        0x10, 0x42,  // Fixed header: type=CONNECT, remaining length=66
        0x00, 0x04, 'M', 'Q', 'T', 'T',  // Protocol name
        0x05,  // Protocol version (5)
        0xce,  // Connect flags: username, password, will, will QoS=1, clean start
        0x00, 0x3c,  // Keep alive (60)
        0x00,  // Properties length
        0x00, 0x0e, 'm', 'q', 't', 't', 'x', '_', '8', '0', '6', '9', '7', '1', '0', 'e',  // Client ID
        0x00,  // Will properties length
        0x00, 0x0a, 'w', 'i', 'l', 'l', '/', 't', 'o', 'p', 'i', 'c',  // Will topic (length=10)
        0x00, 0x0c, 'w', 'i', 'l', 'l', ' ', 'm', 'e', 's', 's', 'a', 'g', 'e',  // Will payload
        0x00, 0x04, 't', 'e', 's', 't',  // Username
        0x00, 0x04, 't', 'e', 's', 't'   // Password
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(connect_data, sizeof(connect_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::CONNECT);

    ConnectPacket* connect = static_cast<ConnectPacket*>(packet);
    EXPECT_TRUE(connect->flags.will_flag);
    EXPECT_EQ(connect->flags.will_qos, 1);
    EXPECT_EQ(connect->will_topic, "will/topic");
    EXPECT_EQ(connect->will_payload, "will message");
}

TEST_F(MQTTv5ComprehensiveTest, ConnectPacketWithProperties)
{
    // Test CONNECT packet with various properties
    uint8_t connect_data[] = {
        0x10, 0x37,  // Fixed header: type=CONNECT, remaining length=55
        0x00, 0x04, 'M', 'Q', 'T', 'T',  // Protocol name
        0x05,  // Protocol version (5)
        0xc2,  // Connect flags: username, password, clean start
        0x00, 0x3c,  // Keep alive (60)
        0x14,  // Properties length = 20
        0x11, 0x00, 0x00, 0x00, 0x78,  // Session expiry interval = 120
        0x21, 0x00, 0x64,  // Receive maximum = 100
        0x27, 0x00, 0x01, 0x00, 0x00,  // Maximum packet size = 65536
        0x22, 0x00, 0x0a,  // Topic alias maximum = 10
        0x19, 0x01,  // Request response information = true
        0x17, 0x01,  // Request problem information = true
        0x00, 0x0e, 'm', 'q', 't', 't', 'x', '_', '8', '0', '6', '9', '7', '1', '0', 'e',  // Client ID
        0x00, 0x04, 't', 'e', 's', 't',  // Username
        0x00, 0x04, 't', 'e', 's', 't'   // Password
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(connect_data, sizeof(connect_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::CONNECT);

    ConnectPacket* connect = static_cast<ConnectPacket*>(packet);
    EXPECT_EQ(connect->properties.session_expiry_interval, 120);
    EXPECT_EQ(connect->properties.receive_maximum, 100);
    EXPECT_EQ(connect->properties.maximum_packet_size, 65536);
    EXPECT_EQ(connect->properties.topic_alias_maximum, 10);
    EXPECT_TRUE(connect->properties.request_response_information);
    EXPECT_TRUE(connect->properties.request_problem_information);
}

// ============= PUBLISH Packet Tests =============

TEST_F(MQTTv5ComprehensiveTest, PublishPacketQoS0)
{
    // Test PUBLISH packet with QoS 0
    uint8_t publish_data[] = {
        0x30, 0x19,  // Fixed header: type=PUBLISH, QoS=0, remaining length=25
        0x00, 0x0a, 't', 'e', 's', 't', '/', 't', 'o', 'p', 'i', 'c',  // Topic name
        0x07,  // Properties length
        0x01, 0x01,  // Payload format indicator = 1
        0x02, 0x00, 0x00, 0x00, 0x3c,  // Message expiry interval = 60 (4-byte value)
        'H', 'e', 'l', 'l', 'o'  // Payload (no length prefix in MQTT v5)
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_data, sizeof(publish_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::PUBLISH);

    PublishPacket* publish = static_cast<PublishPacket*>(packet);
    EXPECT_EQ(publish->qos, 0);
    EXPECT_FALSE(publish->dup);
    EXPECT_FALSE(publish->retain);
    EXPECT_EQ(publish->topic_name, "test/topic");
    EXPECT_EQ(publish->properties.payload_format_indicator, 1);
    EXPECT_EQ(publish->properties.message_expiry_interval, 60);
    EXPECT_EQ(publish->payload.size(), 5);
    EXPECT_EQ(std::string(publish->payload.begin(), publish->payload.end()), "Hello");
}

TEST_F(MQTTv5ComprehensiveTest, PublishPacketQoS1)
{
    // Test PUBLISH packet with QoS 1
    uint8_t publish_data[] = {
        0x32, 0x1b,  // Fixed header: type=PUBLISH, QoS=1, remaining length=27
        0x00, 0x0a, 't', 'e', 's', 't', '/', 't', 'o', 'p', 'i', 'c',  // Topic name
        0x00, 0x01,  // Packet ID = 1
        0x07,  // Properties length
        0x01, 0x01,  // Payload format indicator = 1
        0x02, 0x00, 0x00, 0x00, 0x3c,  // Message expiry interval = 60 (4-byte value)
        'H', 'e', 'l', 'l', 'o'  // Payload (no length prefix in MQTT v5)
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_data, sizeof(publish_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::PUBLISH);

    PublishPacket* publish = static_cast<PublishPacket*>(packet);
    EXPECT_EQ(publish->qos, 1);
    EXPECT_EQ(publish->packet_id, 1);
    EXPECT_EQ(publish->topic_name, "test/topic");
}

TEST_F(MQTTv5ComprehensiveTest, PublishPacketWithTopicAlias)
{
    // Test PUBLISH packet with topic alias
    uint8_t publish_data[] = {
        0x30, 0x0b,  // Fixed header: type=PUBLISH, QoS=0, remaining length=11
        0x00, 0x00,  // Topic name (empty for topic alias)
        0x03,  // Properties length
        0x23, 0x00, 0x05,  // Topic alias = 5
        'H', 'e', 'l', 'l', 'o'  // Payload (no length prefix in MQTT v5)
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_data, sizeof(publish_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::PUBLISH);

    PublishPacket* publish = static_cast<PublishPacket*>(packet);
    EXPECT_EQ(publish->topic_name, "");
    EXPECT_EQ(publish->properties.topic_alias, 5);
}

// ============= SUBSCRIBE Packet Tests =============

TEST_F(MQTTv5ComprehensiveTest, SubscribePacketBasic)
{
    // Test basic SUBSCRIBE packet
    uint8_t subscribe_data[] = {
        0x82, 0x1a,  // Fixed header: type=SUBSCRIBE, remaining length=26
        0x00, 0x01,  // Packet ID = 1
        0x00,  // Properties length
        0x00, 0x0a, 't', 'e', 's', 't', '/', 't', 'o', 'p', 'i', 'c',  // Topic filter
        0x01,  // Subscription options: QoS=1
        0x00, 0x07, 'a', 'n', 'o', 't', 'h', 'e', 'r',  // Another topic filter
        0x02   // Subscription options: QoS=2
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(subscribe_data, sizeof(subscribe_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::SUBSCRIBE);

    SubscribePacket* subscribe = static_cast<SubscribePacket*>(packet);
    EXPECT_EQ(subscribe->packet_id, 1);
    EXPECT_EQ(subscribe->subscriptions.size(), 2);
    EXPECT_EQ(subscribe->subscriptions[0].first, "test/topic");
    EXPECT_EQ(subscribe->subscriptions[0].second, 1);
    EXPECT_EQ(subscribe->subscriptions[1].first, "another");
    EXPECT_EQ(subscribe->subscriptions[1].second, 2);
}

// ============= SUBACK Packet Tests =============

TEST_F(MQTTv5ComprehensiveTest, SubAckPacketBasic)
{
    // Test basic SUBACK packet
    uint8_t suback_data[] = {
        0x90, 0x06,  // Fixed header: type=SUBACK, remaining length=6
        0x00, 0x01,  // Packet ID = 1
        0x00,  // Properties length
        0x01,  // Reason code: Granted QoS 1
        0x02,  // Reason code: Granted QoS 2
        0x80   // Reason code: Unspecified error
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(suback_data, sizeof(suback_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::SUBACK);

    SubAckPacket* suback = static_cast<SubAckPacket*>(packet);
    EXPECT_EQ(suback->packet_id, 1);
    EXPECT_EQ(suback->reason_codes.size(), 3);
    EXPECT_EQ(suback->reason_codes[0], ReasonCode::GrantedQoS1);
    EXPECT_EQ(suback->reason_codes[1], ReasonCode::GrantedQoS2);
    EXPECT_EQ(suback->reason_codes[2], ReasonCode::UnspecifiedError);
}

// ============= UNSUBSCRIBE Packet Tests =============

TEST_F(MQTTv5ComprehensiveTest, UnsubscribePacketBasic)
{
    // Test basic UNSUBSCRIBE packet
    uint8_t unsubscribe_data[] = {
        0xa2, 0x18,  // Fixed header: type=UNSUBSCRIBE, remaining length=24
        0x00, 0x01,  // Packet ID = 1
        0x00,  // Properties length
        0x00, 0x0a, 't', 'e', 's', 't', '/', 't', 'o', 'p', 'i', 'c',  // Topic filter
        0x00, 0x07, 'a', 'n', 'o', 't', 'h', 'e', 'r'  // Another topic filter
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(unsubscribe_data, sizeof(unsubscribe_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::UNSUBSCRIBE);

    UnsubscribePacket* unsubscribe = static_cast<UnsubscribePacket*>(packet);
    EXPECT_EQ(unsubscribe->packet_id, 1);
    EXPECT_EQ(unsubscribe->topic_filters.size(), 2);
    EXPECT_EQ(unsubscribe->topic_filters[0], "test/topic");
    EXPECT_EQ(unsubscribe->topic_filters[1], "another");
}

// ============= UNSUBACK Packet Tests =============

TEST_F(MQTTv5ComprehensiveTest, UnsubAckPacketBasic)
{
    // Test basic UNSUBACK packet
    uint8_t unsuback_data[] = {
        0xb0, 0x05,  // Fixed header: type=UNSUBACK, remaining length=5
        0x00, 0x01,  // Packet ID = 1
        0x00,  // Properties length
        0x00,  // Reason code: Success
        0x11   // Reason code: No subscription existed
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(unsuback_data, sizeof(unsuback_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::UNSUBACK);

    UnsubAckPacket* unsuback = static_cast<UnsubAckPacket*>(packet);
    EXPECT_EQ(unsuback->packet_id, 1);
    EXPECT_EQ(unsuback->reason_codes.size(), 2);
    EXPECT_EQ(unsuback->reason_codes[0], ReasonCode::Success);
    EXPECT_EQ(unsuback->reason_codes[1], ReasonCode::NoSubscriptionExisted);
}

// ============= CONNACK Packet Tests =============

TEST_F(MQTTv5ComprehensiveTest, ConnAckPacketBasic)
{
    // Test basic CONNACK packet
    uint8_t connack_data[] = {
        0x20, 0x03,  // Fixed header: type=CONNACK, remaining length=3
        0x01,  // Connect acknowledge flags: session present
        0x00,  // Reason code: Success
        0x00   // Properties length
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(connack_data, sizeof(connack_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::CONNACK);

    ConnAckPacket* connack = static_cast<ConnAckPacket*>(packet);
    EXPECT_TRUE(connack->session_present);
    EXPECT_EQ(connack->reason_code, ReasonCode::Success);
}

// ============= DISCONNECT Packet Tests =============

TEST_F(MQTTv5ComprehensiveTest, DisconnectPacketBasic)
{
    // Test basic DISCONNECT packet
    uint8_t disconnect_data[] = {
        0xe0, 0x02,  // Fixed header: type=DISCONNECT, remaining length=2
        0x00,  // Reason code: Normal disconnection
        0x00   // Properties length
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(disconnect_data, sizeof(disconnect_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::DISCONNECT);

    DisconnectPacket* disconnect = static_cast<DisconnectPacket*>(packet);
    EXPECT_EQ(disconnect->reason_code, ReasonCode::NormalDisconnection);
}

// ============= AUTH Packet Tests =============

TEST_F(MQTTv5ComprehensiveTest, AuthPacketBasic)
{
    // Test basic AUTH packet
    uint8_t auth_data[] = {
        0xf0, 0x02,  // Fixed header: type=AUTH, remaining length=2
        0x18,  // Reason code: Continue authentication
        0x00   // Properties length
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(auth_data, sizeof(auth_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::AUTH);

    AuthPacket* auth = static_cast<AuthPacket*>(packet);
    EXPECT_EQ(auth->reason_code, ReasonCode::ContinueAuthentication);
}

// ============= PING Packet Tests =============

TEST_F(MQTTv5ComprehensiveTest, PingReqPacketBasic)
{
    // Test basic PINGREQ packet
    uint8_t pingreq_data[] = {
        0xc0, 0x00  // Fixed header: type=PINGREQ, remaining length=0
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(pingreq_data, sizeof(pingreq_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::PINGREQ);
}

TEST_F(MQTTv5ComprehensiveTest, PingRespPacketBasic)
{
    // Test basic PINGRESP packet
    uint8_t pingresp_data[] = {
        0xd0, 0x00  // Fixed header: type=PINGRESP, remaining length=0
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(pingresp_data, sizeof(pingresp_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::PINGRESP);
}

// ============= QoS Flow Tests =============

TEST_F(MQTTv5ComprehensiveTest, PubAckPacketBasic)
{
    // Test basic PUBACK packet
    uint8_t puback_data[] = {
        0x40, 0x04,  // Fixed header: type=PUBACK, remaining length=4
        0x00, 0x01,  // Packet ID = 1
        0x00,  // Reason code: Success
        0x00   // Properties length
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(puback_data, sizeof(puback_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::PUBACK);

    PubAckPacket* puback = static_cast<PubAckPacket*>(packet);
    EXPECT_EQ(puback->packet_id, 1);
    EXPECT_EQ(puback->reason_code, ReasonCode::Success);
}

TEST_F(MQTTv5ComprehensiveTest, PubRecPacketBasic)
{
    // Test basic PUBREC packet
    uint8_t pubrec_data[] = {
        0x50, 0x04,  // Fixed header: type=PUBREC, remaining length=4
        0x00, 0x01,  // Packet ID = 1
        0x00,  // Reason code: Success
        0x00   // Properties length
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(pubrec_data, sizeof(pubrec_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::PUBREC);

    PubRecPacket* pubrec = static_cast<PubRecPacket*>(packet);
    EXPECT_EQ(pubrec->packet_id, 1);
    EXPECT_EQ(pubrec->reason_code, ReasonCode::Success);
}

TEST_F(MQTTv5ComprehensiveTest, PubRelPacketBasic)
{
    // Test basic PUBREL packet
    uint8_t pubrel_data[] = {
        0x62, 0x04,  // Fixed header: type=PUBREL, remaining length=4
        0x00, 0x01,  // Packet ID = 1
        0x00,  // Reason code: Success
        0x00   // Properties length
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(pubrel_data, sizeof(pubrel_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::PUBREL);

    PubRelPacket* pubrel = static_cast<PubRelPacket*>(packet);
    EXPECT_EQ(pubrel->packet_id, 1);
    EXPECT_EQ(pubrel->reason_code, ReasonCode::Success);
}

TEST_F(MQTTv5ComprehensiveTest, PubCompPacketBasic)
{
    // Test basic PUBCOMP packet
    uint8_t pubcomp_data[] = {
        0x70, 0x04,  // Fixed header: type=PUBCOMP, remaining length=4
        0x00, 0x01,  // Packet ID = 1
        0x00,  // Reason code: Success
        0x00   // Properties length
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(pubcomp_data, sizeof(pubcomp_data), &packet);
    
    ASSERT_EQ(ret, MQ_SUCCESS);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(packet->type, PacketType::PUBCOMP);

    PubCompPacket* pubcomp = static_cast<PubCompPacket*>(packet);
    EXPECT_EQ(pubcomp->packet_id, 1);
    EXPECT_EQ(pubcomp->reason_code, ReasonCode::Success);
}

// ============= Error Condition Tests =============

TEST_F(MQTTv5ComprehensiveTest, InvalidPacketType)
{
    // Test invalid packet type
    uint8_t invalid_data[] = {
        0x00, 0x02,  // Invalid packet type
        0x00, 0x00
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(invalid_data, sizeof(invalid_data), &packet);
    
    EXPECT_EQ(ret, MQ_ERR_PACKET_TYPE);
    EXPECT_EQ(packet, nullptr);
}

TEST_F(MQTTv5ComprehensiveTest, PacketTooShort)
{
    // Test packet too short
    uint8_t short_data[] = {0x10};

    Packet* packet = nullptr;
    int ret = parser->parse_packet(short_data, sizeof(short_data), &packet);
    
    EXPECT_EQ(ret, MQ_ERR_PACKET_INCOMPLETE);
    EXPECT_EQ(packet, nullptr);
}

TEST_F(MQTTv5ComprehensiveTest, IncompletePacket)
{
    // Test incomplete packet
    uint8_t incomplete_data[] = {
        0x10, 0x2c,  // Fixed header claims 44 bytes but only have 4
        0x00, 0x04
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(incomplete_data, sizeof(incomplete_data), &packet);
    
    EXPECT_EQ(ret, MQ_ERR_PACKET_INCOMPLETE);
    EXPECT_EQ(packet, nullptr);
}

TEST_F(MQTTv5ComprehensiveTest, MalformedRemainingLength)
{
    // Test malformed remaining length
    uint8_t malformed_data[] = {
        0x10, 0xff, 0xff, 0xff, 0xff,  // Invalid remaining length encoding
        0x00, 0x04
    };

    Packet* packet = nullptr;
    int ret = parser->parse_packet(malformed_data, sizeof(malformed_data), &packet);
    
    EXPECT_EQ(ret, MQ_ERR_PACKET_INVALID);
    EXPECT_EQ(packet, nullptr);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}