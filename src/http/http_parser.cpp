#include "http_parser.h"

#include <cstdio>

extern "C" {
#include <llhttp.h>
}

namespace http {

static llhttp_type to_llhttp_type(HttpParserType type) {
  return type == HttpParserType::REQUEST ? HTTP_REQUEST : HTTP_RESPONSE;
}

static HttpParser* from_native(llhttp_t* parser) {
  return reinterpret_cast<HttpParser*>(parser->data);
}

HttpParser::HttpParser(MQTTAllocator* allocator, HttpParserType type)
    : allocator_(allocator),
      type_(type),
      request_(allocator),
      response_(allocator),
      parser_(nullptr),
      settings_(nullptr),
      pending_header_field_(mqtt::MQTTStrAllocator(allocator)),
      pending_header_value_(mqtt::MQTTStrAllocator(allocator)),
      parsing_header_value_(false),
      message_complete_(false),
      last_error_(mqtt::MQTTStrAllocator(allocator)) {
  parser_ = static_cast<llhttp_t*>(allocator_->allocate(sizeof(llhttp_t)));
  settings_ = static_cast<llhttp_settings_t*>(allocator_->allocate(sizeof(llhttp_settings_t)));

  llhttp_settings_init(settings_);
  settings_->on_message_begin = &HttpParser::on_message_begin;
  settings_->on_url = &HttpParser::on_url;
  settings_->on_status = &HttpParser::on_status;
  settings_->on_header_field = &HttpParser::on_header_field;
  settings_->on_header_value = &HttpParser::on_header_value;
  settings_->on_headers_complete = &HttpParser::on_headers_complete;
  settings_->on_body = &HttpParser::on_body;
  settings_->on_message_complete = &HttpParser::on_message_complete;

  llhttp_init(parser_, to_llhttp_type(type_), settings_);
  parser_->data = this;
}

HttpParser::~HttpParser() {
  if (settings_) {
    allocator_->deallocate(settings_, sizeof(llhttp_settings_t));
    settings_ = nullptr;
  }
  if (parser_) {
    allocator_->deallocate(parser_, sizeof(llhttp_t));
    parser_ = nullptr;
  }
}

void HttpParser::reset() {
  request_.reset();
  response_.reset();
  pending_header_field_.clear();
  pending_header_value_.clear();
  parsing_header_value_ = false;
  message_complete_ = false;
  last_error_.clear();

  llhttp_init(parser_, to_llhttp_type(type_), settings_);
  parser_->data = this;
}

HttpParseStatus HttpParser::execute(const mqtt::MQTTString& data, size_t& consumed) {
  return execute(data.data(), data.size(), consumed);
}

HttpParseStatus HttpParser::execute(const char* data, size_t len, size_t& consumed) {
  consumed = 0;
  if (message_complete_) {
    return HttpParseStatus::OK;
  }

  llhttp_errno_t err = llhttp_execute(parser_, data, len);
  if (err == HPE_OK) {
    consumed = len;
    return message_complete_ ? HttpParseStatus::OK : HttpParseStatus::INCOMPLETE;
  }

  if (err == HPE_PAUSED_UPGRADE || err == HPE_PAUSED) {
    consumed = static_cast<size_t>(llhttp_get_error_pos(parser_) - data);
    return HttpParseStatus::OK;
  }

  consumed = static_cast<size_t>(llhttp_get_error_pos(parser_) - data);
  const char* reason = llhttp_get_error_reason(parser_);
  if (reason != nullptr) {
    last_error_.assign(reason);
  } else {
    last_error_.assign(llhttp_errno_name(err));
  }
  return HttpParseStatus::ERROR;
}

int HttpParser::on_message_begin(llhttp_t* parser) {
  HttpParser* self = from_native(parser);
  self->request_.reset();
  self->response_.reset();
  self->pending_header_field_.clear();
  self->pending_header_value_.clear();
  self->parsing_header_value_ = false;
  self->message_complete_ = false;
  return 0;
}

int HttpParser::on_url(llhttp_t* parser, const char* at, size_t length) {
  return from_native(parser)->append_url(at, length);
}

int HttpParser::on_status(llhttp_t* parser, const char* at, size_t length) {
  return from_native(parser)->append_status(at, length);
}

int HttpParser::on_header_field(llhttp_t* parser, const char* at, size_t length) {
  return from_native(parser)->append_header_field(at, length);
}

int HttpParser::on_header_value(llhttp_t* parser, const char* at, size_t length) {
  return from_native(parser)->append_header_value(at, length);
}

int HttpParser::on_headers_complete(llhttp_t* parser) {
  return from_native(parser)->headers_complete();
}

int HttpParser::on_body(llhttp_t* parser, const char* at, size_t length) {
  return from_native(parser)->append_body(at, length);
}

int HttpParser::on_message_complete(llhttp_t* parser) {
  return from_native(parser)->mark_message_complete();
}

int HttpParser::append_url(const char* at, size_t length) {
  if (type_ != HttpParserType::REQUEST) {
    return 0;
  }
  request_.url.append(at, length);
  request_.path = request_.url;
  return 0;
}

int HttpParser::append_status(const char* at, size_t length) {
  if (type_ != HttpParserType::RESPONSE) {
    return 0;
  }
  response_.reason.append(at, length);
  return 0;
}

int HttpParser::append_header_field(const char* at, size_t length) {
  if (parsing_header_value_) {
    int ret = commit_pending_header();
    if (ret != 0) {
      return ret;
    }
  }
  pending_header_field_.append(at, length);
  parsing_header_value_ = false;
  return 0;
}

int HttpParser::append_header_value(const char* at, size_t length) {
  pending_header_value_.append(at, length);
  parsing_header_value_ = true;
  return 0;
}

int HttpParser::commit_pending_header() {
  if (pending_header_field_.empty()) {
    return 0;
  }

  mqtt::MQTTString key = to_lower_ascii(pending_header_field_, allocator_);
  mqtt::MQTTString value = pending_header_value_;

  if (type_ == HttpParserType::REQUEST) {
    request_.set_header(key, value);
  } else {
    response_.set_header(key, value);
  }

  pending_header_field_.clear();
  pending_header_value_.clear();
  parsing_header_value_ = false;
  return 0;
}

int HttpParser::headers_complete() {
  int ret = commit_pending_header();
  if (ret != 0) {
    return ret;
  }

  char version_buf[16] = {0};
  std::snprintf(version_buf, sizeof(version_buf), "HTTP/%u.%u", parser_->http_major, parser_->http_minor);
  if (type_ == HttpParserType::REQUEST) {
    request_.version.assign(version_buf);
    request_.method.assign(llhttp_method_name(static_cast<llhttp_method_t>(parser_->method)));
  } else {
    response_.version.assign(version_buf);
    response_.status_code = parser_->status_code;
  }
  return 0;
}

int HttpParser::append_body(const char* at, size_t length) {
  if (length == 0) {
    return 0;
  }

  if (type_ == HttpParserType::REQUEST) {
    (void)request_.body.append(at, length);
  } else {
    (void)response_.body.append(at, length);
  }
  return 0;
}

int HttpParser::mark_message_complete() {
  message_complete_ = true;
  return 0;
}

}  // namespace http
