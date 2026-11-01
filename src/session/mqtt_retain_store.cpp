#include "mqtt_retain_store.h"

#include <algorithm>
#include "logger.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {

RetainStore::RetainStore(MQTTAllocator* allocator)
    : allocator_(allocator),
      message_count_(0),
      total_bytes_(0),
      warned_(false)
{
}

RetainStore::~RetainStore()
{
  for (std::unordered_map<std::string, Node*>::iterator it = root_.children.begin();
       it != root_.children.end(); ++it) {
    destroy_node(it->second);
  }
  root_.children.clear();
  root_.content.reset();
}

void RetainStore::destroy_node(Node* node)
{
  if (!node) {
    return;
  }
  for (std::unordered_map<std::string, Node*>::iterator it = node->children.begin();
       it != node->children.end(); ++it) {
    destroy_node(it->second);
  }
  node->children.clear();
  node->content.reset();
  delete node;
}

std::vector<std::string> RetainStore::split(const std::string& topic)
{
  std::vector<std::string> parts;
  std::string current;
  for (size_t i = 0; i < topic.size(); ++i) {
    if (topic[i] == '/') {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(topic[i]);
    }
  }
  parts.push_back(current);
  return parts;
}

std::string RetainStore::join_topic(const std::string& parent, const std::string& segment, bool at_root)
{
  if (at_root) {
    return segment;
  }
  return parent + "/" + segment;
}

RetainStore::Node* RetainStore::descend(Node* node, const std::vector<std::string>& segments, bool create)
{
  for (size_t i = 0; i < segments.size(); ++i) {
    std::unordered_map<std::string, Node*>::iterator found = node->children.find(segments[i]);
    if (found == node->children.end()) {
      if (!create) {
        return nullptr;
      }
      Node* child = new Node();
      node->children.insert(std::make_pair(segments[i], child));
      node = child;
    } else {
      node = found->second;
    }
  }
  return node;
}

void RetainStore::unindex(const Node* node, const std::string& topic)
{
  if (node->has_message && node->has_expiry) {
    expiry_.erase(ExpiryKey(node->expires_at, topic));
  }
}

void RetainStore::account_removed(size_t bytes)
{
  if (message_count_ > 0) {
    --message_count_;
  }
  if (total_bytes_ >= bytes) {
    total_bytes_ -= bytes;
  } else {
    total_bytes_ = 0;
  }
}

bool RetainStore::erase_at(Node* node, const std::string& topic, const std::vector<std::string>& segments,
                           size_t index, size_t& bytes)
{
  if (index == segments.size()) {
    if (!node->has_message) {
      return false;
    }
    unindex(node, topic);
    bytes = node->payload_bytes;
    node->content.reset();
    node->has_message = false;
    node->has_expiry = false;
    node->payload_bytes = 0;
    return true;
  }
  std::unordered_map<std::string, Node*>::iterator found = node->children.find(segments[index]);
  if (found == node->children.end()) {
    return false;
  }
  size_t removed = 0;
  const bool erased = erase_at(found->second, topic, segments, index + 1, removed);
  if (erased && found->second->children.empty() && !found->second->has_message) {
    delete found->second;
    node->children.erase(found);
  }
  bytes = removed;
  return erased;
}

const char* RetainStore::warn_locked(const char* reason)
{
  const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  if (warned_ && now - last_warn_ < std::chrono::seconds(1)) {
    return nullptr;
  }
  warned_ = true;
  last_warn_ = now;
  return reason;
}

bool RetainStore::peek(const std::string& topic, RetainedCopy& out,
                       std::chrono::steady_clock::time_point now) const
{
  if (topic.empty()) {
    return false;
  }
  ReadLockGuard lock(mutex_);
  const Node* node = &root_;
  const std::vector<std::string> segments = split(topic);
  for (size_t i = 0; i < segments.size(); ++i) {
    std::unordered_map<std::string, Node*>::const_iterator found = node->children.find(segments[i]);
    if (found == node->children.end()) {
      return false;
    }
    node = found->second;
  }
  if (!node->has_message || (node->has_expiry && now >= node->expires_at)) {
    return false;
  }
  out.topic = topic;
  out.content = node->content;
  out.has_expiry = node->has_expiry;
  out.expires_at = node->expires_at;
  return true;
}

