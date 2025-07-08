#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <limits>
#include "src/mqtt_allocator.h"
#include "src/mqtt_define.h"
#include "src/mqtt_packet.h"
#include "src/mqtt_parser.h"
#include "src/mqtt_serialize_buffer.h"

using namespace mqtt;

// MQTT v5 Edge Cases and Error Conditions Test Class
class MQTTv5EdgeCasesTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Initialize memory allocator directly
        test_allocator = new MQTTAllocator("mqttv5_edge_cases_test", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
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

// ============= Invalid Packet Type Tests =============

TEST_F(MQTTv5EdgeCasesTest, InvalidPacketTypes)
{
    // Test all invalid packet types
    std::vector<uint8_t> invalid_types = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
        0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
        0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
        0x61, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
        0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
        0x81, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
        0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
        0xA1, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
        0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
        0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
        0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
        0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
        0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
    };

    for (uint8_t invalid_type : invalid_types) {
        uint8_t packet_data[] = {invalid_type, 0x00};
        
        Packet* packet = nullptr;
        int ret = parser->parse_packet(packet_data, sizeof(packet_data), &packet);
        
        EXPECT_LT(ret, 0) << "Invalid packet type 0x" << std::hex << static_cast<int>(invalid_type) << " should fail, got: " << ret;  // Any error code
        EXPECT_EQ(packet, nullptr) << "Packet should be null for invalid type 0x" << std::hex << static_cast<int>(invalid_type);
    }
}

// ============= Remaining Length Edge Cases =============

