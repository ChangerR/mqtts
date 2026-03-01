#pragma once

#include "http_message.h"
#include "mqtt_define.h"

extern "C" {
#include <llhttp.h>
}

namespace http {

enum class HttpParserType { REQUEST, RESPONSE };

enum class HttpParseStatus { OK, INCOMPLETE, ERROR };

class HttpParser {
 public:
  HttpParser(MQTTAllocator* allocator, HttpParserType type);
  ~HttpParser();

  HttpParseStatus execute(const char* data, size_t len, size_t& consumed);
  HttpParseStatus execute(const mqtt::MQTTString& data, size_t& consumed);

  bool message_complete() const { return message_complete_; }
  const HttpRequest& request() const { return request_; }
  const HttpResponse& response() const { return response_; }

  const mqtt::MQTTString& last_error() const { return last_error_; }

  void reset();

 private:
  static int on_message_begin(llhttp_t* parser);
  static int on_url(llhttp_t* parser, const char* at, size_t length);
  static int on_status(llhttp_t* parser, const char* at, size_t length);
  static int on_header_field(llhttp_t* parser, const char* at, size_t length);
  static int on_header_value(llhttp_t* parser, const char* at, size_t length);
  static int on_headers_complete(llhttp_t* parser);
  static int on_body(llhttp_t* parser, const char* at, size_t length);
  static int on_message_complete(llhttp_t* parser);

  int append_url(const char* at, size_t length);
  int append_status(const char* at, size_t length);
  int append_header_field(const char* at, size_t length);
  int append_header_value(const char* at, size_t length);
  int commit_pending_header();
  int headers_complete();
  int append_body(const char* at, size_t length);
  int mark_message_complete();

  MQTTAllocator* allocator_;
  HttpParserType type_;
  HttpRequest request_;
  HttpResponse response_;

  llhttp_t* parser_;
  llhttp_settings_t* settings_;

  mqtt::MQTTString pending_header_field_;
  mqtt::MQTTString pending_header_value_;
  bool parsing_header_value_;
  bool message_complete_;
  mqtt::MQTTString last_error_;
};

}  // namespace http