RetainWriteResult RetainStore::install(const RetainedCopy& copy)
{
  RetainWriteResult result;
  if (copy.topic.empty() || !copy.content.is_valid()) {
    return result;
  }
  const size_t payload_bytes = copy.content->payload.size();
  const std::vector<std::string> segments = split(copy.topic);
  WriteLockGuard lock(mutex_);
  Node* existing = descend(&root_, segments, false);
  const bool replacing = existing && existing->has_message;
  const size_t old_bytes = replacing ? existing->payload_bytes : 0;
  Node* node = existing;
  if (!node) {
    node = descend(&root_, segments, true);
  }
  if (copy.has_expiry) {
    expiry_.insert(ExpiryKey(copy.expires_at, copy.topic));
  }
  if (replacing && !(copy.has_expiry && node->has_expiry && node->expires_at == copy.expires_at)) {
    unindex(node, copy.topic);
  }
  node->content = copy.content;
  node->has_message = true;
  node->has_expiry = copy.has_expiry;
  node->expires_at = copy.expires_at;
  node->payload_bytes = payload_bytes;
  message_count_ = message_count_ - (replacing ? 1 : 0) + 1;
  if (total_bytes_ >= old_bytes) {
    total_bytes_ = total_bytes_ - old_bytes + payload_bytes;
  } else {
    total_bytes_ = payload_bytes;
  }
  result.kind = replacing ? RetainWriteResult::Replaced : RetainWriteResult::Stored;
  result.message_delta = replacing ? 0 : 1;
  result.byte_delta = static_cast<int64_t>(payload_bytes) - static_cast<int64_t>(old_bytes);
  return result;
}

RetainWriteResult RetainStore::remove(const std::string& topic)
{
  RetainWriteResult result;
  if (topic.empty()) {
    return result;
  }
  WriteLockGuard lock(mutex_);
  size_t bytes = 0;
  if (!erase_at(&root_, topic, split(topic), 0, bytes)) {
    return result;
  }
  account_removed(bytes);
  result.kind = RetainWriteResult::Removed;
  result.message_delta = -1;
  result.byte_delta = -static_cast<int64_t>(bytes);
  return result;
}