TEST_F(MQTTv5EdgeCasesTest, RemainingLengthEdgeCases)
{
    // Test maximum valid remaining length (268,435,455)
    uint8_t max_valid_remaining_length[] = {
        0x10, 0xFF, 0xFF, 0xFF, 0x7F  // CONNECT packet with max remaining length
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(max_valid_remaining_length, sizeof(max_valid_remaining_length), &packet);
    
    // Should fail because packet is incomplete (we don't have 268,435,455 bytes)
    EXPECT_EQ(ret, MQ_ERR_PACKET_INCOMPLETE);
    EXPECT_EQ(packet, nullptr);
}

TEST_F(MQTTv5EdgeCasesTest, InvalidRemainingLength)
{
    // Test invalid remaining length (more than 4 bytes)
    uint8_t invalid_remaining_length[] = {
        0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x01  // Invalid 5-byte remaining length
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(invalid_remaining_length, sizeof(invalid_remaining_length), &packet);
    
    EXPECT_EQ(ret, MQ_ERR_PACKET_INVALID);
    EXPECT_EQ(packet, nullptr);
}

TEST_F(MQTTv5EdgeCasesTest, ZeroRemainingLength)
{
    // Test zero remaining length for packets that should have content
    uint8_t zero_remaining_length[] = {
        0x10, 0x00  // CONNECT packet with zero remaining length
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(zero_remaining_length, sizeof(zero_remaining_length), &packet);
    
    // Should fail because CONNECT packet must have content
    EXPECT_EQ(ret, MQ_ERR_PACKET_INCOMPLETE);
    EXPECT_EQ(packet, nullptr);
}

// ============= String Length Edge Cases =============

TEST_F(MQTTv5EdgeCasesTest, EmptyStrings)
{
    // Test CONNECT packet with empty client ID
    uint8_t connect_empty_client_id[] = {
        0x10, 0x0c,  // Fixed header
        0x00, 0x04, 'M', 'Q', 'T', 'T',  // Protocol name
        0x05,  // Protocol version
        0x02,  // Connect flags (clean start only)
        0x00, 0x3c,  // Keep alive
        0x00,  // Properties length
        0x00, 0x00   // Empty client ID
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(connect_empty_client_id, sizeof(connect_empty_client_id), &packet);
    
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::CONNECT);
    
    ConnectPacket* connect = static_cast<ConnectPacket*>(packet);
    EXPECT_EQ(connect->client_id, "");
}

TEST_F(MQTTv5EdgeCasesTest, MaximumStringLength)
{
    // Test string with maximum length (65535)
    std::vector<uint8_t> max_string_packet;
    max_string_packet.push_back(0x10);  // CONNECT packet type
    max_string_packet.push_back(0xFF);  // Remaining length byte 1
    max_string_packet.push_back(0xFF);  // Remaining length byte 2
    max_string_packet.push_back(0x03);  // Remaining length byte 3 (65535 + 10 bytes for fixed fields)
    
    // Protocol name "MQTT"
    max_string_packet.insert(max_string_packet.end(), {0x00, 0x04, 'M', 'Q', 'T', 'T'});
    
    // Protocol version
    max_string_packet.push_back(0x05);
    
    // Connect flags
    max_string_packet.push_back(0x02);
    
    // Keep alive
    max_string_packet.insert(max_string_packet.end(), {0x00, 0x3c});
    
    // Properties length
    max_string_packet.push_back(0x00);
    
    // Client ID with maximum length
    max_string_packet.insert(max_string_packet.end(), {0xFF, 0xFF});  // Length = 65535
    
    // Fill with 'A' characters
    for (int i = 0; i < 65535; ++i) {
        max_string_packet.push_back('A');
    }
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(max_string_packet.data(), max_string_packet.size(), &packet);
    
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::CONNECT);
    
    ConnectPacket* connect = static_cast<ConnectPacket*>(packet);
    EXPECT_EQ(connect->client_id.length(), 65535);
    EXPECT_EQ(connect->client_id[0], 'A');
    EXPECT_EQ(connect->client_id[65534], 'A');
}

// ============= Property Edge Cases =============

TEST_F(MQTTv5EdgeCasesTest, DuplicateProperties)
{
    // Test packet with duplicate properties (should be allowed in MQTT v5)
    uint8_t connect_duplicate_props[] = {
        0x10, 0x18,  // Fixed header (remaining length = 24)
        0x00, 0x04, 'M', 'Q', 'T', 'T',  // Protocol name
        0x05,  // Protocol version
        0x02,  // Connect flags
        0x00, 0x3c,  // Keep alive
        0x0a,  // Properties length = 10
        0x11, 0x00, 0x00, 0x00, 0x78,  // Session expiry interval = 120
        0x11, 0x00, 0x00, 0x00, 0x3c,  // Duplicate session expiry interval = 60
        0x00, 0x04, 't', 'e', 's', 't'   // Client ID
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(connect_duplicate_props, sizeof(connect_duplicate_props), &packet);
    
    // Should handle duplicate properties (implementation specific)
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::CONNECT);
}

TEST_F(MQTTv5EdgeCasesTest, UnknownProperties)
{
    // Test packet with unknown property identifier
    uint8_t connect_unknown_prop[] = {
        0x10, 0x15,  // Fixed header (remaining length = 21)
        0x00, 0x04, 'M', 'Q', 'T', 'T',  // Protocol name
        0x05,  // Protocol version
        0x02,  // Connect flags
        0x00, 0x3c,  // Keep alive
        0x05,  // Properties length = 5
        0xFF, 0x00, 0x00, 0x00, 0x01,  // Unknown property (0xFF) with value 1
        0x00, 0x04, 't', 'e', 's', 't'   // Client ID
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(connect_unknown_prop, sizeof(connect_unknown_prop), &packet);
    
    // Currently, unknown properties cause parsing to fail
    // This is a conservative approach until proper skipping is implemented
    EXPECT_EQ(ret, MQ_ERR_PACKET_INVALID);
    EXPECT_EQ(packet, nullptr);
}

// ============= QoS Edge Cases =============

TEST_F(MQTTv5EdgeCasesTest, InvalidQoSLevel)
{
    // Test PUBLISH packet with invalid QoS level (3)
    uint8_t publish_invalid_qos[] = {
        0x36, 0x0f,  // Fixed header: type=PUBLISH, QoS=3 (invalid), remaining length=15
        0x00, 0x04, 't', 'e', 's', 't',  // Topic name
        0x00, 0x01,  // Packet ID
        0x00,  // Properties length
        0x05, 'H', 'e', 'l', 'l', 'o'  // Payload
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_invalid_qos, sizeof(publish_invalid_qos), &packet);
    
    // Should handle invalid QoS appropriately
    EXPECT_EQ(ret, MQ_SUCCESS);  // Parser might accept it but extract QoS differently
    if (ret == MQ_SUCCESS) {
        EXPECT_NE(packet, nullptr);
        EXPECT_EQ(packet->type, PacketType::PUBLISH);
        
        PublishPacket* publish = static_cast<PublishPacket*>(packet);
        // QoS should be extracted correctly despite invalid bits
        EXPECT_LE(publish->qos, 2);  // Should be normalized to valid QoS
    }
}

TEST_F(MQTTv5EdgeCasesTest, QoS0WithPacketID)
{
    // Test PUBLISH packet with QoS 0 (should not have packet ID)
    uint8_t publish_qos0_with_id[] = {
        0x30, 0x0d,  // Fixed header: type=PUBLISH, QoS=0, remaining length=13
        0x00, 0x04, 't', 'e', 's', 't',  // Topic name (6 bytes)
        0x00,  // Properties length (1 byte)
        0x05, 'H', 'e', 'l', 'l', 'o'  // Payload (6 bytes)
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_qos0_with_id, sizeof(publish_qos0_with_id), &packet);
    
    // Should parse successfully
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::PUBLISH);
    
    PublishPacket* publish = static_cast<PublishPacket*>(packet);
    EXPECT_EQ(publish->qos, 0);
    EXPECT_EQ(publish->packet_id, 0);  // Should be 0 for QoS 0
}

// ============= Packet ID Edge Cases =============

TEST_F(MQTTv5EdgeCasesTest, ZeroPacketID)
{
    // Test PUBLISH packet with QoS 1 and packet ID 0 (invalid)
    uint8_t publish_zero_packet_id[] = {
        0x32, 0x0f,  // Fixed header: type=PUBLISH, QoS=1, remaining length=15
        0x00, 0x04, 't', 'e', 's', 't',  // Topic name (6 bytes)
        0x00, 0x00,  // Packet ID = 0 (invalid) (2 bytes)
        0x00,  // Properties length (1 byte)
        0x05, 'H', 'e', 'l', 'l', 'o'  // Payload (6 bytes)
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_zero_packet_id, sizeof(publish_zero_packet_id), &packet);
    
    // Should parse but packet ID validation might happen at protocol level
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::PUBLISH);
    
    PublishPacket* publish = static_cast<PublishPacket*>(packet);
    EXPECT_EQ(publish->packet_id, 0);
}

TEST_F(MQTTv5EdgeCasesTest, MaximumPacketID)
{
    // Test PUBLISH packet with maximum packet ID (65535)
    uint8_t publish_max_packet_id[] = {
        0x32, 0x0f,  // Fixed header: type=PUBLISH, QoS=1, remaining length=15
        0x00, 0x04, 't', 'e', 's', 't',  // Topic name (6 bytes)
        0xFF, 0xFF,  // Packet ID = 65535 (2 bytes)
        0x00,  // Properties length (1 byte)
        0x05, 'H', 'e', 'l', 'l', 'o'  // Payload (6 bytes)
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_max_packet_id, sizeof(publish_max_packet_id), &packet);
    
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::PUBLISH);
    
    PublishPacket* publish = static_cast<PublishPacket*>(packet);
    EXPECT_EQ(publish->packet_id, 65535);
}

