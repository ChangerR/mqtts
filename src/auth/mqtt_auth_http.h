#pragma once

#include "mqtt_auth_interface.h"
#include <atomic>
#include <map>

namespace mqtt {
namespace auth {

// An optional external policy service makes authentication and authorization
// decisions. No successful decision is cached. Payload bytes remain opaque;
// this provider has no application-specific topics, fields, or dependencies.
class HttpAuthProvider : public IAuthProvider {
public:
  explicit HttpAuthProvider(const std::map<std::string, std::string>& settings);
  int initialize() override;
  void cleanup() override;
  AuthResult authenticate_user(const MQTTString&, const MQTTString&, const MQTTString&,
                               const MQTTString&, uint16_t, UserInfo&) override;
  AuthResult check_topic_access(const UserInfo&, const MQTTString&, Permission) override;
  AuthResult check_publish(const UserInfo&, const MQTTString&, const MQTTByteVector&) override;
  bool is_super_user(const MQTTString&) override { return false; }
  bool requires_online_authorization() const override { return true; }
  const char* get_provider_name() const override { return "HTTP"; }
  bool is_healthy() const override { return initialized_.load(); }
  AuthStats get_stats() const override;
  void reset_stats() override;

private:
  AuthResult request(const std::string& url, const std::string& body, uint64_t& expires_at_ms);
  std::map<std::string, std::string> settings_;
  std::string token_;
  long timeout_ms_ = 2000;
  bool include_payload_ = false;
  size_t max_payload_bytes_ = 1024 * 1024;
  std::atomic<bool> initialized_{false};
  mutable std::mutex stats_mutex_;
  AuthStats stats_;
};

} // namespace auth
} // namespace mqtt