RetainWriteResult RetainStore::store(const PublishPacket& packet, const MQTTString& sender,
                                     const RetainConfig& limits, std::chrono::steady_clock::time_point now)
{
  RetainWriteResult result;
  const std::string topic = from_mqtt_string(packet.topic_name);
  if (topic.empty() || topic.find('+') != std::string::npos || topic.find('#') != std::string::npos) {
    return result;
  }
  if (packet.payload.empty()) {
    return remove(topic);
  }
  // 过期间隔为 0：先替换再立刻删除，不能把旧值留在库里。
  if (packet.properties.has_message_expiry_interval && packet.properties.message_expiry_interval == 0) {
    return remove(topic);
  }

  const size_t payload_bytes = packet.payload.size();
  const size_t payload_limit = limits.max_payload_bytes == 0 ? size_t(1024 * 1024) : limits.max_payload_bytes;
  if (payload_bytes > payload_limit) {
    WriteLockGuard lock(mutex_);
    result.kind = RetainWriteResult::Rejected;
    result.warn = warn_locked("payload exceeds max_payload_bytes");
    return result;
  }

  SharedMessageContentPtr content;
  bool has_expiry = false;
  std::chrono::steady_clock::time_point expires_at;
  try {
    content = SharedMessageContentPtr(new SharedMessageContent(
        packet.topic_name, packet.payload, packet.qos, true, false, packet.properties, sender, allocator_));
    if (packet.properties.has_message_expiry_interval) {
      has_expiry = true;
      expires_at = now + std::chrono::seconds(packet.properties.message_expiry_interval);
    }
  } catch (const std::bad_alloc&) {
    WriteLockGuard lock(mutex_);
    result.kind = RetainWriteResult::Rejected;
    result.warn = warn_locked("out of memory");
    return result;
  }

  const std::vector<std::string> segments = split(topic);
  WriteLockGuard lock(mutex_);
  int64_t reclaimed_messages = 0;
  int64_t reclaimed_bytes = 0;
  reclaim_expired(now, reclaimed_messages, reclaimed_bytes);
  Node* existing = descend(&root_, segments, false);
  const bool replacing = existing && existing->has_message;
  const size_t old_bytes = replacing ? existing->payload_bytes : 0;
  const size_t new_count = message_count_ - (replacing ? 1 : 0) + 1;
  const size_t new_bytes = total_bytes_ - old_bytes + payload_bytes;
  const char* reject = nullptr;
  if (new_count > limits.max_messages) {
    reject = "retained message count limit";
  } else if (new_bytes > limits.max_total_bytes) {
    reject = "retained byte limit";
  }
  if (reject) {
    result.kind = RetainWriteResult::Rejected;
    result.message_delta = reclaimed_messages;
    result.byte_delta = reclaimed_bytes;
    result.warn = warn_locked(reject);
    return result;
  }
  Node* node = existing;
  if (!node) {
    try {
      node = descend(&root_, segments, true);
    } catch (const std::bad_alloc&) {
      result.kind = RetainWriteResult::Rejected;
      result.warn = warn_locked("out of memory");
      return result;
    }
  }

  const bool same_key = replacing && node->has_expiry && has_expiry && node->expires_at == expires_at;
  if (has_expiry && !same_key) {
    try {
      expiry_.insert(ExpiryKey(expires_at, topic));
    } catch (const std::bad_alloc&) {
      result.kind = RetainWriteResult::Rejected;
      result.message_delta = reclaimed_messages;
      result.byte_delta = reclaimed_bytes;
      result.warn = warn_locked("out of memory");
      return result;
    }
  }
  if (replacing && !same_key) {
    unindex(node, topic);
  }
  node->content = content;
  node->has_message = true;
  node->has_expiry = has_expiry;
  node->expires_at = expires_at;
  node->payload_bytes = payload_bytes;
  message_count_ = new_count;
  total_bytes_ = new_bytes;
  result.kind = replacing ? RetainWriteResult::Replaced : RetainWriteResult::Stored;
  result.message_delta = reclaimed_messages + (replacing ? 0 : 1);
  result.byte_delta =
      reclaimed_bytes + static_cast<int64_t>(payload_bytes) - static_cast<int64_t>(old_bytes);
  return result;
}

void RetainStore::offer(MatchPick& pick, RetainedCopy copy) const
{
  if (pick.limit == 0) {
    pick.truncated = true;
    return;
  }
  if (pick.best.size() < pick.limit) {
    pick.best.push(std::move(copy));
    return;
  }
  if (copy.topic < pick.best.top().topic) {
    pick.best.pop();
    pick.best.push(std::move(copy));
  }
  pick.truncated = true;
}

void RetainStore::consider(Node* node, const std::string& topic, std::chrono::steady_clock::time_point now,
                           MatchPick& pick, std::vector<std::string>& expired) const
{
  if (!node || !node->has_message || topic.empty()) {
    return;
  }
  if (node->has_expiry && now >= node->expires_at) {
    expired.push_back(topic);
    return;
  }
  RetainedCopy copy;
  copy.topic = topic;
  copy.content = node->content;
  copy.has_expiry = node->has_expiry;
  copy.expires_at = node->expires_at;
  offer(pick, std::move(copy));
}

void RetainStore::collect(Node* node, const std::string& topic, bool at_root,
                          std::chrono::steady_clock::time_point now, MatchPick& pick,
                          std::vector<std::string>& expired) const
{
  consider(node, topic, now, pick, expired);
  for (std::unordered_map<std::string, Node*>::const_iterator it = node->children.begin();
       it != node->children.end(); ++it) {
    if (at_root && !it->first.empty() && it->first[0] == '$') {
      continue;
    }
    collect(it->second, join_topic(topic, it->first, at_root), false, now, pick, expired);
  }
}

