#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_packet.h"

namespace mqtt {

class GlobalSessionManager;

namespace auth {
class AuthManager;
struct ClientAuthContext;
}  // namespace auth

// 遗嘱在发布时去掉 Will Delay Interval。延迟只决定何时调用发布。
struct WillBody {
  std::string topic;
  std::vector<uint8_t> payload;
  uint8_t qos = 0;
  bool retain = false;
  uint32_t delay_seconds = 0;
  uint8_t payload_format = 0;
  bool has_message_expiry = false;
  uint32_t message_expiry = 0;
  std::string content_type;
  std::string response_topic;
  std::vector<uint8_t> correlation_data;
  std::vector<std::pair<std::string, std::string>> user_properties;
};

struct WillConnectDecision {
  bool accept = true;
  ReasonCode reason = ReasonCode::Success;
};

// 非持久化会话在断开时结束，延迟一律为 0。持久化时取遗嘱延迟和会话过期的较小值。
uint32_t effective_will_delay_seconds(bool persistent, uint32_t will_delay_seconds,
                                     uint32_t session_expiry_seconds);

WillConnectDecision validate_will_connect(uint8_t will_qos, bool will_retain, const std::string& topic,
                                         uint8_t qos_limit, bool retain_available);

WillBody will_body_from_connect(const ConnectPacket& packet);

enum class WillPublishOutcome { Published, Rejected, Cancelled };

// still_current 为空表示不做世代检查。授权 PENDING 时在当前协程里有界等待，超时算拒绝。
WillPublishOutcome publish_will(GlobalSessionManager* sessions, auth::AuthManager* auth_manager,
                                const auth::ClientAuthContext* auth, const std::string& forward_client_id,
                                const WillBody& body, const std::function<bool()>& still_current);

// 延迟遗嘱的认证上下文。分配器独立于连接，不能指向已经释放的 Handler。
struct CopiedAuth {
  std::unique_ptr<MQTTAllocator> allocator;
  auth::ClientAuthContext* context = nullptr;

  CopiedAuth() = default;
  CopiedAuth(CopiedAuth&& other) noexcept;
  CopiedAuth& operator=(CopiedAuth&& other) noexcept;
  ~CopiedAuth();

  CopiedAuth(const CopiedAuth&) = delete;
  CopiedAuth& operator=(const CopiedAuth&) = delete;
};

bool copy_auth_context(const auth::ClientAuthContext& source, CopiedAuth& out);

// 按到期时间保存延迟遗嘱。mutex 只保护队列和世代号；发布在锁外进行。
class WillScheduler {
 public:
  struct Entry {
    std::string client_id;
    std::string forward_id;
    uint64_t generation = 0;
    std::chrono::steady_clock::time_point due{};
    std::thread::id owner;
    WillBody body;
    CopiedAuth auth;
  };

  void schedule(std::unique_ptr<Entry> entry);
  // 同一会话在遗嘱延迟结束前重连：丢掉还没到期的遗嘱。已经到期的保留并发布。
  void cancel(const std::string& client_id);
  // 旧会话结束：不改变世代号，让所属线程马上发布。
  void expire_now(const std::string& client_id);
  // session_present 为假表示会话已经结束，到期或被结束的遗嘱都要发布。
  void observe_connect(const std::string& client_id, bool clean_start, bool session_present = true);
  // 发布协程结束时调用，释放不再被队列或在途遗嘱引用的 Client ID。
  void complete(const std::string& client_id, uint64_t generation);

  std::vector<std::unique_ptr<Entry>> take_due(std::thread::id owner,
                                               std::chrono::steady_clock::time_point now,
                                               bool flush_all);
  bool generation_matches(const std::string& client_id, uint64_t generation) const;

 private:
  struct ClientGenerations {
    uint64_t next = 0;
    std::unordered_set<uint64_t> live;
    std::unordered_set<uint64_t> inflight;
  };

  void suppress_pending(const std::string& client_id, std::chrono::steady_clock::time_point now);
  void drop_client_if_idle(const std::string& client_id);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, ClientGenerations> generations_;
  std::vector<std::unique_ptr<Entry>> entries_;
};

// 取出本线程到期的遗嘱，并在协程里发布。flush_all 用于停机时尽力发布尚未到期的条目。
void launch_due_wills(GlobalSessionManager* sessions, auth::AuthManager* auth, std::thread::id owner,
                      bool flush_all);

}  // namespace mqtt
