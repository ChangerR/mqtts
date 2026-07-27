#include <gtest/gtest.h>

#include <string>

#include "http_parser.h"

namespace {

TEST(HttpParserTest, ParsesUpgradeRequestWithCaseInsensitiveHeaders)
{
  MQTTAllocator allocator("test_http", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  http::HttpParser parser(&allocator, http::HttpParserType::REQUEST);

  const mqtt::MQTTString raw = mqtt::to_mqtt_string(
      "GET /mqtt HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: abc123==\r\n"
      "\r\n",
      &allocator);

  size_t consumed = 0;
  http::HttpParseStatus status = parser.execute(raw, consumed);
  ASSERT_EQ(http::HttpParseStatus::OK, status);
  ASSERT_TRUE(parser.message_complete());

  const http::HttpRequest& req = parser.request();
  EXPECT_EQ(mqtt::to_mqtt_string("GET", &allocator), req.method);
  EXPECT_EQ(mqtt::to_mqtt_string("websocket", &allocator),
            req.get_header(mqtt::to_mqtt_string("upgrade", &allocator)));
  EXPECT_EQ(mqtt::to_mqtt_string("websocket", &allocator),
            req.get_header(mqtt::to_mqtt_string("UPGRADE", &allocator)));
  EXPECT_EQ(mqtt::to_mqtt_string("abc123==", &allocator),
            req.get_header(mqtt::to_mqtt_string("sec-websocket-key", &allocator)));
}

TEST(HttpParserTest, SerializedResponseKeepsRequestedHeaderCasing)
{
  MQTTAllocator allocator("test_http_resp", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  http::HttpResponse response(&allocator);
  response.status_code = 101;
  response.reason.assign("Switching Protocols");
  response.set_header(mqtt::to_mqtt_string("Upgrade", &allocator),
                      mqtt::to_mqtt_string("websocket", &allocator));
  response.set_header(mqtt::to_mqtt_string("Connection", &allocator),
                      mqtt::to_mqtt_string("Upgrade", &allocator));
  response.set_header(mqtt::to_mqtt_string("Sec-WebSocket-Accept", &allocator),
                      mqtt::to_mqtt_string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", &allocator));

  const std::string serialized = mqtt::from_mqtt_string(response.serialize());
  EXPECT_NE(std::string::npos, serialized.find("101 Switching Protocols"));
  EXPECT_NE(std::string::npos, serialized.find("Upgrade: websocket\r\n"));
  EXPECT_NE(std::string::npos, serialized.find("Connection: Upgrade\r\n"));
  EXPECT_NE(std::string::npos,
            serialized.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"));
}

TEST(HttpParserTest, HeaderLookupStaysCaseInsensitiveAfterMixedCaseSet)
{
  MQTTAllocator allocator("test_http_dedup", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  http::HttpResponse response(&allocator);
  response.set_header(mqtt::to_mqtt_string("Upgrade", &allocator),
                      mqtt::to_mqtt_string("websocket", &allocator));
  response.set_header(mqtt::to_mqtt_string("UPGRADE", &allocator),
                      mqtt::to_mqtt_string("h2c", &allocator));

  // The two spellings must collapse into a single header.
  EXPECT_EQ(1u, response.headers.size());

  const std::string serialized = mqtt::from_mqtt_string(response.serialize());
  EXPECT_NE(std::string::npos, serialized.find("UPGRADE: h2c\r\n"));
}

}  // namespace

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