void RetainStore::walk(Node* node, const std::string& topic, const std::vector<std::string>& filter,
                       size_t index, bool root_level, std::chrono::steady_clock::time_point now,
                       MatchPick& pick, std::vector<std::string>& expired) const
{
  if (!node) {
    return;
  }
  if (index == filter.size()) {
    consider(node, topic, now, pick, expired);
    return;
  }
  const std::string& segment = filter[index];
  if (segment == "#") {
    if (index + 1 != filter.size()) {
      return;
    }
    collect(node, topic, root_level, now, pick, expired);
    return;
  }
  if (segment == "+") {
    for (std::unordered_map<std::string, Node*>::const_iterator it = node->children.begin();
         it != node->children.end(); ++it) {
      if (root_level && !it->first.empty() && it->first[0] == '$') {
        continue;
      }
      walk(it->second, join_topic(topic, it->first, root_level), filter, index + 1, false, now, pick,
           expired);
    }
    return;
  }
  std::unordered_map<std::string, Node*>::const_iterator found = node->children.find(segment);
  if (found == node->children.end()) {
    return;
  }
  walk(found->second, join_topic(topic, segment, root_level), filter, index + 1, false, now, pick, expired);
}

void RetainStore::reclaim_expired(std::chrono::steady_clock::time_point now, int64_t& message_delta,
                                  int64_t& byte_delta)
{
  while (!expiry_.empty() && expiry_.begin()->first <= now) {
    const std::string topic = expiry_.begin()->second;
    size_t bytes = 0;
    if (!erase_at(&root_, topic, split(topic), 0, bytes)) {
      expiry_.erase(expiry_.begin());
      continue;
    }
    account_removed(bytes);
    --message_delta;
    byte_delta -= static_cast<int64_t>(bytes);
  }
}

void RetainStore::purge(const std::vector<std::string>& topics, std::chrono::steady_clock::time_point now,
                        int64_t& message_delta, int64_t& byte_delta)
{
  message_delta = 0;
  byte_delta = 0;
  if (topics.empty()) {
    return;
  }
  WriteLockGuard lock(mutex_);
  for (size_t i = 0; i < topics.size(); ++i) {
    Node* node = descend(&root_, split(topics[i]), false);
    if (!node || !node->has_message || !node->has_expiry || now < node->expires_at) {
      continue;
    }
    size_t bytes = 0;
    if (!erase_at(&root_, topics[i], split(topics[i]), 0, bytes)) {
      continue;
    }
    account_removed(bytes);
    --message_delta;
    byte_delta -= static_cast<int64_t>(bytes);
  }
}

RetainMatchResult RetainStore::match(const std::string& filter, size_t limit,
                                     std::chrono::steady_clock::time_point now)
{
  RetainMatchResult result;
  if (filter.empty()) {
    return result;
  }
  std::vector<std::string> expired;
  MatchPick pick;
  pick.limit = limit;
  {
    ReadLockGuard lock(mutex_);
    walk(&root_, "", split(filter), 0, true, now, pick, expired);
  }
  purge(expired, now, result.message_delta, result.byte_delta);
  result.truncated = pick.truncated;
  result.messages.reserve(pick.best.size());
  while (!pick.best.empty()) {
    result.messages.push_back(pick.best.top());
    pick.best.pop();
  }
  std::sort(result.messages.begin(), result.messages.end(),
            [](const RetainedCopy& left, const RetainedCopy& right) { return left.topic < right.topic; });
  return result;
}

size_t RetainStore::message_count() const
{
  ReadLockGuard lock(mutex_);
  return message_count_;
}

size_t RetainStore::total_bytes() const
{
  ReadLockGuard lock(mutex_);
  return total_bytes_;
}

}  // namespace mqtt
