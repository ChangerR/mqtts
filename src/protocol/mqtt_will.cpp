#include "mqtt_will.h"

#include <algorithm>
#include <utility>
#include "logger.h"
#include "mqtt_auth_interface.h"
#include "mqtt_runtime.h"
#include "mqtt_session_manager_v2.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {

namespace {

const int kWillAuthWaitMs = 11000;

void assign_string(MQTTString& destination, const std::string& value)
{
  destination.assign(value.begin(), value.end());
}

struct WillFire {
  GlobalSessionManager* sessions;
  auth::AuthManager* auth;
  std::unique_ptr<WillScheduler::Entry> entry;
};

void* run_will_fire(void* arg)
{
  std::unique_ptr<WillFire> fire(static_cast<WillFire*>(arg));
  runtime::current_runtime().enable_async_syscalls();
  if (!fire || !fire->entry || !fire->sessions) {
    return nullptr;
  }
  WillScheduler::Entry* entry = fire->entry.get();
  const std::string client_id = entry->client_id;
  const uint64_t generation = entry->generation;
  GlobalSessionManager* sessions = fire->sessions;
  const WillPublishOutcome outcome = publish_will(
      sessions, fire->auth, entry->auth.context, entry->forward_id, entry->body,
      [sessions, client_id, generation]() {
        return sessions->will_scheduler().generation_matches(client_id, generation);
      });
  sessions->will_scheduler().complete(client_id, generation);
  if (outcome == WillPublishOutcome::Published) {
    sessions->note_will_published();
  } else if (outcome == WillPublishOutcome::Rejected) {
    sessions->note_will_rejected();
  }
  return nullptr;
}

}  // namespace

uint32_t effective_will_delay_seconds(bool persistent, uint32_t will_delay_seconds,
                                     uint32_t session_expiry_seconds)
{
  if (!persistent) {
    return 0;
  }
  return std::min(will_delay_seconds, session_expiry_seconds);
}

WillConnectDecision validate_will_connect(uint8_t will_qos, bool will_retain, const std::string& topic,
                                         uint8_t qos_limit, bool retain_available)
{
  WillConnectDecision decision;
  if (topic.empty() || topic.find('+') != std::string::npos || topic.find('#') != std::string::npos) {
    decision.accept = false;
    decision.reason = ReasonCode::ProtocolError;
    return decision;
  }
  if (will_qos > qos_limit) {
    decision.accept = false;
    decision.reason = ReasonCode::QoSNotSupported;
    return decision;
  }
  if (will_retain && !retain_available) {
    decision.accept = false;
    decision.reason = ReasonCode::RetainNotSupported;
  }
  return decision;
}

WillBody will_body_from_connect(const ConnectPacket& packet)
{
  WillBody body;
  body.topic = from_mqtt_string(packet.will_topic);
  body.payload.assign(packet.will_payload.begin(), packet.will_payload.end());
  body.qos = packet.flags.will_qos;
  body.retain = packet.flags.will_retain != 0;
  body.delay_seconds = packet.will_properties.will_delay_interval;
  body.payload_format = packet.will_properties.payload_format_indicator;
  body.has_message_expiry = packet.will_properties.has_message_expiry_interval;
  body.message_expiry = packet.will_properties.message_expiry_interval;
  body.content_type = from_mqtt_string(packet.will_properties.content_type);
  body.response_topic = from_mqtt_string(packet.will_properties.response_topic);
  body.correlation_data.assign(packet.will_properties.correlation_data.begin(),
                               packet.will_properties.correlation_data.end());
  for (size_t i = 0; i < packet.will_properties.user_properties.size(); ++i) {
    body.user_properties.push_back(
        std::make_pair(from_mqtt_string(packet.will_properties.user_properties[i].first),
                       from_mqtt_string(packet.will_properties.user_properties[i].second)));
  }
  return body;
}

CopiedAuth::CopiedAuth(CopiedAuth&& other) noexcept
    : allocator(std::move(other.allocator)), context(other.context)
{
  other.context = nullptr;
}

