#include "mqtt_auth_grpc.h"
#include "mqtt_string_utils.h"
#include "authorization.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>

using namespace mqtt::auth;
namespace pb = mqtts::authz::v1;
using namespace std::chrono;
static uint64_t wall() { return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count(); }

class Policy : public pb::Authorization::Service {
public:
  std::atomic<int> mode{0}, version{1}, calls{0}, largest{0}, delay{0};
  std::atomic<bool> delta{false};
  std::atomic<int> ttl{100}, age{600};
  grpc::Status GetRevision(grpc::ServerContext*, const pb::RevisionRequest* req, pb::RevisionResponse* out) override {
    if (mode == 3) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "offline");
    const int current = version;
    out->set_revision(std::to_string(current));
    if (delta && (req->known_revision() == std::to_string(current) || req->known_revision() == std::to_string(current - 1))) {
      out->set_is_delta(true);
      if (req->known_revision() != out->revision()) out->add_invalidated_usernames("fixture");
    }
    return grpc::Status::OK;
  }
  grpc::Status BatchAuthorize(grpc::ServerContext* context, const pb::BatchAuthorizeRequest* req, pb::BatchAuthorizeResponse* out) override {
    ++calls;
    auto headers = context->client_metadata().find("authorization");
    assert(headers != context->client_metadata().end());
    assert(std::string(headers->second.data(), headers->second.size()) == "Bearer " + std::string(40, 'q'));
    assert(req->requests_size() <= 32); largest = std::max(largest.load(), req->requests_size());
    const int revision = version, current_mode = mode;
    if (delay) std::this_thread::sleep_for(milliseconds(delay.load()));
    if (current_mode == 3) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "offline");
    for (int i = req->requests_size() - 1; i >= 0; --i) {
      const auto& request = req->requests(i);
      auto* result = out->add_results();
      result->set_request_id(current_mode == 1 ? 999999 : request.request_id());
      auto* decision = result->mutable_decision();
      decision->set_outcome(request.topic().find("denied") != std::string::npos || current_mode == 2 || (current_mode == 4 && request.username() == "fixture") ? pb::DENY : pb::ALLOW);
      decision->set_cache_revision(std::to_string(revision));
      decision->set_cache_ttl_ms(ttl); decision->set_cache_max_age_ms(age);
      decision->set_expires_at_ms(wall() + age);
    }
    return grpc::Status::OK;
  }
};

