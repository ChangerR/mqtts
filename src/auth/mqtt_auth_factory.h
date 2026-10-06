#pragma once
#include "mqtt_auth_interface.h"
#include "mqtt_config.h"

namespace mqtt {
namespace auth {
int configure_auth(const mqtt::AuthConfig& config, MQTTAllocator* allocator,
                   std::unique_ptr<AuthManager>& manager);
}
}
