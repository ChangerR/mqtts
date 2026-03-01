#ifndef MQTT_STRING_UTILS_H
#define MQTT_STRING_UTILS_H

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>
#include "mqtt_buffer.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {

// MQTT string length limits according to MQTT v5.0 specification
const uint16_t MQTT_MAX_STRING_LENGTH = 65535;
const uint16_t MQTT_MAX_BINARY_LENGTH = 65535;

// Error codes for string operations
const int MQ_STRING_SUCCESS = 0;
const int MQ_STRING_ERR_INVALID_LENGTH = -1;
const int MQ_STRING_ERR_INVALID_UTF8 = -2;
const int MQ_STRING_ERR_BUFFER_TOO_SHORT = -3;
const int MQ_STRING_ERR_STRING_TOO_LONG = -4;
const int MQ_STRING_ERR_INVALID_CONTROL_CHARS = -5;

// UTF-8 validation function
int is_valid_utf8(const uint8_t* data, size_t length);

// Check for control characters (U+0000 to U+001F and U+007F to U+009F)
int has_control_characters(const uint8_t* data, size_t length);

// Validate MQTT string according to MQTT v5.0 specification
int validate_mqtt_string(const uint8_t* data, size_t length);

// Template-based unified string parsing
template<typename StringType>
int parse_string_impl(const uint8_t* buffer, size_t length, StringType& str, size_t& bytes_read)
{
  int ret = MQ_STRING_SUCCESS;
  uint16_t str_length = 0;
  bytes_read = 0;

  if (length < 2) {
    ret = MQ_STRING_ERR_BUFFER_TOO_SHORT;
  } else {
    // Parse string length (big-endian)
    str_length = (buffer[0] << 8) | buffer[1];

    if (length < static_cast<size_t>(2 + str_length)) {
      ret = MQ_STRING_ERR_BUFFER_TOO_SHORT;
    } else {
      // Validate UTF-8 for string types (not binary data)
      if (std::is_same<StringType, std::string>::value ||
          std::is_same<StringType, MQTTString>::value) {
        ret = validate_mqtt_string(buffer + 2, str_length);
      }

      if (ret == MQ_STRING_SUCCESS) {
        // Assign data to string
        str.assign(reinterpret_cast<const char*>(buffer + 2), str_length);
        bytes_read = 2 + str_length;
      }
    }
  }

  return ret;
}

// Template-based unified binary data parsing
template<typename VectorType>
int parse_binary_data_impl(const uint8_t* buffer, size_t length, VectorType& data,
                           size_t& bytes_read)
{
  int ret = MQ_STRING_SUCCESS;
  uint16_t data_length = 0;
  bytes_read = 0;

  if (length < 2) {
    ret = MQ_STRING_ERR_BUFFER_TOO_SHORT;
  } else {
    // Parse data length (big-endian)
    data_length = (buffer[0] << 8) | buffer[1];

    if (length < static_cast<size_t>(2 + data_length)) {
      ret = MQ_STRING_ERR_BUFFER_TOO_SHORT;
    } else {
      // Assign data to vector
      data.assign(buffer + 2, buffer + 2 + data_length);
      bytes_read = 2 + data_length;
    }
  }

  return ret;
}

// Template-based unified string serialization
template<typename StringType>
int serialize_string_impl(const StringType& str, MQTTBuffer& buffer)
{
  int ret = MQ_STRING_SUCCESS;
  uint16_t length = 0;

  // Check string length limit
  if (str.length() > MQTT_MAX_STRING_LENGTH) {
    ret = MQ_STRING_ERR_STRING_TOO_LONG;
  } else {
    // Validate UTF-8 for string types (not binary data)
    if (std::is_same<StringType, std::string>::value ||
        std::is_same<StringType, MQTTString>::value) {
      ret = validate_mqtt_string(reinterpret_cast<const uint8_t*>(str.data()), str.length());
    }

    if (ret == MQ_STRING_SUCCESS) {
      // Serialize length (big-endian)
      length = static_cast<uint16_t>(str.length());
      ret = buffer.push_back((length >> 8) & 0xFF);
    }
    if (ret == MQ_STRING_SUCCESS) {
      ret = buffer.push_back(length & 0xFF);
    }
    if (ret == MQ_STRING_SUCCESS) {
      // Serialize data
      ret = buffer.append(str.data(), str.length());
    }
  }

  return ret;
}

// Template-based unified binary data serialization
template<typename VectorType>
int serialize_binary_data_impl(const VectorType& data, MQTTBuffer& buffer)
{
  int ret = MQ_STRING_SUCCESS;
  uint16_t length = 0;

  // Check data length limit
  if (data.size() > MQTT_MAX_BINARY_LENGTH) {
    ret = MQ_STRING_ERR_STRING_TOO_LONG;
  } else {
    // Serialize length (big-endian)
    length = static_cast<uint16_t>(data.size());
    ret = buffer.push_back((length >> 8) & 0xFF);
    if (ret == MQ_STRING_SUCCESS) {
      ret = buffer.push_back(length & 0xFF);
    }
    if (ret == MQ_STRING_SUCCESS) {
      // Serialize data
      ret = buffer.append(data.data(), data.size());
    }
  }

  return ret;
}

// Convenience functions for specific types
inline int parse_mqtt_string(const uint8_t* buffer, size_t length, MQTTString& str,
                             size_t& bytes_read)
{
  return parse_string_impl(buffer, length, str, bytes_read);
}

inline int parse_string(const uint8_t* buffer, size_t length, std::string& str,
                        size_t& bytes_read)
{
  return parse_string_impl(buffer, length, str, bytes_read);
}

inline int parse_mqtt_binary_data(const uint8_t* buffer, size_t length, MQTTByteVector& data,
                                  size_t& bytes_read)
{
  return parse_binary_data_impl(buffer, length, data, bytes_read);
}

inline int parse_binary_data(const uint8_t* buffer, size_t length, std::vector<uint8_t>& data,
                             size_t& bytes_read)
{
  return parse_binary_data_impl(buffer, length, data, bytes_read);
}

inline int serialize_mqtt_string(const MQTTString& str, MQTTBuffer& buffer)
{
  return serialize_string_impl(str, buffer);
}

inline int serialize_string(const std::string& str, MQTTBuffer& buffer)
{
  return serialize_string_impl(str, buffer);
}

inline int serialize_mqtt_binary_data(const MQTTByteVector& data, MQTTBuffer& buffer)
{
  return serialize_binary_data_impl(data, buffer);
}

inline int serialize_binary_data(const std::vector<uint8_t>& data, MQTTBuffer& buffer)
{
  return serialize_binary_data_impl(data, buffer);
}

}  // namespace mqtt

#endif  // MQTT_STRING_UTILS_H
