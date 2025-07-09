#ifndef MQTT_STRING_UTILS_H
#define MQTT_STRING_UTILS_H

#include <cstdint>
#include <string>
#include <vector>
#include <type_traits>
#include "mqtt_stl_allocator.h"
#include "mqtt_serialize_buffer.h"

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
bool is_valid_utf8(const uint8_t* data, size_t length);

// Check for control characters (U+0000 to U+001F and U+007F to U+009F)
bool has_control_characters(const uint8_t* data, size_t length);

// Validate MQTT string according to MQTT v5.0 specification
int validate_mqtt_string(const uint8_t* data, size_t length);

// Template-based unified string parsing
template<typename StringType>
int parse_string_impl(const uint8_t* buffer, size_t length, StringType& str, size_t& bytes_read)
{
    if (length < 2) {
        return MQ_STRING_ERR_BUFFER_TOO_SHORT;
    }

    // Parse string length (big-endian)
    uint16_t str_length = (buffer[0] << 8) | buffer[1];
    
    if (length < 2 + str_length) {
        return MQ_STRING_ERR_BUFFER_TOO_SHORT;
    }

    // Validate UTF-8 for string types (not binary data)
    if (std::is_same<StringType, std::string>::value || std::is_same<StringType, MQTTString>::value) {
        int validation_result = validate_mqtt_string(buffer + 2, str_length);
        if (validation_result != MQ_STRING_SUCCESS) {
            return validation_result;
        }
    }

    // Assign data to string
    str.assign(reinterpret_cast<const char*>(buffer + 2), str_length);
    bytes_read = 2 + str_length;
    return MQ_STRING_SUCCESS;
}

// Template-based unified binary data parsing
template<typename VectorType>
int parse_binary_data_impl(const uint8_t* buffer, size_t length, VectorType& data, size_t& bytes_read)
{
    if (length < 2) {
        return MQ_STRING_ERR_BUFFER_TOO_SHORT;
    }

    // Parse data length (big-endian)
    uint16_t data_length = (buffer[0] << 8) | buffer[1];
    
    if (length < 2 + data_length) {
        return MQ_STRING_ERR_BUFFER_TOO_SHORT;
    }

    // Assign data to vector
    data.assign(buffer + 2, buffer + 2 + data_length);
    bytes_read = 2 + data_length;
    return MQ_STRING_SUCCESS;
}

// Template-based unified string serialization
template<typename StringType>
int serialize_string_impl(const StringType& str, MQTTSerializeBuffer& buffer)
{
    // Check string length limit
    if (str.length() > MQTT_MAX_STRING_LENGTH) {
        return MQ_STRING_ERR_STRING_TOO_LONG;
    }

    // Validate UTF-8 for string types (not binary data)
    if (std::is_same<StringType, std::string>::value || std::is_same<StringType, MQTTString>::value) {
        int validation_result = validate_mqtt_string(
            reinterpret_cast<const uint8_t*>(str.data()), str.length());
        if (validation_result != MQ_STRING_SUCCESS) {
            return validation_result;
        }
    }

    // Serialize length (big-endian)
    uint16_t length = static_cast<uint16_t>(str.length());
    int ret = buffer.push_back((length >> 8) & 0xFF);
    if (ret != 0) return ret;
    
    ret = buffer.push_back(length & 0xFF);
    if (ret != 0) return ret;

    // Serialize data
    ret = buffer.append(str.data(), str.length());
    if (ret != 0) return ret;

    return MQ_STRING_SUCCESS;
}

// Template-based unified binary data serialization
template<typename VectorType>
int serialize_binary_data_impl(const VectorType& data, MQTTSerializeBuffer& buffer)
{
    // Check data length limit
    if (data.size() > MQTT_MAX_BINARY_LENGTH) {
        return MQ_STRING_ERR_STRING_TOO_LONG;
    }

    // Serialize length (big-endian)
    uint16_t length = static_cast<uint16_t>(data.size());
    int ret = buffer.push_back((length >> 8) & 0xFF);
    if (ret != 0) return ret;
    
    ret = buffer.push_back(length & 0xFF);
    if (ret != 0) return ret;

    // Serialize data
    ret = buffer.append(data.data(), data.size());
    if (ret != 0) return ret;

    return MQ_STRING_SUCCESS;
}

// Convenience functions for specific types
inline int parse_mqtt_string(const uint8_t* buffer, size_t length, MQTTString& str, size_t& bytes_read) {
    return parse_string_impl(buffer, length, str, bytes_read);
}

inline int parse_string(const uint8_t* buffer, size_t length, std::string& str, size_t& bytes_read) {
    return parse_string_impl(buffer, length, str, bytes_read);
}

inline int parse_mqtt_binary_data(const uint8_t* buffer, size_t length, MQTTByteVector& data, size_t& bytes_read) {
    return parse_binary_data_impl(buffer, length, data, bytes_read);
}

inline int parse_binary_data(const uint8_t* buffer, size_t length, std::vector<uint8_t>& data, size_t& bytes_read) {
    return parse_binary_data_impl(buffer, length, data, bytes_read);
}

inline int serialize_mqtt_string(const MQTTString& str, MQTTSerializeBuffer& buffer) {
    return serialize_string_impl(str, buffer);
}

inline int serialize_string(const std::string& str, MQTTSerializeBuffer& buffer) {
    return serialize_string_impl(str, buffer);
}

inline int serialize_mqtt_binary_data(const MQTTByteVector& data, MQTTSerializeBuffer& buffer) {
    return serialize_binary_data_impl(data, buffer);
}

inline int serialize_binary_data(const std::vector<uint8_t>& data, MQTTSerializeBuffer& buffer) {
    return serialize_binary_data_impl(data, buffer);
}

}  // namespace mqtt

#endif  // MQTT_STRING_UTILS_H