int main() {
  Policy policy; int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&policy); auto server = builder.BuildAndStart(); assert(server && port);
  setenv("MQTTS_TEST_RPC_TOKEN", std::string(40, 'q').c_str(), 1);
  std::map<std::string,std::string> settings = {
    {"endpoint", "127.0.0.1:" + std::to_string(port)}, {"insecure", "true"}, {"token_env", "MQTTS_TEST_RPC_TOKEN"},
    {"cache_ttl_ms", "100"}, {"cache_max_age_ms", "600"}, {"cache_version_interval_ms", "100"},
    {"timeout_ms", "500"}, {"rpc_queue_capacity", "256"}, {"rpc_workers", "4"}, {"batch_wait_ms", "5"}
  };
  GrpcAuthProvider provider(settings); assert(provider.initialize() == MQ_SUCCESS);
  auto* allocator = MQTTMemoryManager::get_instance().get_root_allocator();
  UserInfo user(allocator); user.username = "fixture"; user.client_id = "connection"; user.authorization_session = 7;
  std::this_thread::sleep_for(milliseconds(130)); // Observe the initial revision.
  const auto topic = [&](const std::string& value) { return mqtt::MQTTString(value.c_str(), mqtt::MQTTStrAllocator(allocator)); };
  const auto await = [&](const mqtt::MQTTString& destination, std::shared_ptr<AuthorizationRequest>& pending, AuthResult result) {
    const auto end = steady_clock::now() + seconds(2);
    while (result == AuthResult::PENDING && steady_clock::now() < end) {
      std::this_thread::sleep_for(milliseconds(1)); result = provider.check_delivery_access(user, destination, pending);
    }
    assert(result != AuthResult::PENDING); return result;
  };
  std::vector<std::shared_ptr<AuthorizationRequest>> pending(128);
  std::vector<AuthResult> results;
  for (int i = 0; i < 128; ++i) results.push_back(provider.check_delivery_access(user, topic((i % 2 ? "denied/" : "allowed/") + std::to_string(i)), pending[i]));
  for (int i = 0; i < 128; ++i) {
    auto destination = topic((i % 2 ? "denied/" : "allowed/") + std::to_string(i));
    assert(await(destination, pending[i], results[i]) == (i % 2 ? AuthResult::ACCESS_DENIED : AuthResult::SUCCESS));
  }
  const auto stats = provider.get_stats();
  assert(stats.rpc_batch_items == 128 && stats.rpc_batches < 32 && policy.largest > 1);

  // Unknown IDs cannot grant a different request. Recovery then warms a lease.
  policy.mode = 1;
  std::shared_ptr<AuthorizationRequest> ticket;
  auto destination = topic("malformed");
  assert(await(destination, ticket, provider.check_delivery_access(user, destination, ticket)) == AuthResult::INTERNAL_ERROR);
  policy.mode = 0; destination = topic("outage");
  assert(await(destination, ticket, provider.check_delivery_access(user, destination, ticket)) == AuthResult::SUCCESS);
  const auto before = policy.calls.load();
  assert(provider.check_delivery_access(user, destination, ticket) == AuthResult::SUCCESS && policy.calls == before);
  policy.mode = 3; std::this_thread::sleep_for(milliseconds(150));
  assert(provider.check_delivery_access(user, destination, ticket) == AuthResult::SUCCESS);
  std::this_thread::sleep_for(milliseconds(650));
  assert(await(destination, ticket, provider.check_delivery_access(user, destination, ticket)) == AuthResult::INTERNAL_ERROR);

  // A revision observed while an old ALLOW is in flight invalidates its waiter.
  provider.cleanup(); policy.mode = 0; policy.delay = 0;
  assert(provider.initialize() == MQ_SUCCESS); provider.cleanup();
  GrpcAuthProvider newer(settings); assert(newer.initialize() == MQ_SUCCESS);
  std::this_thread::sleep_for(milliseconds(150)); policy.delay = 300;
  destination = topic("revoked/in-flight");
  assert(newer.check_delivery_access(user, destination, ticket) == AuthResult::PENDING);
  std::this_thread::sleep_for(milliseconds(50)); ++policy.version;
  std::this_thread::sleep_for(milliseconds(400));
  assert(newer.check_delivery_access(user, destination, ticket) == AuthResult::INTERNAL_ERROR);
  newer.cleanup();
  policy.mode = 0; policy.delay = 0; policy.delta = true; policy.ttl = 5000; policy.age = 5000;
  settings["cache_ttl_ms"] = "5000"; settings["cache_max_age_ms"] = "5000";
  GrpcAuthProvider scoped(settings); assert(scoped.initialize() == MQ_SUCCESS);
  std::this_thread::sleep_for(milliseconds(150));
  UserInfo unrelated(allocator); unrelated.username = "unrelated"; unrelated.client_id = "other"; unrelated.authorization_session = 8;
  // Exercise fixed identity partitions with distinct buckets on this toolchain.
  while (std::hash<std::string>{}(mqtt::from_mqtt_string(unrelated.username)) % 1024 == std::hash<std::string>{}("fixture") % 1024)
    unrelated.username += "x";
  destination = topic("scoped-cache");
  assert(scoped.check_topic_access(user, destination, Permission::READ) == AuthResult::SUCCESS);
  assert(scoped.check_topic_access(unrelated, destination, Permission::READ) == AuthResult::SUCCESS);
  policy.mode = 4; ++policy.version;
  std::this_thread::sleep_for(milliseconds(220));
  const auto warm_calls = policy.calls.load();
  assert(scoped.check_topic_access(unrelated, destination, Permission::READ) == AuthResult::SUCCESS);
  assert(policy.calls == warm_calls);
  assert(scoped.check_topic_access(user, destination, Permission::READ) == AuthResult::ACCESS_DENIED);
  assert(policy.calls == warm_calls + 1);
  // Both synchronous waiters and deferred delivery jobs fence old RPC results.
  policy.mode = 0; policy.delay = 300;
  AuthResult late = AuthResult::SUCCESS;
  const auto start_calls = policy.calls.load();
  std::thread waiter([&] { late = scoped.check_topic_access(user, topic("scoped-in-flight"), Permission::READ); });
  for (int i = 0; i < 100 && policy.calls == start_calls; ++i) std::this_thread::sleep_for(milliseconds(2));
  ++policy.version;
  waiter.join(); assert(late == AuthResult::INTERNAL_ERROR);
  // Missing delta support/history still performs full invalidation.
  policy.delay = 0; policy.delta = false; ++policy.version;
  std::this_thread::sleep_for(milliseconds(220));
  const auto reset_calls = policy.calls.load();
  assert(scoped.check_topic_access(unrelated, destination, Permission::READ) == AuthResult::SUCCESS);
  assert(policy.calls == reset_calls + 1);
  scoped.cleanup(); server->Shutdown();
  std::cout << "128 mixed decisions in " << stats.rpc_batches << " RPC batches; cache, outage expiry, malformed IDs and in-flight revocation passed\n";
}