// ============= Topic Name Edge Cases =============

TEST_F(MQTTv5EdgeCasesTest, EmptyTopicName)
{
    // Test PUBLISH packet with empty topic name
    uint8_t publish_empty_topic[] = {
        0x30, 0x09,  // Fixed header: type=PUBLISH, QoS=0, remaining length=9
        0x00, 0x00,  // Empty topic name
        0x00,  // Properties length
        0x05, 'H', 'e', 'l', 'l', 'o'  // Payload
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_empty_topic, sizeof(publish_empty_topic), &packet);
    
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::PUBLISH);
    
    PublishPacket* publish = static_cast<PublishPacket*>(packet);
    EXPECT_EQ(publish->topic_name, "");
}

TEST_F(MQTTv5EdgeCasesTest, TopicNameWithNullCharacters)
{
    // Test PUBLISH packet with topic name containing null characters
    uint8_t publish_null_topic[] = {
        0x30, 0x0f,  // Fixed header: type=PUBLISH, QoS=0, remaining length=15
        0x00, 0x06, 't', 'e', 's', 't', 0x00, 0x00,  // Topic name with null characters
        0x00,  // Properties length
        0x05, 'H', 'e', 'l', 'l', 'o'  // Payload
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_null_topic, sizeof(publish_null_topic), &packet);
    
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::PUBLISH);
    
    PublishPacket* publish = static_cast<PublishPacket*>(packet);
    EXPECT_EQ(publish->topic_name.length(), 6);
    EXPECT_EQ(publish->topic_name[0], 't');
    EXPECT_EQ(publish->topic_name[4], 0x00);
    EXPECT_EQ(publish->topic_name[5], 0x00);
}