CopiedAuth& CopiedAuth::operator=(CopiedAuth&& other) noexcept
{
  if (this != &other) {
    if (context && allocator) {
      context->~ClientAuthContext();
      allocator->deallocate(context, sizeof(auth::ClientAuthContext));
    }
    allocator = std::move(other.allocator);
    context = other.context;
    other.context = nullptr;
  }
  return *this;
}

CopiedAuth::~CopiedAuth()
{
  if (context && allocator) {
    context->~ClientAuthContext();
    allocator->deallocate(context, sizeof(auth::ClientAuthContext));
    context = nullptr;
  }
}

bool copy_auth_context(const auth::ClientAuthContext& source, CopiedAuth& out)
{
  try {
    CopiedAuth copied;
    copied.allocator.reset(new MQTTAllocator("will-auth", MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0));
    void* memory = copied.allocator->allocate(sizeof(auth::ClientAuthContext));
    if (!memory) {
      return false;
    }
    auth::ClientAuthContext* context = new (memory) auth::ClientAuthContext(copied.allocator.get());
    copied.context = context;
    context->user_info.username.assign(source.user_info.username.begin(), source.user_info.username.end());
    context->user_info.client_id.assign(source.user_info.client_id.begin(), source.user_info.client_id.end());
    context->user_info.client_ip.assign(source.user_info.client_ip.begin(), source.user_info.client_ip.end());
    context->user_info.client_port = source.user_info.client_port;
    context->user_info.is_super_user = source.user_info.is_super_user;
    context->user_info.expires_at_ms = source.user_info.expires_at_ms;
    context->user_info.provider_name = source.user_info.provider_name;
    context->user_info.authorization_session = source.user_info.authorization_session;
    for (size_t i = 0; i < source.acl_rules.size(); ++i) {
      auth::TopicAclRule rule(copied.allocator.get());
      rule.topic_pattern.assign(source.acl_rules[i].topic_pattern.begin(),
                                source.acl_rules[i].topic_pattern.end());
      rule.permission = source.acl_rules[i].permission;
      rule.max_qos = source.acl_rules[i].max_qos;
      context->acl_rules.push_back(std::move(rule));
    }
    context->acl_version = source.acl_version;
    context->expires_at_ms = source.expires_at_ms;
    context->is_super_user = source.is_super_user;
    out = std::move(copied);
    return out.context != nullptr;
  } catch (const std::exception&) {
    return false;
  }
}

