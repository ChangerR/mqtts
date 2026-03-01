#include <cassert>
#include <iostream>

#include "http_parser.h"

static void test_parse_request() {
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
  assert(status == http::HttpParseStatus::OK);
  assert(parser.message_complete());

  const http::HttpRequest& req = parser.request();
  assert(req.method == mqtt::to_mqtt_string("GET", &allocator));
  assert(req.get_header(mqtt::to_mqtt_string("upgrade", &allocator)) ==
         mqtt::to_mqtt_string("websocket", &allocator));
  assert(req.get_header(mqtt::to_mqtt_string("sec-websocket-key", &allocator)) ==
         mqtt::to_mqtt_string("abc123==", &allocator));
}

static void test_serialize_response() {
  MQTTAllocator allocator("test_http_resp", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  http::HttpResponse response(&allocator);
  response.status_code = 101;
  response.reason.assign("Switching Protocols");
  response.set_header(mqtt::to_mqtt_string("Upgrade", &allocator),
                      mqtt::to_mqtt_string("websocket", &allocator));
  response.set_header(mqtt::to_mqtt_string("Connection", &allocator),
                      mqtt::to_mqtt_string("Upgrade", &allocator));

  mqtt::MQTTString serialized = response.serialize();
  std::string serialized_std = mqtt::from_mqtt_string(serialized);
  assert(serialized_std.find("101 Switching Protocols") != std::string::npos);
  assert(serialized_std.find("upgrade: websocket") != std::string::npos);
}

int main() {
  std::cout << "Running HTTP parser tests..." << std::endl;
  test_parse_request();
  test_serialize_response();
  std::cout << "HTTP parser tests passed" << std::endl;
  return 0;
}
