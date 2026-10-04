#include "mqtt_auth_remote.h"
#include "mqtt_runtime.h"
#include "authorization.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <list>
#include <thread>
#include <unordered_set>

namespace mqtt {
namespace auth {
namespace {
using Json = nlohmann::json;
namespace pb = ::mqtts::authz::v1;
std::atomic<uint64_t> next_authorization_session{1};
uint64_t monotonic_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
size_t read_response(char* data, size_t size, size_t count, void* target) {
  auto& output = *static_cast<std::string*>(target);
  if (size && count > (8192 - output.size()) / size) return 0;
  output.append(data, size * count);
  return size * count;
}
bool valid_url(const std::string& url) {
  return (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0)
      && url.find_first_of("\r\n") == std::string::npos;
}
std::string digest(const void* bytes, size_t size) {
  unsigned char output[SHA256_DIGEST_LENGTH];
  SHA256(static_cast<const unsigned char*>(bytes), size, output);
  return std::string(reinterpret_cast<const char*>(output), sizeof(output));
}
std::string digest(const std::string& value) { return digest(value.data(), value.size()); }
std::string base64(const MQTTByteVector& payload) {
  std::string encoded(4 * ((payload.size() + 2) / 3) + 1, '\0');
  const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&encoded[0]),
      payload.data(), static_cast<int>(payload.size()));
  if (length < 0) throw std::runtime_error("payload encoding failed");
  encoded.resize(static_cast<size_t>(length));
  return encoded;
}
struct Signal {
  int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ~Signal() { if (fd >= 0) close(fd); }
  void notify() { const uint64_t value = 1; if (fd >= 0) (void)write(fd, &value, sizeof(value)); }
};
struct Decision {
  AuthResult result = AuthResult::ACCESS_DENIED;
  bool authoritative = false;
  uint64_t expires_at = 0, fresh_ms = 0, max_age_ms = 0;
  std::string revision;
};
struct Job {
  std::string url, body;
  uint64_t started = monotonic_ms(), deadline = 0;
  std::function<Decision(Decision)> complete;
  std::function<bool()> current;
  std::mutex mutex;
  bool finished = false;
  Decision decision;
  std::vector<std::shared_ptr<Signal>> waiters;

  Decision poll() {
    if (current && !current()) return Decision();
    std::lock_guard<std::mutex> lock(mutex);
    if (finished) {
      if (decision.expires_at && decision.expires_at <= wall_ms()) return Decision();
      return decision;
    }
    Decision value;
    if (monotonic_ms() < deadline) value.result = AuthResult::PENDING;
    return value;
  }

  void finish(Decision value) {
    try { if (complete) value = complete(std::move(value)); }
    catch (...) { value = Decision(); }
    std::vector<std::shared_ptr<Signal>> signals;
    {
      std::lock_guard<std::mutex> lock(mutex);
      decision = std::move(value);
      finished = true;
      signals.swap(waiters);
    }
    for (const auto& signal : signals) signal->notify();
  }
  Decision wait() {
    const auto signal = std::make_shared<Signal>();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (finished) return decision;
      if (signal->fd < 0 || waiters.size() >= 64) return Decision();
      waiters.push_back(signal);
    }
    for (;;) {
      const auto now = monotonic_ms();
      if (now >= deadline) break;
      // Each waiter owns a descriptor: libco and multiple threads can safely
      // wait for the same single-flight request without sharing poll state.
      runtime::current_runtime().wait_readable(signal->fd, static_cast<int>(deadline - now));
      std::lock_guard<std::mutex> lock(mutex);
      if (finished) return decision;
    }
    std::lock_guard<std::mutex> lock(mutex);
    return finished ? decision : Decision();
  }
};

struct PendingDelivery : AuthorizationRequest {
  std::shared_ptr<Job> job;
  uint64_t session;
  std::string username, client_id, topic;
  PendingDelivery(std::shared_ptr<Job> value, const UserInfo& user, const MQTTString& destination)
      : job(std::move(value)), session(user.authorization_session),
        username(from_mqtt_string(user.username)), client_id(from_mqtt_string(user.client_id)),
        topic(from_mqtt_string(destination)) {}
  AuthResult poll(const UserInfo& user, const MQTTString& destination) override {
    if (session != user.authorization_session || username != from_mqtt_string(user.username)
        || client_id != from_mqtt_string(user.client_id) || topic != from_mqtt_string(destination)
        || (user.expires_at_ms && user.expires_at_ms <= wall_ms())) return AuthResult::ACCESS_DENIED;
    return job->poll().result;
  }
};
}

