#pragma once
#include "mqtt_auth_remote.h"
namespace mqtt { namespace auth {
class HttpAuthProvider : public RemoteAuthProvider {
public:
  explicit HttpAuthProvider(const std::map<std::string, std::string>& settings)
      : RemoteAuthProvider(settings, false) {}
};
} }