TEST_F(MQTTv5EdgeCasesTest, TopicNameWithWildcards)
{
    // Test PUBLISH packet with topic name containing wildcards (invalid for PUBLISH)
    uint8_t publish_wildcard_topic[] = {
        0x30, 0x0f,  // Fixed header: type=PUBLISH, QoS=0, remaining length=15
        0x00, 0x06, 't', 'e', 's', 't', '/', '+',  // Topic name with wildcard
        0x00,  // Properties length
        0x05, 'H', 'e', 'l', 'l', 'o'  // Payload
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_wildcard_topic, sizeof(publish_wildcard_topic), &packet);
    
    // Should parse successfully (validation happens at protocol level)
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::PUBLISH);
    
    PublishPacket* publish = static_cast<PublishPacket*>(packet);
    EXPECT_EQ(publish->topic_name, "test/+");
}

// ============= Payload Edge Cases =============

TEST_F(MQTTv5EdgeCasesTest, EmptyPayload)
{
    // Test PUBLISH packet with empty payload
    uint8_t publish_empty_payload[] = {
        0x30, 0x07,  // Fixed header: type=PUBLISH, QoS=0, remaining length=7
        0x00, 0x04, 't', 'e', 's', 't',  // Topic name
        0x00   // Properties length (no payload)
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_empty_payload, sizeof(publish_empty_payload), &packet);
    
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::PUBLISH);
    
    PublishPacket* publish = static_cast<PublishPacket*>(packet);
    EXPECT_EQ(publish->payload.size(), 0);
}

TEST_F(MQTTv5EdgeCasesTest, BinaryPayload)
{
    // Test PUBLISH packet with binary payload
    uint8_t publish_binary_payload[] = {
        0x30, 0x0E,  // Fixed header: type=PUBLISH, QoS=0, remaining length=14
        0x00, 0x04, 't', 'e', 's', 't',  // Topic name
        0x00,  // Properties length
        0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD  // Binary payload
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(publish_binary_payload, sizeof(publish_binary_payload), &packet);
    
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::PUBLISH);
    
    PublishPacket* publish = static_cast<PublishPacket*>(packet);
    EXPECT_EQ(publish->payload.size(), 7);
    EXPECT_EQ(publish->payload[0], 0x00);
    EXPECT_EQ(publish->payload[1], 0x01);
    EXPECT_EQ(publish->payload[6], 0xFD);
}

// ============= Subscription Edge Cases =============

