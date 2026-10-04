#pragma once

#include "mqtt_auth_interface.h"
#include <map>

namespace mqtt {
namespace auth {

// Generic policy client. Optional authorization leases stay local; bounded RPC/HTTP
// workers handle misses and refreshes without blocking the MQTT event loop.
class RemoteAuthProvider : public IAuthProvider {
public:
  RemoteAuthProvider(const std::map<std::string, std::string>& settings, bool grpc);
  ~RemoteAuthProvider() override;
  int initialize() override;
  void cleanup() override;
  AuthResult authenticate_user(const MQTTString&, const MQTTString&, const MQTTString&,
                               const MQTTString&, uint16_t, UserInfo&) override;
  AuthResult check_topic_access(const UserInfo&, const MQTTString&, Permission) override;
  AuthResult check_publish(const UserInfo&, const MQTTString&, const MQTTByteVector&) override;
  AuthResult check_delivery_access(const UserInfo&, const MQTTString&,
                                   std::shared_ptr<AuthorizationRequest>&) override;
  bool is_super_user(const MQTTString&) override { return false; }
  bool requires_online_authorization() const override { return true; }
  const char* get_provider_name() const override;
  bool is_healthy() const override;
  AuthStats get_stats() const override;
  void reset_stats() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace auth
} // namespace mqtt