struct RemoteAuthProvider::Impl {
  explicit Impl(const std::map<std::string, std::string>& value, bool rpc) : settings(value), grpc_mode(rpc) {}
  bool grpc_mode;
  size_t batch_size = 32, batch_bytes = 4 * 1024 * 1024;
  uint64_t batch_wait = 1;
  std::unique_ptr<pb::Authorization::Stub> stub;
  std::map<std::string, std::string> settings;
  std::string token;
  size_t payload_limit = 1024 * 1024, cache_capacity = 16384, worker_count = 4;
  size_t queue_capacity = 64, queue_byte_limit = 16 * 1024 * 1024;
  uint64_t timeout = 2000, fresh_limit = 0, age_limit = 300000, version_interval = 250, cooldown = 1000;
  bool include_payload = false;
  std::vector<std::string> ignored_fields;
  std::atomic<bool> initialized{false}, stopping{false};
  std::shared_ptr<std::atomic<uint64_t>> epoch = std::make_shared<std::atomic<uint64_t>>(0);
  std::shared_ptr<const std::string> revision = std::make_shared<const std::string>();

  struct Counters {
    std::atomic<uint64_t> logins{0}, successes{0}, checks{0}, allowed{0}, hits{0}, misses{0}, stale{0};
    std::atomic<uint64_t> evictions{0}, requests{0}, failures{0}, rejected{0}, refreshes{0}, opened{0}, batches{0}, items{0};
  } stats;
  struct CacheEntry {
    Decision decision;
    uint64_t fresh_until = 0, expires = 0, retry_after = 0, epoch = 0;
    std::list<std::string>::iterator lru;
  };
  struct Shard {
    std::mutex mutex;
    std::unordered_map<std::string, CacheEntry> entries;
    std::unordered_map<std::string, std::shared_ptr<Job>> pending;
    std::list<std::string> lru;
  };
  std::array<Shard, 16> shards;
  std::mutex queue_mutex, breaker_mutex;
  std::condition_variable work_ready;
  std::deque<std::shared_ptr<Job>> queue;
  size_t queued_bytes = 0;
  std::vector<std::thread> workers;
  std::thread version_worker;
  uint64_t circuit_until = 0;
  size_t consecutive_failures = 0;
  bool probe_running = false;