WillPublishOutcome publish_will(GlobalSessionManager* sessions, auth::AuthManager* auth_manager,
                                const auth::ClientAuthContext* auth, const std::string& forward_client_id,
                                const WillBody& body, const std::function<bool()>& still_current)
{
  if (!sessions || forward_client_id.empty() || body.topic.empty()) {
    return WillPublishOutcome::Rejected;
  }
  if (still_current && !still_current()) {
    return WillPublishOutcome::Cancelled;
  }
  runtime::current_runtime().enable_async_syscalls();
  try {
    MQTTAllocator scratch("will-publish", MQTTMemoryTag::MEM_TAG_SESSION_MANAGER, 0);
    MQTTString topic = to_mqtt_string(body.topic, &scratch);
    MQTTByteVector payload = to_mqtt_bytes(body.payload, &scratch);
    if (auth_manager) {
      if (!auth) {
        return WillPublishOutcome::Rejected;
      }
      std::shared_ptr<auth::AuthorizationRequest> pending;
      auth::AuthResult result = auth::AuthResult::ACCESS_DENIED;
      auth_manager->check_publish(*auth, topic, payload, pending, result);
      const std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(kWillAuthWaitMs);
      while (result == auth::AuthResult::PENDING && std::chrono::steady_clock::now() < deadline) {
        if (still_current && !still_current()) {
          return WillPublishOutcome::Cancelled;
        }
        runtime::current_runtime().wait(-1, 0, 1);
        if (pending) {
          result = pending->poll(auth->user_info, topic);
        } else {
          auth_manager->check_publish(*auth, topic, payload, pending, result);
        }
      }
      if (still_current && !still_current()) {
        return WillPublishOutcome::Cancelled;
      }
      if (result != auth::AuthResult::SUCCESS) {
        LOG_INFO("Discarding will for {} on topic {}: authorization {}", forward_client_id, body.topic,
                 static_cast<int>(result));
        return WillPublishOutcome::Rejected;
      }
    }

    PublishPacket packet(&scratch);
    packet.type = PacketType::PUBLISH;
    packet.dup = false;
    packet.qos = body.qos;
    packet.retain = body.retain;
    packet.packet_id = 0;
    packet.topic_name = topic;
    packet.payload = payload;
    if (body.payload_format != 0) {
      packet.properties.payload_format_indicator = body.payload_format;
    }
    if (body.has_message_expiry) {
      packet.properties.has_message_expiry_interval = true;
      packet.properties.message_expiry_interval = body.message_expiry;
    }
    if (!body.content_type.empty()) {
      assign_string(packet.properties.content_type, body.content_type);
    }
    if (!body.response_topic.empty()) {
      assign_string(packet.properties.response_topic, body.response_topic);
    }
    if (!body.correlation_data.empty()) {
      packet.properties.correlation_data.assign(body.correlation_data.begin(), body.correlation_data.end());
    }
    for (size_t i = 0; i < body.user_properties.size(); ++i) {
      packet.properties.user_properties.emplace_back(
          to_mqtt_string(body.user_properties[i].first, &scratch),
          to_mqtt_string(body.user_properties[i].second, &scratch));
    }
    packet.properties.will_delay_interval = 0;
    packet.properties.topic_alias = 0;
    packet.properties.has_topic_alias = false;

    MQTTString sender = to_mqtt_string(forward_client_id, &scratch);
    const int forwarded = sessions->forward_publish_via_router(topic, packet, sender);
    if (forwarded < 0) {
      LOG_WARN("Will publish for {} on topic {} failed: {}", forward_client_id, body.topic, forwarded);
      return WillPublishOutcome::Rejected;
    }
    LOG_INFO("Published will for {} on topic {} to {} recipients", forward_client_id, body.topic,
             forwarded);
    return WillPublishOutcome::Published;
  } catch (const std::exception& error) {
    LOG_WARN("Will publish for {} failed: {}", forward_client_id, error.what());
    return WillPublishOutcome::Rejected;
  }
}

