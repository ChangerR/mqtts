#include "http_message.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace http {

mqtt::MQTTString to_lower_ascii(const mqtt::MQTTString& value, MQTTAllocator* allocator) {
  mqtt::MQTTString lower(value, mqtt::MQTTStrAllocator(allocator));
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lower;
}

HttpRequest::HttpRequest(MQTTAllocator* in_allocator)
    : allocator(in_allocator),
      method(mqtt::MQTTStrAllocator(in_allocator)),
      url(mqtt::MQTTStrAllocator(in_allocator)),
      path(mqtt::MQTTStrAllocator(in_allocator)),
      version(mqtt::MQTTStrAllocator(in_allocator)),
      headers(0, std::hash<mqtt::MQTTString>(), std::equal_to<mqtt::MQTTString>(),
              mqtt::MQTTSTLAllocator<std::pair<const mqtt::MQTTString, mqtt::MQTTString>>(in_allocator)),
      body(in_allocator) {}

void HttpRequest::reset() {
  method.clear();
  url.clear();
  path.clear();
  version.clear();
  headers.clear();
  body.clear();
}

bool HttpRequest::has_header(const mqtt::MQTTString& key) const {
  mqtt::MQTTString key_lower = to_lower_ascii(key, allocator);
  return headers.find(key_lower) != headers.end();
}

mqtt::MQTTString HttpRequest::get_header(const mqtt::MQTTString& key) const {
  mqtt::MQTTString key_lower = to_lower_ascii(key, allocator);
  HeaderMap::const_iterator iter = headers.find(key_lower);
  if (iter == headers.end()) {
    return mqtt::MQTTString(mqtt::MQTTStrAllocator(allocator));
  }
  return iter->second;
}

void HttpRequest::set_header(const mqtt::MQTTString& key, const mqtt::MQTTString& value) {
  mqtt::MQTTString key_lower = to_lower_ascii(key, allocator);
  headers[key_lower] = value;
}

HttpResponse::HttpResponse(MQTTAllocator* in_allocator)
    : allocator(in_allocator),
      status_code(200),
      reason(mqtt::MQTTStrAllocator(in_allocator)),
      version("HTTP/1.1", mqtt::MQTTStrAllocator(in_allocator)),
      headers(0, std::hash<mqtt::MQTTString>(), std::equal_to<mqtt::MQTTString>(),
              mqtt::MQTTSTLAllocator<std::pair<const mqtt::MQTTString, mqtt::MQTTString>>(in_allocator)),
      body(in_allocator) {
  reason.assign("OK");
}

void HttpResponse::reset() {
  status_code = 200;
  reason.assign("OK");
  version.assign("HTTP/1.1");
  headers.clear();
  body.clear();
}

void HttpResponse::set_header(const mqtt::MQTTString& key, const mqtt::MQTTString& value) {
  mqtt::MQTTString key_lower = to_lower_ascii(key, allocator);
  headers[key_lower] = value;
}

mqtt::MQTTString HttpResponse::serialize() const {
  mqtt::MQTTBuffer buffer(allocator);

  size_t estimated_size = version.size() + reason.size() + body.size() + 32;
  for (HeaderMap::const_iterator it = headers.begin(); it != headers.end(); ++it) {
    estimated_size += it->first.size() + it->second.size() + 4;
  }
  (void)buffer.reserve(estimated_size);

  const auto append_bytes = [&buffer](const char* data, size_t len) {
    (void)buffer.append(data, len);
  };

  append_bytes(version.data(), version.size());
  append_bytes(" ", 1);

  char status_code_buf[16] = {0};
  int status_code_len = std::snprintf(status_code_buf, sizeof(status_code_buf), "%d", status_code);
  if (status_code_len > 0) {
    append_bytes(status_code_buf, static_cast<size_t>(status_code_len));
  }

  append_bytes(" ", 1);
  append_bytes(reason.data(), reason.size());
  append_bytes("\r\n", 2);

  for (HeaderMap::const_iterator it = headers.begin(); it != headers.end(); ++it) {
    append_bytes(it->first.data(), it->first.size());
    append_bytes(": ", 2);
    append_bytes(it->second.data(), it->second.size());
    append_bytes("\r\n", 2);
  }

  append_bytes("\r\n", 2);
  if (!body.empty()) {
    (void)buffer.append(body.data(), body.size());
  }

  return buffer.serialize();
}

}  // namespace http