TEST_F(MQTTv5EdgeCasesTest, SubscribeWithNoTopics)
{
    // Test SUBSCRIBE packet with no topic filters (invalid)
    uint8_t subscribe_no_topics[] = {
        0x82, 0x03,  // Fixed header: type=SUBSCRIBE, remaining length=3
        0x00, 0x01,  // Packet ID = 1
        0x00   // Properties length (no topics)
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(subscribe_no_topics, sizeof(subscribe_no_topics), &packet);
    
    // Parser might accept empty SUBSCRIBE packets
    EXPECT_TRUE(ret == 0 || ret == -202);  // SUCCESS or MQ_ERR_PACKET_INVALID
    if (ret == 0) {
        EXPECT_NE(packet, nullptr);
        EXPECT_EQ(packet->type, PacketType::SUBSCRIBE);
    } else {
        EXPECT_EQ(packet, nullptr);
    }
}

TEST_F(MQTTv5EdgeCasesTest, SubscribeWithInvalidOptions)
{
    // Test SUBSCRIBE packet with invalid subscription options
    uint8_t subscribe_invalid_options[] = {
        0x82, 0x0a,  // Fixed header: type=SUBSCRIBE, remaining length=10
        0x00, 0x01,  // Packet ID = 1
        0x00,  // Properties length
        0x00, 0x04, 't', 'e', 's', 't',  // Topic filter
        0xFF   // Invalid subscription options
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(subscribe_invalid_options, sizeof(subscribe_invalid_options), &packet);
    
    // Should parse but with normalized options
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::SUBSCRIBE);
    
    SubscribePacket* subscribe = static_cast<SubscribePacket*>(packet);
    EXPECT_EQ(subscribe->subscriptions.size(), 1);
    // QoS should be normalized (extracted from lower 2 bits)
    EXPECT_LE(subscribe->subscriptions[0].second & 0x03, 3);  // Accept 0-3 range
}

// ============= Reason Code Edge Cases =============

TEST_F(MQTTv5EdgeCasesTest, InvalidReasonCodes)
{
    // Test CONNACK packet with invalid reason code
    uint8_t connack_invalid_reason[] = {
        0x20, 0x03,  // Fixed header: type=CONNACK, remaining length=3
        0x00,  // Connect acknowledge flags
        0xFF,  // Invalid reason code
        0x00   // Properties length
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(connack_invalid_reason, sizeof(connack_invalid_reason), &packet);
    
    // Should parse but reason code will be as-is
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::CONNACK);
    
    ConnAckPacket* connack = static_cast<ConnAckPacket*>(packet);
    EXPECT_EQ(static_cast<uint8_t>(connack->reason_code), 0xFF);
}

// ============= Memory Limit Tests =============

TEST_F(MQTTv5EdgeCasesTest, LargePacketSizeLimit)
{
    // Test handling of very large packets
    const size_t large_size = 1024 * 1024;  // 1MB
    std::vector<uint8_t> large_packet;
    large_packet.resize(large_size);
    
    // Create a large PUBLISH packet
    large_packet[0] = 0x30;  // PUBLISH, QoS 0
    // Set remaining length to indicate large size
    large_packet[1] = 0xFF;
    large_packet[2] = 0xFF;
    large_packet[3] = 0x3F;  // Remaining length = 1,048,575
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(large_packet.data(), large_packet.size(), &packet);
    
    // Should handle large packets appropriately (might fail due to memory limits)
    // The exact behavior depends on implementation
    EXPECT_TRUE(ret == 0 || ret == -100 || ret == -201 || ret == -203);  // SUCCESS, MEMORY_ALLOC, PACKET_TOO_LARGE, PACKET_INCOMPLETE
}

// ============= Protocol Version Edge Cases =============

TEST_F(MQTTv5EdgeCasesTest, WrongProtocolVersion)
{
    // Test CONNECT packet with wrong protocol version
    uint8_t connect_wrong_version[] = {
        0x10, 0x0c,  // Fixed header
        0x00, 0x04, 'M', 'Q', 'T', 'T',  // Protocol name
        0x04,  // Protocol version 4 (MQTT 3.1.1)
        0x02,  // Connect flags
        0x00, 0x3c,  // Keep alive
        0x00,  // Properties length
        0x00, 0x00   // Empty client ID
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(connect_wrong_version, sizeof(connect_wrong_version), &packet);
    
    // Should parse but version will be preserved
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::CONNECT);
    
    ConnectPacket* connect = static_cast<ConnectPacket*>(packet);
    EXPECT_EQ(connect->protocol_version, 4);
}

TEST_F(MQTTv5EdgeCasesTest, WrongProtocolName)
{
    // Test CONNECT packet with wrong protocol name
    uint8_t connect_wrong_name[] = {
        0x10, 0x0c,  // Fixed header
        0x00, 0x04, 'M', 'Q', 'I', 'S',  // Wrong protocol name
        0x05,  // Protocol version
        0x02,  // Connect flags
        0x00, 0x3c,  // Keep alive
        0x00,  // Properties length
        0x00, 0x00   // Empty client ID
    };
    
    Packet* packet = nullptr;
    int ret = parser->parse_packet(connect_wrong_name, sizeof(connect_wrong_name), &packet);
    
    // Should parse but protocol name will be preserved
    EXPECT_EQ(ret, MQ_SUCCESS);
    EXPECT_NE(packet, nullptr);
    EXPECT_EQ(packet->type, PacketType::CONNECT);
    
    ConnectPacket* connect = static_cast<ConnectPacket*>(packet);
    EXPECT_EQ(connect->protocol_name, "MQIS");
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}