  uint64_t number(const char* name, uint64_t fallback, uint64_t low, uint64_t high) {
    auto it = settings.find(name);
    if (it == settings.end()) return fallback;
    size_t used = 0;
    const auto value = std::stoull(it->second, &used);
    if (used != it->second.size() || value < low || value > high) throw std::invalid_argument(name);
    return value;
  }
  bool circuit_admit() {
    std::lock_guard<std::mutex> lock(breaker_mutex);
    if (consecutive_failures < 3) return true;
    if (monotonic_ms() < circuit_until || probe_running) return false;
    probe_running = true;
    return true;
  }
  void circuit_result(bool healthy) {
    std::lock_guard<std::mutex> lock(breaker_mutex);
    probe_running = false;
    if (healthy) { consecutive_failures = 0; circuit_until = 0; }
    else if (++consecutive_failures >= 3) {
      circuit_until = monotonic_ms() + cooldown;
      ++stats.opened;
    }
  }
  Json http(CURL* curl, const std::string& url, const std::string& body, uint64_t deadline, bool version_check = false) {
    if (!curl || stopping || deadline <= monotonic_ms()) return Json();
    // Version polling has a reserved worker. Its failures do not prevent a
    // healthy authorization endpoint from validating new identities.
    if (!version_check && !circuit_admit()) { ++stats.rejected; return Json(); }
    ++stats.requests;
    const auto now = monotonic_ms();
    const auto remaining = deadline > now ? deadline - now : 1;
    std::string response;
    curl_easy_reset(curl); // Retains libcurl's connection/DNS/TLS-session caches.
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("X-Broker-Token: " + token).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(remaining));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(remaining));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    if (settings.count("ca_file") && !settings.at("ca_file").empty())
      curl_easy_setopt(curl, CURLOPT_CAINFO, settings.at("ca_file").c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, read_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    Json data;
    if (result == CURLE_OK && status == 200) data = Json::parse(response, nullptr, false);
    bool healthy = data.is_object();
    if (healthy && !version_check) {
      auto value = data.find("result");
      healthy = value != data.end() && value->is_string() && (*value == "allow" || *value == "deny");
    }
    if (!version_check) circuit_result(healthy);
    if (!healthy) { ++stats.failures; return Json(); }
    return data;
  }
  Decision parse(const Json& data) {
    Decision result;
    if (!data.is_object()) return result;
    try {
      result.authoritative = true;
      result.result = data.at("result") == "allow" ? AuthResult::SUCCESS : AuthResult::ACCESS_DENIED;
      if (data.contains("expire_at")) {
        if (!data["expire_at"].is_number_integer()) return Decision();
        const auto value = data["expire_at"].get<int64_t>();
        if (value <= 0 || static_cast<uint64_t>(value) > std::numeric_limits<uint64_t>::max() / 1000) return Decision();
        result.expires_at = static_cast<uint64_t>(value) * 1000;
        if (result.expires_at <= wall_ms()) result.result = AuthResult::ACCESS_DENIED;
      }
      // Both operator and policy service must explicitly permit decision reuse.
      auto ttl = data.find("cache_ttl_ms");
      if (fresh_limit && ttl != data.end() && ttl->is_number_unsigned()) {
        result.fresh_ms = std::min<uint64_t>(fresh_limit, ttl->get<uint64_t>());
        result.max_age_ms = result.fresh_ms;
        auto maximum = data.find("cache_max_age_ms");
        if (maximum != data.end() && maximum->is_number_unsigned())
          result.max_age_ms = std::min<uint64_t>(age_limit, maximum->get<uint64_t>());
        result.fresh_ms = std::min(result.fresh_ms, result.max_age_ms);
        if (result.result != AuthResult::SUCCESS) result.max_age_ms = result.fresh_ms = std::min<uint64_t>(1000, result.fresh_ms);
      }
      if (data.contains("cache_revision") && data["cache_revision"].is_string()) {
        result.revision = data["cache_revision"].get<std::string>();
        if (result.revision.size() > 128) return Decision();
      }
      return result;
    } catch (...) { return Decision(); }
  }
  Decision parse(const pb::Decision& value) {
    Decision result;
    if ((value.outcome() != pb::ALLOW && value.outcome() != pb::DENY) || value.cache_revision().size() > 128) return result;
    result.authoritative = true;
    result.result = value.outcome() == pb::ALLOW ? AuthResult::SUCCESS : AuthResult::ACCESS_DENIED;
    result.expires_at = value.expires_at_ms();
    if (result.expires_at && result.expires_at <= wall_ms()) result.result = AuthResult::ACCESS_DENIED;
    result.fresh_ms = std::min<uint64_t>(fresh_limit, value.cache_ttl_ms());
    result.max_age_ms = std::min<uint64_t>(age_limit, value.cache_max_age_ms());
    result.fresh_ms = std::min(result.fresh_ms, result.max_age_ms);
    if (result.result != AuthResult::SUCCESS) result.max_age_ms = result.fresh_ms = std::min<uint64_t>(1000, result.fresh_ms);
    result.revision = value.cache_revision();
    return result;
  }
  void rpc_context(grpc::ClientContext& context, uint64_t deadline) {
    const auto now = monotonic_ms();
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadline > now ? deadline - now : 0));
    context.AddMetadata("authorization", "Bearer " + token);
    context.set_wait_for_ready(false);
  }
  void rpc_batch(const std::vector<std::shared_ptr<Job>>& jobs) {
    std::vector<Decision> results(jobs.size());
    bool healthy = false, attempted = false;
    try {
      uint64_t deadline = std::numeric_limits<uint64_t>::max();
      for (const auto& job : jobs) deadline = std::min(deadline, job->deadline);
      if (!stopping && deadline > monotonic_ms() && circuit_admit()) {
        attempted = true; ++stats.requests;
        grpc::ClientContext context; rpc_context(context, deadline);
        if (jobs.front()->url == "authenticate") {
          pb::AuthenticateRequest request; pb::Decision reply;
          healthy = request.ParseFromString(jobs.front()->body) && stub->Authenticate(&context, request, &reply).ok();
          if (healthy) { results[0] = parse(reply); healthy = results[0].authoritative; }
        } else {
          pb::BatchAuthorizeRequest request; pb::BatchAuthorizeResponse reply;
          for (size_t i = 0; i < jobs.size(); ++i) {
            auto* item = request.add_requests();
            if (!item->ParseFromString(jobs[i]->body)) throw std::invalid_argument("invalid authorization request");
            item->set_request_id(i + 1);
          }
          ++stats.batches; stats.items += jobs.size();
          healthy = stub->BatchAuthorize(&context, request, &reply).ok() && static_cast<size_t>(reply.results_size()) == jobs.size();
          std::unordered_set<uint64_t> seen;
          if (healthy) for (const auto& item : reply.results()) {
            const auto id = item.request_id();
            if (!id || id > jobs.size() || !seen.insert(id).second || !item.has_decision()) { healthy = false; break; }
            results[id - 1] = parse(item.decision());
          }
          // A malformed envelope cannot associate permissions with a request.
          if (!healthy) results.assign(jobs.size(), Decision());
          else for (const auto& result : results) if (!result.authoritative) healthy = false;
        }
      } else ++stats.rejected;
    } catch (...) { healthy = false; results.assign(jobs.size(), Decision()); }
    if (attempted) { circuit_result(healthy); if (!healthy) ++stats.failures; }
    for (size_t i = 0; i < jobs.size(); ++i) {
      if (monotonic_ms() >= jobs[i]->deadline) results[i] = Decision();
      jobs[i]->finish(std::move(results[i]));
    }
  }
  bool enqueue(const std::shared_ptr<Job>& job) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (stopping || queue.size() >= queue_capacity || job->body.size() > queue_byte_limit - queued_bytes ||
        (grpc_mode && job->url == "authorize" && job->body.size() + 32 > batch_bytes)) {
      ++stats.rejected;
      return false;
    }
    queued_bytes += job->body.size();
    queue.push_back(job);
    work_ready.notify_all();
    return true;
  }
  void worker() {
    CURL* curl = curl_easy_init();
    for (;;) {
      std::shared_ptr<Job> job;
      std::vector<std::shared_ptr<Job>> batch;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        work_ready.wait(lock, [&] { return stopping || !queue.empty(); });
        if (queue.empty()) break;
        job = queue.front(); queue.pop_front(); queued_bytes -= job->body.size();
        if (grpc_mode) {
          batch.push_back(job);
          if (job->url == "authorize" && job->deadline > monotonic_ms()) {
            // A bounded aggregation window is paid only on cold/refresh work.
            // CONNECT is never delayed for batching and cannot be starved by it.
            work_ready.wait_for(lock, std::chrono::milliseconds(batch_wait), [&] { return stopping.load() || queue.size() >= batch_size - 1 || (!queue.empty() && queue.front()->url == "authenticate"); });
            size_t bytes = job->body.size() + 32;
            while (!queue.empty() && batch.size() < batch_size && queue.front()->url == "authorize") {
              const auto& next = queue.front();
              if (bytes + next->body.size() + 32 > batch_bytes) break;
              bytes += next->body.size() + 32; queued_bytes -= next->body.size();
              batch.push_back(next); queue.pop_front();
            }
          }
        }
      }
      if (grpc_mode) { rpc_batch(batch); continue; }
      Decision result;
      try { if (!stopping) result = parse(http(curl, job->url, job->body, job->deadline)); }
      catch (...) { ++stats.failures; circuit_result(false); }
      job->finish(std::move(result));
    }
    if (curl) curl_easy_cleanup(curl);
  }
  void update_version(const std::string& value) {
    const auto before = std::atomic_load(&revision);
    if (*before == value) return;
    std::atomic_store(&revision, std::make_shared<const std::string>(value));
    ++*epoch;
    for (auto& shard : shards) {
      std::lock_guard<std::mutex> lock(shard.mutex);
      stats.evictions += shard.entries.size();
      shard.entries.clear(); shard.lru.clear();
      // In-flight jobs retain their epoch; an older response cannot restore a
      // revoked lease or authorize a waiter after invalidation is observed.
    }
  }
  void watch_version() {
    CURL* curl = curl_easy_init();
    while (!stopping) {
      Json data;
      if (grpc_mode) {
        grpc::ClientContext context; rpc_context(context, monotonic_ms() + timeout);
        pb::RevisionRequest request; pb::RevisionResponse reply; ++stats.requests;
        if (stub->GetRevision(&context, request, &reply).ok()) data = {{"cache_revision", reply.revision()}};
        else ++stats.failures;
      } else data = http(curl, settings.at("cache_version_url"), "{}", monotonic_ms() + timeout, true);
      if (data.is_object() && data.contains("cache_revision") && data["cache_revision"].is_string()) {
        const auto value = data["cache_revision"].get<std::string>();
        if (!value.empty() && value.size() <= 128) update_version(value);
      }
      std::unique_lock<std::mutex> lock(queue_mutex);
      work_ready.wait_for(lock, std::chrono::milliseconds(version_interval), [&] { return stopping.load(); });
    }
    if (curl) curl_easy_cleanup(curl);
  }
  Decision await_job(const std::shared_ptr<Job>& job, std::shared_ptr<Job>* deferred) {
    if (!deferred) return job->wait();
    *deferred = job;
    return job->poll();
  }
  Decision fetch(const std::string& url, const std::string& body, std::shared_ptr<Job>* deferred = nullptr) {
    if (!initialized || body.size() > 8192 + (include_payload ? 4 * ((payload_limit + 2) / 3) : 0)) return Decision();
    const auto job = std::make_shared<Job>();
    job->url = url; job->body = body; job->deadline = job->started + timeout;
    if (deferred) {
      const auto generation = epoch;
      const auto captured_epoch = generation->load();
      job->current = [generation, captured_epoch] { return generation->load() == captured_epoch; };
    }
    return enqueue(job) ? await_job(job, deferred) : Decision();
  }
  std::string payload_key(const MQTTByteVector& payload) {
    if (!ignored_fields.empty()) {
      // Ignore only explicitly declared non-policy fields. Unknown fields,
      // aliases and all identity-bearing fields remain in the digest. Duplicate
      // JSON keys use the exact byte digest to avoid parser ambiguity.
      bool ambiguous = false;
      std::vector<std::unordered_set<std::string>> keys;
      const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 64) throw std::invalid_argument("cache JSON depth limit");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.empty() && !keys.back().insert(parsed.get<std::string>()).second) ambiguous = true;
        if (event == Json::parse_event_t::object_end && !keys.empty()) keys.pop_back();
        return true;
      };
      try {
        auto parsed = Json::parse(payload.begin(), payload.end(), callback, false);
        if (!ambiguous && parsed.is_object()) {
          for (const auto& field : ignored_fields) parsed.erase(field);
          return digest(parsed.dump());
        }
      } catch (const Json::exception&) {} catch (const std::invalid_argument&) {}
    }
    return digest(payload.data(), payload.size());
  }
  Decision authorize(const UserInfo& user, const std::string& topic, const char* action, const MQTTByteVector* payload,
                     std::shared_ptr<Job>* deferred = nullptr) {
    Decision rejected;
    if (!initialized || (user.expires_at_ms && user.expires_at_ms <= wall_ms()) ||
        (payload && payload->size() > payload_limit)) return rejected;
    const auto body = [&]() {
      if (grpc_mode) {
        pb::AuthorizeRequest value;
        value.set_username(from_mqtt_string(user.username)); value.set_client_id(from_mqtt_string(user.client_id));
        value.set_action(std::string(action) == "publish" ? pb::PUBLISH : pb::SUBSCRIBE); value.set_topic(topic);
        if (payload) { value.set_has_payload(true); value.set_payload(payload->data(), payload->size()); }
        return value.SerializeAsString();
      }
      Json value = {{"username", from_mqtt_string(user.username)}, {"clientid", from_mqtt_string(user.client_id)}, {"action", action}, {"topic", topic}};
      if (payload) { value["payload_encoding"] = "base64"; value["payload"] = base64(*payload); }
      return value.dump();
    };
    if (!fresh_limit) return fetch(settings.at("authorization_url"), body(), deferred);
    std::string key = Json::array({user.authorization_session, from_mqtt_string(user.username), from_mqtt_string(user.client_id), action, topic}).dump();
    if (payload) key += payload_key(*payload);
    key = digest(key);
    auto& shard = shards[static_cast<unsigned char>(key[0]) % shards.size()];
    std::shared_ptr<Job> job;
    Decision cached;
    bool hit = false, create = false;
    const auto now = monotonic_ms();
    const auto captured_epoch = epoch->load();
    {
      std::lock_guard<std::mutex> lock(shard.mutex);
      auto found = shard.entries.find(key);
      if (found != shard.entries.end()) {
        if (now < found->second.expires && found->second.epoch == epoch->load()) {
          hit = true; cached = found->second.decision;
          shard.lru.splice(shard.lru.begin(), shard.lru, found->second.lru);
          ++stats.hits;
          if (now < found->second.fresh_until) return cached;
          ++stats.stale;
          if (now < found->second.retry_after) return cached;
        } else { shard.lru.erase(found->second.lru); shard.entries.erase(found); }
      }
      if (!hit) ++stats.misses;
      auto pending = shard.pending.find(key);
      if (pending != shard.pending.end()) job = pending->second;
      else if (shard.pending.size() < queue_capacity + worker_count) {
        job = std::make_shared<Job>(); job->deadline = job->started + timeout;
        const auto generation = epoch;
        job->current = [generation, captured_epoch] { return generation->load() == captured_epoch; };
        shard.pending.emplace(key, job); create = true;
      }
    }
    if (!job) return hit ? cached : rejected;
    if (create) {
      if (hit) ++stats.refreshes;
      job->url = settings.at("authorization_url");
      const uint64_t session_expiry = user.expires_at_ms;
      const uint64_t started = job->started;
      job->complete = [this, &shard, key, captured_epoch, session_expiry, started](Decision result) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.pending.erase(key);
        if (captured_epoch != epoch->load()) return Decision();
        auto existing = shard.entries.find(key);
        if (!result.authoritative) {
          if (existing != shard.entries.end()) existing->second.retry_after = monotonic_ms() + cooldown;
          return result; // A failure never extends a previously granted lease.
        }
        if (existing != shard.entries.end()) {
          shard.lru.erase(existing->second.lru); shard.entries.erase(existing);
        }
        const auto current = std::atomic_load(&revision);
        if (!settings.at("cache_version_url").empty() && (result.revision.empty() || (!current->empty() && result.revision != *current))) return result;
        uint64_t expires = started + result.max_age_ms;
        const uint64_t real_now = wall_ms(), mono_now = monotonic_ms();
        for (const auto bound : {session_expiry, result.expires_at}) {
          if (bound) expires = std::min(expires, mono_now + (bound > real_now ? bound - real_now : 0));
        }
        if (!result.fresh_ms || expires <= mono_now) return result;
        while (shard.entries.size() >= cache_capacity / shards.size()) {
          shard.entries.erase(shard.lru.back()); shard.lru.pop_back(); ++stats.evictions;
        }
        shard.lru.push_front(key);
        CacheEntry entry; entry.decision = result; entry.expires = expires; entry.epoch = captured_epoch;
        entry.fresh_until = std::min(started + result.fresh_ms, expires); entry.lru = shard.lru.begin();
        shard.entries.emplace(key, std::move(entry));
        return result;
      };
      try { job->body = body(); } catch (...) { job->finish(Decision()); return hit ? cached : rejected; }
      if (job->body.size() > 8192 + (payload ? 4 * ((payload_limit + 2) / 3) : 0) || !enqueue(job)) job->finish(Decision());
    }
    return hit ? cached : await_job(job, deferred);
  }
};

