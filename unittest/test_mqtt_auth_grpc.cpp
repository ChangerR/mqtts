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
  grpc::Status GetRevision(grpc::ServerContext*, const pb::RevisionRequest*, pb::RevisionResponse* out) override {
    if (mode == 3) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "offline");
    out->set_revision(std::to_string(version.load())); return grpc::Status::OK;
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
      decision->set_outcome(request.topic().find("denied") != std::string::npos || current_mode == 2 ? pb::DENY : pb::ALLOW);
      decision->set_cache_revision(std::to_string(revision));
      decision->set_cache_ttl_ms(100); decision->set_cache_max_age_ms(600);
      decision->set_expires_at_ms(wall() + 600);
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
  assert(await(destination, ticket, provider.check_delivery_access(user, destination, ticket)) == AuthResult::ACCESS_DENIED);
  policy.mode = 0; destination = topic("outage");
  assert(await(destination, ticket, provider.check_delivery_access(user, destination, ticket)) == AuthResult::SUCCESS);
  const auto before = policy.calls.load();
  assert(provider.check_delivery_access(user, destination, ticket) == AuthResult::SUCCESS && policy.calls == before);
  policy.mode = 3; std::this_thread::sleep_for(milliseconds(150));
  assert(provider.check_delivery_access(user, destination, ticket) == AuthResult::SUCCESS);
  std::this_thread::sleep_for(milliseconds(650));
  assert(await(destination, ticket, provider.check_delivery_access(user, destination, ticket)) == AuthResult::ACCESS_DENIED);

  // A revision observed while an old ALLOW is in flight invalidates its waiter.
  provider.cleanup(); policy.mode = 0; policy.delay = 0;
  assert(provider.initialize() == MQ_SUCCESS); provider.cleanup();
  GrpcAuthProvider newer(settings); assert(newer.initialize() == MQ_SUCCESS);
  std::this_thread::sleep_for(milliseconds(150)); policy.delay = 300;
  destination = topic("revoked/in-flight");
  assert(newer.check_delivery_access(user, destination, ticket) == AuthResult::PENDING);
  std::this_thread::sleep_for(milliseconds(50)); ++policy.version;
  std::this_thread::sleep_for(milliseconds(400));
  assert(newer.check_delivery_access(user, destination, ticket) == AuthResult::ACCESS_DENIED);
  newer.cleanup(); server->Shutdown();
  std::cout << "128 mixed decisions in " << stats.rpc_batches << " RPC batches; cache, outage expiry, malformed IDs and in-flight revocation passed\n";
}
