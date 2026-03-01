#include "mqtt_string_utils.h"

namespace mqtt {

// UTF-8 validation according to RFC 3629
int is_valid_utf8(const uint8_t* data, size_t length)
{
  int ret = MQ_STRING_SUCCESS;

  if (data == nullptr && length > 0) {
    ret = MQ_STRING_ERR_INVALID_LENGTH;
  } else if (data != nullptr && length > 0) {
    const uint8_t* current = data;
    const uint8_t* end = data + length;

    while (current < end && ret == MQ_STRING_SUCCESS) {
      uint8_t byte = *current++;

      // ASCII character (0xxxxxxx)
      if (byte < 0x80) {
        continue;
      }

      // Multi-byte UTF-8 sequence
      int continuation_bytes = 0;
      uint32_t code_point = 0;

      if ((byte & 0xE0) == 0xC0) {
        // 2-byte sequence (110xxxxx 10xxxxxx)
        continuation_bytes = 1;
        code_point = byte & 0x1F;
      } else if ((byte & 0xF0) == 0xE0) {
        // 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx)
        continuation_bytes = 2;
        code_point = byte & 0x0F;
      } else if ((byte & 0xF8) == 0xF0) {
        // 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
        continuation_bytes = 3;
        code_point = byte & 0x07;
      } else {
        // Invalid start byte
        ret = MQ_STRING_ERR_INVALID_UTF8;
      }

      // Check if we have enough bytes for the sequence
      if (ret == MQ_STRING_SUCCESS && current + continuation_bytes > end) {
        ret = MQ_STRING_ERR_INVALID_UTF8;
      }

      // Process continuation bytes
      int i = 0;
      while (i < continuation_bytes && ret == MQ_STRING_SUCCESS) {
        byte = *current++;
        if ((byte & 0xC0) != 0x80) {
          // Invalid continuation byte
          ret = MQ_STRING_ERR_INVALID_UTF8;
        } else {
          code_point = (code_point << 6) | (byte & 0x3F);
        }
        ++i;
      }

      // Check for overlong encodings and invalid code points
      if (ret == MQ_STRING_SUCCESS && continuation_bytes == 1 && code_point < 0x80) {
        ret = MQ_STRING_ERR_INVALID_UTF8;  // Overlong 2-byte sequence
      }
      if (ret == MQ_STRING_SUCCESS && continuation_bytes == 2 && code_point < 0x800) {
        ret = MQ_STRING_ERR_INVALID_UTF8;  // Overlong 3-byte sequence
      }
      if (ret == MQ_STRING_SUCCESS && continuation_bytes == 3 && code_point < 0x10000) {
        ret = MQ_STRING_ERR_INVALID_UTF8;  // Overlong 4-byte sequence
      }

      // Check for surrogate pairs (U+D800 to U+DFFF) and invalid code points
      if (ret == MQ_STRING_SUCCESS &&
          ((code_point >= 0xD800 && code_point <= 0xDFFF) || code_point > 0x10FFFF)) {
        ret = MQ_STRING_ERR_INVALID_UTF8;
      }
    }
  }

  return ret;
}

// Check for control characters according to MQTT v5.0 specification
int has_control_characters(const uint8_t* data, size_t length)
{
  int ret = MQ_STRING_SUCCESS;

  if (data == nullptr && length > 0) {
    ret = MQ_STRING_ERR_INVALID_LENGTH;
  } else if (data != nullptr && length > 0) {
    for (size_t i = 0; i < length && ret == MQ_STRING_SUCCESS; i++) {
      uint8_t byte = data[i];

      // Check for C0 control characters (U+0000 to U+001F)
      if (byte <= 0x1F) {
        ret = MQ_STRING_ERR_INVALID_CONTROL_CHARS;
      }

      // Check for DEL character (U+007F)
      if (ret == MQ_STRING_SUCCESS && byte == 0x7F) {
        ret = MQ_STRING_ERR_INVALID_CONTROL_CHARS;
      }

      // Check for C1 control characters (U+0080 to U+009F)
      // These are encoded as 2-byte sequences in UTF-8
      if (ret == MQ_STRING_SUCCESS && byte == 0xC2 && i + 1 < length) {
        uint8_t next_byte = data[i + 1];
        if (next_byte >= 0x80 && next_byte <= 0x9F) {
          ret = MQ_STRING_ERR_INVALID_CONTROL_CHARS;
        }
      }
    }
  }

  return ret;
}

// Validate MQTT string according to MQTT v5.0 specification
int validate_mqtt_string(const uint8_t* data, size_t length)
{
  int ret = MQ_STRING_SUCCESS;

  // Check for null pointer
  if (data == nullptr && length > 0) {
    ret = MQ_STRING_ERR_INVALID_LENGTH;
  }

  // Check length limit
  if (ret == MQ_STRING_SUCCESS && length > MQTT_MAX_STRING_LENGTH) {
    ret = MQ_STRING_ERR_STRING_TOO_LONG;
  }

  // Validate UTF-8 encoding
  if (ret == MQ_STRING_SUCCESS && length > 0) {
    ret = is_valid_utf8(data, length);
  }

  // Check for control characters
  if (ret == MQ_STRING_SUCCESS && length > 0) {
    ret = has_control_characters(data, length);
  }

  return ret;
}

}  // namespace mqtt