RemoteAuthProvider::RemoteAuthProvider(const std::map<std::string, std::string>& settings, bool grpc) : impl_(new Impl(settings, grpc)) {}
RemoteAuthProvider::~RemoteAuthProvider() { cleanup(); }
int RemoteAuthProvider::initialize() {
  auto& p = *impl_;
  if (p.initialized) return MQ_SUCCESS;
  p.token.clear(); p.ignored_fields.clear();
  static const std::unordered_set<std::string> common = {"token_file", "token_env", "ca_file", "timeout_ms", "publish_payload", "max_payload_bytes", "cache_ttl_ms", "cache_max_age_ms", "cache_max_entries", "cache_version_interval_ms", "publish_cache_ignored_fields", "failure_cooldown_ms"};
  static const std::unordered_set<std::string> http_settings = {"authentication_url", "authorization_url", "cache_version_url", "http_workers", "http_queue_capacity", "http_queue_bytes"};
  static const std::unordered_set<std::string> rpc_settings = {"endpoint", "insecure", "client_cert_file", "client_key_file", "rpc_workers", "rpc_queue_capacity", "rpc_queue_bytes", "batch_max_requests", "batch_max_bytes", "batch_wait_ms"};
  for (const auto& setting : p.settings) if (!common.count(setting.first) && !(p.grpc_mode ? rpc_settings : http_settings).count(setting.first)) return MQ_ERR_INVALID_ARGS;
  static const CURLcode ready = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (ready != CURLE_OK) return MQ_ERR_INVALID_ARGS;
  if (p.grpc_mode) {
    if (p.settings["endpoint"].empty() || p.settings["endpoint"].size() > 1024 || p.settings["endpoint"].find_first_of("\r\n") != std::string::npos) return MQ_ERR_INVALID_ARGS;
    p.settings["authentication_url"] = "authenticate"; p.settings["authorization_url"] = "authorize"; p.settings["cache_version_url"] = "revision";
  } else if (!valid_url(p.settings["authentication_url"]) || !valid_url(p.settings["authorization_url"])) return MQ_ERR_INVALID_ARGS;
  try {
    p.timeout = p.number("timeout_ms", 2000, 100, 10000);
    p.payload_limit = p.number("max_payload_bytes", 1048576, 1, 16777216);
    p.fresh_limit = p.number("cache_ttl_ms", 0, 0, 300000);
    p.age_limit = p.number("cache_max_age_ms", 300000, 1, 300000);
    p.cache_capacity = p.number("cache_max_entries", 16384, 16, 1048576);
    p.worker_count = p.number(p.grpc_mode ? "rpc_workers" : "http_workers", 4, 1, 16);
    p.queue_capacity = p.number(p.grpc_mode ? "rpc_queue_capacity" : "http_queue_capacity", 64, 1, 4096);
    p.queue_byte_limit = p.number(p.grpc_mode ? "rpc_queue_bytes" : "http_queue_bytes", 16777216, 8192, 134217728);
    p.cooldown = p.number("failure_cooldown_ms", 1000, 100, 30000);
    p.version_interval = p.number("cache_version_interval_ms", 250, 100, 60000);
    if (p.fresh_limit > p.age_limit) return MQ_ERR_INVALID_ARGS;
    if (!p.grpc_mode && !p.settings["cache_version_url"].empty() && (!p.fresh_limit || !valid_url(p.settings["cache_version_url"]))) return MQ_ERR_INVALID_ARGS;
    const auto mode = p.settings.find("publish_payload");
    if (mode != p.settings.end() && mode->second != "none" && mode->second != (p.grpc_mode ? "bytes" : "base64")) return MQ_ERR_INVALID_ARGS;
    p.include_payload = mode != p.settings.end() && mode->second == (p.grpc_mode ? "bytes" : "base64");
    if (p.settings.count("publish_cache_ignored_fields")) {
      const auto fields = Json::parse(p.settings.at("publish_cache_ignored_fields"));
      if (!p.include_payload || !p.fresh_limit || !fields.is_array() || fields.size() > 32) return MQ_ERR_INVALID_ARGS;
      for (const auto& field : fields) {
        if (!field.is_string() || field.get<std::string>().empty() || field.get<std::string>().size() > 128) return MQ_ERR_INVALID_ARGS;
        p.ignored_fields.push_back(field.get<std::string>());
      }
    }
    if (!p.settings["token_file"].empty()) { std::ifstream file(p.settings["token_file"]); std::getline(file, p.token); }
    else if (!p.settings["token_env"].empty()) { const char* value = std::getenv(p.settings["token_env"].c_str()); if (value) p.token = value; }
  } catch (...) { return MQ_ERR_INVALID_ARGS; }
  if (!p.token.empty() && p.token.back() == '\r') p.token.pop_back();
  if (p.token.size() < 32 || p.token.size() > 4096 || p.token.find_first_of("\r\n") != std::string::npos) return MQ_ERR_INVALID_ARGS;
  if (p.grpc_mode) {
    try {
      p.batch_size = p.number("batch_max_requests", 32, 1, 64);
      p.batch_bytes = p.number("batch_max_bytes", 4194304, 16384, 4194304);
      p.batch_wait = p.number("batch_wait_ms", 1, 0, 10);
      if (p.payload_limit > 1048576 || p.payload_limit + 8192 > p.batch_bytes) return MQ_ERR_INVALID_ARGS;
      const auto file_contents = [](const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::invalid_argument("RPC TLS file unavailable");
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
      };
      std::shared_ptr<grpc::ChannelCredentials> credentials;
      if (p.settings["insecure"] == "true") {
        if (!p.settings["ca_file"].empty() || !p.settings["client_cert_file"].empty() || !p.settings["client_key_file"].empty()) return MQ_ERR_INVALID_ARGS;
        credentials = grpc::InsecureChannelCredentials();
      } else {
        if (!p.settings["insecure"].empty() && p.settings["insecure"] != "false") return MQ_ERR_INVALID_ARGS;
        grpc::SslCredentialsOptions tls;
        if (!p.settings["ca_file"].empty()) tls.pem_root_certs = file_contents(p.settings["ca_file"]);
        if (!p.settings["client_cert_file"].empty() || !p.settings["client_key_file"].empty()) {
          tls.pem_cert_chain = file_contents(p.settings["client_cert_file"]); tls.pem_private_key = file_contents(p.settings["client_key_file"]);
        }
        credentials = grpc::SslCredentials(tls);
      }
      grpc::ChannelArguments args; args.SetMaxReceiveMessageSize(4194304); args.SetMaxSendMessageSize(4194304);
      p.stub = pb::Authorization::NewStub(grpc::CreateCustomChannel(p.settings["endpoint"], credentials, args));
    } catch (...) { return MQ_ERR_INVALID_ARGS; }
  }
  p.stopping = false;
  try {
    for (size_t i = 0; i < p.worker_count; ++i) p.workers.emplace_back([&p] { p.worker(); });
    if (!p.settings["cache_version_url"].empty()) p.version_worker = std::thread([&p] { p.watch_version(); });
  } catch (...) { cleanup(); return MQ_ERR_INVALID_ARGS; }
  p.initialized = true;
  return MQ_SUCCESS;
}
void RemoteAuthProvider::cleanup() {
  auto& p = *impl_;
  p.initialized = false; p.stopping = true; p.work_ready.notify_all();
  ++*p.epoch;
  if (p.version_worker.joinable()) p.version_worker.join();
  for (auto& worker : p.workers) if (worker.joinable()) worker.join();
  p.workers.clear();
  for (auto& shard : p.shards) {
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.entries.clear(); shard.lru.clear(); shard.pending.clear();
  }
  if (p.grpc_mode) {
    p.settings.erase("authentication_url"); p.settings.erase("authorization_url"); p.settings.erase("cache_version_url");
  }
}
const char* RemoteAuthProvider::get_provider_name() const { return impl_->grpc_mode ? "gRPC" : "HTTP"; }
bool RemoteAuthProvider::is_healthy() const { return impl_->initialized; }
AuthResult RemoteAuthProvider::authenticate_user(const MQTTString& username, const MQTTString& password,
    const MQTTString& client_id, const MQTTString& client_ip, uint16_t client_port, UserInfo& user) {
  auto& p = *impl_; ++p.stats.logins;
  Decision result;
  try {
    if (p.grpc_mode) {
      pb::AuthenticateRequest request;
      request.set_username(from_mqtt_string(username)); request.set_password(from_mqtt_string(password)); request.set_client_id(from_mqtt_string(client_id));
      result = p.fetch("authenticate", request.SerializeAsString());
    } else result = p.fetch(p.settings.at("authentication_url"), Json({{"username", from_mqtt_string(username)}, {"password", from_mqtt_string(password)}, {"clientid", from_mqtt_string(client_id)}}).dump());
  } catch (...) {}
  if (result.result == AuthResult::SUCCESS) {
    ++p.stats.successes;
    user.username = username; user.client_id = client_id; user.client_ip = client_ip; user.client_port = client_port;
    user.is_super_user = false; user.expires_at_ms = result.expires_at; user.authorization_session = next_authorization_session++;
  }
  return result.result;
}
AuthResult RemoteAuthProvider::check_topic_access(const UserInfo& user, const MQTTString& topic, Permission permission) {
  if (permission != Permission::READ && permission != Permission::WRITE) return AuthResult::ACCESS_DENIED;
  auto& p = *impl_; ++p.stats.checks;
  Decision result;
  try { result = p.authorize(user, from_mqtt_string(topic), permission == Permission::READ ? "subscribe" : "publish", nullptr); } catch (...) {}
  if (result.result == AuthResult::SUCCESS) ++p.stats.allowed;
  return result.result;
}
AuthResult RemoteAuthProvider::check_publish(const UserInfo& user, const MQTTString& topic, const MQTTByteVector& payload) {
  auto& p = *impl_; ++p.stats.checks;
  Decision result;
  try { result = p.authorize(user, from_mqtt_string(topic), "publish", p.include_payload ? &payload : nullptr); } catch (...) {}
  if (result.result == AuthResult::SUCCESS) ++p.stats.allowed;
  return result.result;
}
AuthResult RemoteAuthProvider::check_delivery_access(const UserInfo& user, const MQTTString& topic,
                                                   std::shared_ptr<AuthorizationRequest>& pending) {
  auto& p = *impl_;
  if (!p.initialized) { pending.reset(); return AuthResult::ACCESS_DENIED; }
  AuthResult result = AuthResult::ACCESS_DENIED;
  try {
    if (pending) result = pending->poll(user, topic);
    else {
      ++p.stats.checks;
      std::shared_ptr<Job> job;
      result = p.authorize(user, from_mqtt_string(topic), "subscribe", nullptr, &job).result;
      if (result == AuthResult::PENDING) pending = std::make_shared<PendingDelivery>(job, user, topic);
    }
  } catch (...) { result = AuthResult::ACCESS_DENIED; }
  if (result != AuthResult::PENDING) {
    pending.reset();
    if (result == AuthResult::SUCCESS) ++p.stats.allowed;
  }
  return result;
}
AuthStats RemoteAuthProvider::get_stats() const {
  const auto& s = impl_->stats;
  AuthStats out;
  out.total_login_attempts = s.logins; out.successful_logins = s.successes; out.failed_logins = out.total_login_attempts - std::min(out.total_login_attempts, out.successful_logins);
  out.total_topic_checks = s.checks; out.topic_access_granted = s.allowed; out.topic_access_denied = out.total_topic_checks - std::min(out.total_topic_checks, out.topic_access_granted);
  out.cache_hits = s.hits; out.cache_misses = s.misses; out.cache_stale_hits = s.stale; out.cache_evictions = s.evictions;
  if (impl_->grpc_mode) { out.rpc_requests = s.requests; out.rpc_failures = s.failures; out.rpc_rejected = s.rejected; out.rpc_batches = s.batches; out.rpc_batch_items = s.items; }
  else { out.http_requests = s.requests; out.http_failures = s.failures; out.http_rejected = s.rejected; } out.cache_refreshes = s.refreshes; out.circuit_opened = s.opened;
  return out;
}
void RemoteAuthProvider::reset_stats() {
  auto& s = impl_->stats;
  s.logins = 0; s.successes = 0; s.checks = 0; s.allowed = 0; s.hits = 0; s.misses = 0; s.stale = 0;
  s.evictions = 0; s.requests = 0; s.failures = 0; s.rejected = 0; s.refreshes = 0; s.opened = 0; s.batches = 0; s.items = 0;
}
} // namespace auth
} // namespace mqtt
