#pragma once
#include "mqtt_auth_remote.h"
namespace mqtt { namespace auth {
class GrpcAuthProvider : public RemoteAuthProvider {
public:
  explicit GrpcAuthProvider(const std::map<std::string, std::string>& settings)
      : RemoteAuthProvider(settings, true) {}
};
} }
