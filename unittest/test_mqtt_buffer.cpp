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

  mqtt::MQTTString out(mqtt::MQTTStrAllocator(&allocator));
  assert(restored.get_string(5, out));
  assert(out == mqtt::to_mqtt_string("hello", &allocator));
}

int main() {
  std::cout << "Running MQTTBuffer tests..." << std::endl;
  test_endian_rw();
  test_string_and_serialize();
  std::cout << "MQTTBuffer tests passed" << std::endl;
  return 0;
}
