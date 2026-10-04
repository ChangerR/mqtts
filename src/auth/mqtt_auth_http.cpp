#include "mqtt_auth_http.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <algorithm>
#include <openssl/evp.h>
#include <fstream>
#include <limits>

namespace mqtt {
namespace auth {
namespace {
size_t read_response(char* data, size_t size, size_t count, void* target)
{
  auto& output = *static_cast<std::string*>(target);
  if (size && count > (8192 - output.size()) / size) return 0;
  output.append(data, size * count);
  return size * count;
}
bool valid_url(const std::string& url)
{
  return (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0)
      && url.find_first_of("\r\n") == std::string::npos;
}
uint64_t now_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
}

HttpAuthProvider::HttpAuthProvider(const std::map<std::string, std::string>& settings)
    : settings_(settings) {}

int HttpAuthProvider::initialize()
{
  static const std::vector<std::string> supported = {"authentication_url", "authorization_url",
    "token_file", "token_env", "ca_file", "timeout_ms", "publish_payload", "max_payload_bytes"};
  for (const auto& setting : settings_) {
    if (std::find(supported.begin(), supported.end(), setting.first) == supported.end())
      return MQ_ERR_INVALID_ARGS;
  }
  static const CURLcode curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (curl_ready != CURLE_OK || !valid_url(settings_["authentication_url"])
      || !valid_url(settings_["authorization_url"])) return MQ_ERR_INVALID_ARGS;
  try {
    if (settings_.count("timeout_ms")) timeout_ms_ = std::stol(settings_["timeout_ms"]);
    const auto mode = settings_.find("publish_payload");
    if (mode != settings_.end() && mode->second != "none" && mode->second != "base64")
      return MQ_ERR_INVALID_ARGS;
    include_payload_ = mode != settings_.end() && mode->second == "base64";
    if (settings_.count("max_payload_bytes")) {
      size_t consumed = 0;
      const auto value = std::stoul(settings_["max_payload_bytes"], &consumed);
      if (consumed != settings_["max_payload_bytes"].size() || value == 0 || value > 16 * 1024 * 1024)
        return MQ_ERR_INVALID_ARGS;
      max_payload_bytes_ = value;
    }
    if (!settings_["token_file"].empty()) {
      std::ifstream file(settings_["token_file"]);
      std::getline(file, token_);
    } else if (!settings_["token_env"].empty()) {
      const char* value = std::getenv(settings_["token_env"].c_str());
      if (value) token_ = value;
    }
  } catch (...) { return MQ_ERR_INVALID_ARGS; }
  if (!token_.empty() && token_.back() == '\r') token_.pop_back();
  if (token_.size() < 32 || token_.size() > 4096 || token_.find_first_of("\r\n") != std::string::npos
      || timeout_ms_ < 100 || timeout_ms_ > 10000) return MQ_ERR_INVALID_ARGS;
  initialized_ = true;
  return MQ_SUCCESS;
}

void HttpAuthProvider::cleanup() { initialized_ = false; }

AuthResult HttpAuthProvider::request(const std::string& url, const std::string& body,
                                      uint64_t& expires_at_ms)
{
  expires_at_ms = 0;
  if (!initialized_ || body.size() > 8192 + (include_payload_ ? 4 * ((max_payload_bytes_ + 2) / 3) : 0)) return AuthResult::ACCESS_DENIED;
  CURL* curl = curl_easy_init();
  if (!curl) return AuthResult::INTERNAL_ERROR;
  std::string response;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, ("X-Broker-Token: " + token_).c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms_);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  const auto ca = settings_.find("ca_file");
  if (ca != settings_.end() && !ca->second.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, ca->second.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, read_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  const auto result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK || status != 200) return AuthResult::ACCESS_DENIED;
  try {
    const auto data = nlohmann::json::parse(response);
    if (!data.is_object() || data.value("result", std::string()) != "allow") return AuthResult::ACCESS_DENIED;
    if (data.contains("expire_at")) {
      if (!data["expire_at"].is_number_integer()) return AuthResult::ACCESS_DENIED;
      const auto expiry = data["expire_at"].get<int64_t>();
      if (expiry <= 0 || static_cast<uint64_t>(expiry) > std::numeric_limits<uint64_t>::max() / 1000)
        return AuthResult::ACCESS_DENIED;
      expires_at_ms = static_cast<uint64_t>(expiry) * 1000;
      if (expires_at_ms <= now_ms()) return AuthResult::ACCESS_DENIED;
    }
    return AuthResult::SUCCESS;
  } catch (...) { return AuthResult::ACCESS_DENIED; }
}

AuthResult HttpAuthProvider::authenticate_user(const MQTTString& username, const MQTTString& password,
    const MQTTString& client_id, const MQTTString& client_ip, uint16_t client_port, UserInfo& user)
{
  uint64_t expiry = 0;
  AuthResult result = AuthResult::ACCESS_DENIED;
  try {
    const nlohmann::json body = {{"username", from_mqtt_string(username)}, {"password", from_mqtt_string(password)},
      {"clientid", from_mqtt_string(client_id)}};
    result = request(settings_.at("authentication_url"), body.dump(), expiry);
  } catch (...) {}
  if (result == AuthResult::SUCCESS) {
    user.username = username;
    user.client_id = client_id;
    user.client_ip = client_ip;
    user.client_port = client_port;
    user.is_super_user = false;
    user.expires_at_ms = expiry;
  }
  std::lock_guard<std::mutex> lock(stats_mutex_);
  ++stats_.total_login_attempts;
  if (result == AuthResult::SUCCESS) ++stats_.successful_logins; else ++stats_.failed_logins;
  return result;
}

AuthResult HttpAuthProvider::check_topic_access(const UserInfo& user, const MQTTString& topic, Permission permission)
{
  if (permission != Permission::READ && permission != Permission::WRITE) return AuthResult::ACCESS_DENIED;
  uint64_t expiry = 0;
  AuthResult result = AuthResult::ACCESS_DENIED;
  try {
    const nlohmann::json body = {{"username", from_mqtt_string(user.username)}, {"clientid", from_mqtt_string(user.client_id)},
      {"action", permission == Permission::READ ? "subscribe" : "publish"}, {"topic", from_mqtt_string(topic)}};
    result = request(settings_.at("authorization_url"), body.dump(), expiry);
  } catch (...) {}
  std::lock_guard<std::mutex> lock(stats_mutex_);
  ++stats_.total_topic_checks;
  if (result == AuthResult::SUCCESS) ++stats_.topic_access_granted; else ++stats_.topic_access_denied;
  return result;
}

AuthStats HttpAuthProvider::get_stats() const { std::lock_guard<std::mutex> lock(stats_mutex_); return stats_; }

AuthResult HttpAuthProvider::check_publish(const UserInfo& user, const MQTTString& topic, const MQTTByteVector& payload)
{
  if (!include_payload_) return check_topic_access(user, topic, Permission::WRITE);
  AuthResult result = AuthResult::ACCESS_DENIED;
  if (payload.size() <= max_payload_bytes_) {
    try {
      // Transport bytes are opaque to the broker. Only the external policy
      // service knows their format or application-specific meaning.
      std::string encoded(4 * ((payload.size() + 2) / 3) + 1, '\0');
      const auto length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&encoded[0]),
          payload.data(), static_cast<int>(payload.size()));
      if (length >= 0) {
        encoded.resize(static_cast<size_t>(length));
        const nlohmann::json body = {{"username", from_mqtt_string(user.username)},
          {"clientid", from_mqtt_string(user.client_id)}, {"action", "publish"},
          {"topic", from_mqtt_string(topic)}, {"payload_encoding", "base64"}, {"payload", encoded}};
        uint64_t expiry = 0;
        result = request(settings_.at("authorization_url"), body.dump(), expiry);
      }
    } catch (...) {}
  }
  std::lock_guard<std::mutex> lock(stats_mutex_);
  ++stats_.total_topic_checks;
  if (result == AuthResult::SUCCESS) ++stats_.topic_access_granted; else ++stats_.topic_access_denied;
  return result;
}

void HttpAuthProvider::reset_stats() { std::lock_guard<std::mutex> lock(stats_mutex_); stats_ = AuthStats(); }
} // namespace auth
} // namespace mqtt
