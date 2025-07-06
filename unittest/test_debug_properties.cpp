#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include "src/mqtt_allocator.h"
#include "src/mqtt_parser.h"
#include "src/mqtt_serialize_buffer.h"

int main()
{
    std::cout << "Testing empty properties serialization..." << std::endl;
    
    MQTTAllocator allocator("test_client", MQTTMemoryTag::MEM_TAG_CLIENT, 0);
    mqtt::MQTTParser parser(&allocator);
    
    // Create empty properties
    mqtt::Properties properties(&allocator);
    
    // Try to serialize properties
    mqtt::MQTTSerializeBuffer properties_buffer(&allocator);
    int ret = parser.serialize_properties(properties, properties_buffer);
    
    std::cout << "serialize_properties returned: " << ret << std::endl;
    std::cout << "Properties buffer size: " << properties_buffer.size() << std::endl;
    
    if (ret == 0 && properties_buffer.size() > 0) {
        std::cout << "Properties buffer contents:";
        for (size_t i = 0; i < properties_buffer.size(); ++i) {
            std::cout << " 0x" << std::hex << static_cast<int>(properties_buffer.data()[i]);
        }
        std::cout << std::endl;
    }
    
    std::cout << "Empty properties serialization test completed" << std::endl;
    return 0;
}