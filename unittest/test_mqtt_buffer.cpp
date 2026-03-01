#include <cassert>
#include <iostream>

#include "mqtt_buffer.h"

static void test_endian_rw() {
  MQTTAllocator allocator("test_buffer", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::MQTTBuffer buffer(&allocator);

  assert(buffer.put_uint16_be(0x1234));
  assert(buffer.put_uint16_le(0x5678));
  assert(buffer.put_uint32_be(0x01020304));
  assert(buffer.put_uint32_le(0xA0B0C0D0));
  assert(buffer.put_uint64_be(0x0102030405060708ULL));
  assert(buffer.put_uint64_le(0x1112131415161718ULL));

  buffer.reset_read_pos();

  uint16_t v16 = 0;
  uint32_t v32 = 0;
  uint64_t v64 = 0;

  assert(buffer.get_uint16_be(v16) && v16 == 0x1234);
  assert(buffer.get_uint16_le(v16) && v16 == 0x5678);
  assert(buffer.get_uint32_be(v32) && v32 == 0x01020304);
  assert(buffer.get_uint32_le(v32) && v32 == 0xA0B0C0D0);
  assert(buffer.get_uint64_be(v64) && v64 == 0x0102030405060708ULL);
  assert(buffer.get_uint64_le(v64) && v64 == 0x1112131415161718ULL);
}

static void test_string_and_serialize() {
  MQTTAllocator allocator("test_buffer_str", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::MQTTBuffer buffer(&allocator);

  const char* text = "hello";
  assert(buffer.append(text, 5));

  mqtt::MQTTString serialized = buffer.serialize();
  assert(serialized == mqtt::to_mqtt_string("hello", &allocator));

  mqtt::MQTTBuffer restored(&allocator);
  assert(restored.deserialize(serialized));

  mqtt::MQTTString out{mqtt::MQTTStrAllocator(&allocator)};
  assert(restored.get_string(5, out));
  assert(out == mqtt::to_mqtt_string("hello", &allocator));
}

static void test_text_and_decimal_append() {
  MQTTAllocator allocator("test_buffer_text", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::MQTTBuffer buffer(&allocator);

  assert(buffer.append_cstr("HTTP/1.1 "));
  assert(buffer.append_decimal_int(200));
  assert(buffer.append_byte(static_cast<uint8_t>(' ')));
  assert(buffer.append_string(mqtt::to_mqtt_string("OK", &allocator)));
  assert(buffer.append_cstr("\r\n"));

  assert(buffer.serialize() == mqtt::to_mqtt_string("HTTP/1.1 200 OK\r\n", &allocator));

  buffer.clear();
  assert(buffer.append_decimal_int(-12345));
  assert(buffer.serialize() == mqtt::to_mqtt_string("-12345", &allocator));
}

static void test_serialize_empty_buffer() {
  MQTTAllocator allocator("test_buffer_empty", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::MQTTBuffer buffer(&allocator);

  mqtt::MQTTString serialized = buffer.serialize();
  assert(serialized.empty());
}

static void test_resize_and_tail_write() {
  MQTTAllocator allocator("test_buffer_resize", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
  mqtt::MQTTBuffer buffer(&allocator);

  assert(buffer.append_cstr("ABCDE"));
  assert(buffer.size() == 5);

  assert(buffer.resize(3));
  assert(buffer.size() == 3);
  assert(buffer.serialize() == mqtt::to_mqtt_string("ABC", &allocator));

  assert(buffer.resize(8));
  assert(buffer.size() == 8);
}

int main() {
  std::cout << "Running MQTTBuffer tests..." << std::endl;
  test_endian_rw();
  test_string_and_serialize();
  test_text_and_decimal_append();
  test_serialize_empty_buffer();
  test_resize_and_tail_write();
  std::cout << "MQTTBuffer tests passed" << std::endl;
  return 0;
}
