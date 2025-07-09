#include "mqtt_string_utils.h"
#include <cassert>

namespace mqtt {

// UTF-8 validation according to RFC 3629
bool is_valid_utf8(const uint8_t* data, size_t length)
{
    if (!data || length == 0) {
        return true;
    }

    const uint8_t* end = data + length;
    
    while (data < end) {
        uint8_t byte = *data++;
        
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
            return false;
        }
        
        // Check if we have enough bytes for the sequence
        if (data + continuation_bytes > end) {
            return false;
        }
        
        // Process continuation bytes
        for (int i = 0; i < continuation_bytes; i++) {
            byte = *data++;
            if ((byte & 0xC0) != 0x80) {
                // Invalid continuation byte
                return false;
            }
            code_point = (code_point << 6) | (byte & 0x3F);
        }
        
        // Check for overlong encodings and invalid code points
        if (continuation_bytes == 1 && code_point < 0x80) {
            return false;  // Overlong 2-byte sequence
        }
        if (continuation_bytes == 2 && code_point < 0x800) {
            return false;  // Overlong 3-byte sequence
        }
        if (continuation_bytes == 3 && code_point < 0x10000) {
            return false;  // Overlong 4-byte sequence
        }
        
        // Check for surrogate pairs (U+D800 to U+DFFF) and invalid code points
        if ((code_point >= 0xD800 && code_point <= 0xDFFF) || code_point > 0x10FFFF) {
            return false;
        }
    }
    
    return true;
}

// Check for control characters according to MQTT v5.0 specification
bool has_control_characters(const uint8_t* data, size_t length)
{
    if (!data || length == 0) {
        return false;
    }

    for (size_t i = 0; i < length; i++) {
        uint8_t byte = data[i];
        
        // Check for C0 control characters (U+0000 to U+001F)
        if (byte <= 0x1F) {
            return true;
        }
        
        // Check for DEL character (U+007F)
        if (byte == 0x7F) {
            return true;
        }
        
        // Check for C1 control characters (U+0080 to U+009F)
        // These are encoded as 2-byte sequences in UTF-8
        if (byte == 0xC2 && i + 1 < length) {
            uint8_t next_byte = data[i + 1];
            if (next_byte >= 0x80 && next_byte <= 0x9F) {
                return true;
            }
        }
    }
    
    return false;
}

// Validate MQTT string according to MQTT v5.0 specification
int validate_mqtt_string(const uint8_t* data, size_t length)
{
    // Check for null pointer
    if (!data && length > 0) {
        return MQ_STRING_ERR_INVALID_LENGTH;
    }
    
    // Check length limit
    if (length > MQTT_MAX_STRING_LENGTH) {
        return MQ_STRING_ERR_STRING_TOO_LONG;
    }
    
    // Empty string is valid
    if (length == 0) {
        return MQ_STRING_SUCCESS;
    }
    
    // Validate UTF-8 encoding
    if (!is_valid_utf8(data, length)) {
        return MQ_STRING_ERR_INVALID_UTF8;
    }
    
    // Check for control characters
    if (has_control_characters(data, length)) {
        return MQ_STRING_ERR_INVALID_CONTROL_CHARS;
    }
    
    return MQ_STRING_SUCCESS;
}

}  // namespace mqtt