void WillScheduler::schedule(std::unique_ptr<Entry> entry)
{
  if (!entry || entry->client_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  // 新连接的遗嘱单独编号。已经到期、等待发布的上一代不能被这次插入清掉。
  ClientGenerations& state = generations_[entry->client_id];
  uint64_t generation = ++state.next;
  if (generation == 0) {
    generation = ++state.next;
  }
  entry->generation = generation;
  state.live.insert(generation);
  const std::thread::id owner = entry->owner;
  const std::chrono::steady_clock::time_point due = entry->due;
  Queued queued;
  queued.owner = owner;
  queued.position = queues_[owner].emplace(due, std::move(entry));
  state.queued.push_back(queued);
}

void WillScheduler::forget_queued(ClientGenerations& state, const Entry* entry)
{
  for (size_t i = 0; i < state.queued.size(); ++i) {
    if (state.queued[i].position->second.get() == entry) {
      state.queued[i] = state.queued.back();
      state.queued.pop_back();
      return;
    }
  }
}

void WillScheduler::suppress_pending(const std::string& client_id,
                                    std::chrono::steady_clock::time_point now)
{
  const std::unordered_map<std::string, ClientGenerations>::iterator state =
      generations_.find(client_id);
  if (state == generations_.end()) {
    return;
  }
  std::vector<Queued>& queued = state->second.queued;
  for (size_t i = 0; i < queued.size();) {
    Entry* entry = queued[i].position->second.get();
    if (entry->due > now) {
      state->second.live.erase(entry->generation);
      DueQueue& queue = queues_[queued[i].owner];
      queue.erase(queued[i].position);
      if (queue.empty()) {
        queues_.erase(queued[i].owner);
      }
      queued[i] = queued.back();
      queued.pop_back();
      continue;
    }
    ++i;
  }
  drop_client_if_idle(client_id);
}

void WillScheduler::drop_client_if_idle(const std::string& client_id)
{
  const std::unordered_map<std::string, ClientGenerations>::iterator state = generations_.find(client_id);
  if (state == generations_.end()) {
    return;
  }
  if (state->second.queued.empty() && state->second.live.empty() &&
      state->second.inflight.empty()) {
    generations_.erase(state);
  }
}

void WillScheduler::cancel(const std::string& client_id)
{
  if (client_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  suppress_pending(client_id, std::chrono::steady_clock::now());
}

void WillScheduler::expire_now(const std::string& client_id)
{
  if (client_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const std::unordered_map<std::string, ClientGenerations>::iterator state =
      generations_.find(client_id);
  if (state == generations_.end()) {
    return;
  }
  const std::chrono::steady_clock::time_point immediately =
      std::chrono::steady_clock::time_point::min();
  for (size_t i = 0; i < state->second.queued.size(); ++i) {
    Queued& queued = state->second.queued[i];
    DueQueue& queue = queues_[queued.owner];
    std::unique_ptr<Entry> entry = std::move(queued.position->second);
    queue.erase(queued.position);
    entry->due = immediately;
    queued.position = queue.emplace(immediately, std::move(entry));
  }
}

void WillScheduler::observe_connect(const std::string& client_id, bool clean_start,
                                   bool session_present)
{
  if (clean_start || !session_present) {
    expire_now(client_id);
  } else {
    cancel(client_id);
  }
}

void WillScheduler::complete(const std::string& client_id, uint64_t generation)
{
  if (client_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const std::unordered_map<std::string, ClientGenerations>::iterator state = generations_.find(client_id);
  if (state == generations_.end()) {
    return;
  }
  state->second.inflight.erase(generation);
  bool queued = false;
  for (size_t i = 0; i < state->second.queued.size(); ++i) {
    if (state->second.queued[i].position->second->generation == generation) {
      queued = true;
      break;
    }
  }
  if (!queued) {
    state->second.live.erase(generation);
  }
  drop_client_if_idle(client_id);
}

std::vector<std::unique_ptr<WillScheduler::Entry>> WillScheduler::take_due(
    std::thread::id owner, std::chrono::steady_clock::time_point now, bool flush_all)
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::unique_ptr<Entry>> due;
  const std::unordered_map<std::thread::id, DueQueue>::iterator found = queues_.find(owner);
  if (found == queues_.end()) {
    return due;
  }
  DueQueue& queue = found->second;
  while (!queue.empty() && (flush_all || queue.begin()->first <= now)) {
    const std::unordered_map<std::string, ClientGenerations>::iterator state =
        generations_.find(queue.begin()->second->client_id);
    if (state != generations_.end()) {
      forget_queued(state->second, queue.begin()->second.get());
    }
    std::unique_ptr<Entry> entry = std::move(queue.begin()->second);
    queue.erase(queue.begin());
    if (state == generations_.end()) {
      continue;
    }
    if (state->second.live.find(entry->generation) == state->second.live.end()) {
      drop_client_if_idle(entry->client_id);
      continue;
    }
    state->second.inflight.insert(entry->generation);
    due.push_back(std::move(entry));
  }
  if (queue.empty()) {
    queues_.erase(found);
  }
  return due;
}

bool WillScheduler::generation_matches(const std::string& client_id, uint64_t generation) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const std::unordered_map<std::string, ClientGenerations>::const_iterator found =
      generations_.find(client_id);
  return found != generations_.end() &&
         found->second.live.find(generation) != found->second.live.end();
}

void launch_due_wills(GlobalSessionManager* sessions, auth::AuthManager* auth, std::thread::id owner,
                      bool flush_all)
{
  if (!sessions) {
    return;
  }
  std::vector<std::unique_ptr<WillScheduler::Entry>> due =
      sessions->will_scheduler().take_due(owner, std::chrono::steady_clock::now(), flush_all);
  for (size_t i = 0; i < due.size(); ++i) {
    WillFire* fire = new WillFire();
    fire->sessions = sessions;
    fire->auth = auth;
    fire->entry = std::move(due[i]);
    runtime::TaskHandle handle =
        runtime::current_runtime().spawn(run_will_fire, fire, 128 * 1024);
    if (!handle.is_valid()) {
      const std::string client_id = fire->entry->client_id;
      const uint64_t generation = fire->entry->generation;
      delete fire;
      sessions->will_scheduler().complete(client_id, generation);
      sessions->note_will_rejected();
      continue;
    }
    handle.release();
  }
}

}  // namespace mqtt
