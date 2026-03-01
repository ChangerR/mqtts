#pragma once

#include <cstdint>

#include "mqtt_allocator.h"
#include "mqtt_stl_allocator.h"
#include "mqtt_buffer.h"

namespace http {

using HeaderMap = mqtt::MQTTMap<mqtt::MQTTString, mqtt::MQTTString>;

struct HttpRequest {
  explicit HttpRequest(MQTTAllocator* allocator);

  MQTTAllocator* allocator;
  mqtt::MQTTString method;
  mqtt::MQTTString url;
  mqtt::MQTTString path;
  mqtt::MQTTString version;
  HeaderMap headers;
  mqtt::MQTTBuffer body;

  void reset();
  bool has_header(const mqtt::MQTTString& key) const;
  mqtt::MQTTString get_header(const mqtt::MQTTString& key) const;
  void set_header(const mqtt::MQTTString& key, const mqtt::MQTTString& value);
};

struct HttpResponse {
  explicit HttpResponse(MQTTAllocator* allocator);

  MQTTAllocator* allocator;
  int status_code;
  mqtt::MQTTString reason;
  mqtt::MQTTString version;
  HeaderMap headers;
  mqtt::MQTTBuffer body;

  void reset();
  void set_header(const mqtt::MQTTString& key, const mqtt::MQTTString& value);
  mqtt::MQTTString serialize() const;
};

mqtt::MQTTString to_lower_ascii(const mqtt::MQTTString& value, MQTTAllocator* allocator);

}  // namespace http
