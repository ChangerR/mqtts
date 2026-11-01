#ifndef MQTT_RETAIN_STORE_H
#define MQTT_RETAIN_STORE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "mqtt_config.h"
#include "mqtt_message_queue.h"
#include "pthread_rwlock_wrapper.h"

namespace mqtt {

// Retain Handling: 0 总是下发，1 仅新订阅下发，2 不下发。
inline bool retain_handling_delivers(uint8_t retain_handling, bool subscription_existed)
{
  if (retain_handling >= 2) {
    return false;
  }
  if (retain_handling == 1 && subscription_existed) {
    return false;
  }
  return true;
}

struct RetainWriteResult
{
  enum Kind { Stored, Replaced, Removed, Absent, Rejected };
  Kind kind;
  int64_t message_delta;
  int64_t byte_delta;
  // 非空时调用方打一条限频 WARN。字面量，不指向库内缓冲。
  const char* warn;

  RetainWriteResult() : kind(Absent), message_delta(0), byte_delta(0), warn(nullptr) {}
};

struct RetainedCopy
{
  std::string topic;
  SharedMessageContentPtr content;
  bool has_expiry;
  std::chrono::steady_clock::time_point expires_at;

  RetainedCopy() : has_expiry(false) {}
};

struct RetainedTopicOrder
{
  bool operator()(const RetainedCopy& left, const RetainedCopy& right) const
  {
    return left.topic < right.topic;
  }
};

struct RetainMatchResult
{
  std::vector<RetainedCopy> messages;
  bool truncated;
  // 读到过期项时顺手删除，调用方把差值记到本线程计数上。
  int64_t message_delta;
  int64_t byte_delta;

  RetainMatchResult() : truncated(false), message_delta(0), byte_delta(0) {}
};

// 进程内保留库。按主题层级的 trie 做“过滤器 → 主题”反向匹配。
// 内容是绑在保留分配器上的 SharedMessageContent。读写锁内只做 trie 和指针拷贝。
class RetainStore
{
 public:
  explicit RetainStore(MQTTAllocator* allocator);
  ~RetainStore();

  RetainStore(const RetainStore&) = delete;
  RetainStore& operator=(const RetainStore&) = delete;

  RetainWriteResult store(const PublishPacket& packet, const MQTTString& sender,
                          const RetainConfig& limits, std::chrono::steady_clock::time_point now);
  RetainWriteResult remove(const std::string& topic);
  // 日志提交失败时把内存改回调用 store 之前的值。过期项视为不存在。
  bool peek(const std::string& topic, RetainedCopy& out, std::chrono::steady_clock::time_point now) const;
  RetainWriteResult install(const RetainedCopy& copy);
  // 匹配非过期消息。遍历时只保留字典序最小的 limit 条。过期项在读锁外用写锁清掉。
  RetainMatchResult match(const std::string& filter, size_t limit,
                          std::chrono::steady_clock::time_point now);

  size_t message_count() const;
  size_t total_bytes() const;

 private:
  struct Node
  {
    std::unordered_map<std::string, Node*> children;
    SharedMessageContentPtr content;
    bool has_message;
    bool has_expiry;
    std::chrono::steady_clock::time_point expires_at;
    size_t payload_bytes;

    Node() : has_message(false), has_expiry(false), payload_bytes(0) {}
  };

  typedef std::pair<std::chrono::steady_clock::time_point, std::string> ExpiryKey;

  MQTTAllocator* allocator_;
  mutable RWMutex mutex_;
  Node root_;
  // 带过期时间的条目按到期先后排序，写入时只回收已到期的，不扫整棵树。
  std::set<ExpiryKey> expiry_;
  size_t message_count_;
  size_t total_bytes_;
  bool warned_;
  std::chrono::steady_clock::time_point last_warn_;

  // 字典序最大的主题在堆顶，堆大小不超过本次投递上限。
  struct MatchPick
  {
    size_t limit;
    bool truncated;
    std::priority_queue<RetainedCopy, std::vector<RetainedCopy>, RetainedTopicOrder> best;

    MatchPick() : limit(0), truncated(false) {}
  };

  static std::vector<std::string> split(const std::string& topic);
  // at_root 为真时 parent 是 trie 根，不能用 parent.empty() 判断，否则前导空层级会被吃掉。
  static std::string join_topic(const std::string& parent, const std::string& segment, bool at_root);
  Node* descend(Node* node, const std::vector<std::string>& segments, bool create);
  bool erase_at(Node* node, const std::string& topic, const std::vector<std::string>& segments,
                size_t index, size_t& bytes);
  void unindex(const Node* node, const std::string& topic);
  void account_removed(size_t bytes);
  void destroy_node(Node* node);
  const char* warn_locked(const char* reason);
  void offer(MatchPick& pick, RetainedCopy copy) const;
  void consider(Node* node, const std::string& topic, std::chrono::steady_clock::time_point now,
                MatchPick& pick, std::vector<std::string>& expired) const;
  void collect(Node* node, const std::string& topic, bool at_root,
               std::chrono::steady_clock::time_point now, MatchPick& pick,
               std::vector<std::string>& expired) const;
  void walk(Node* node, const std::string& topic, const std::vector<std::string>& filter, size_t index,
            bool root_level, std::chrono::steady_clock::time_point now, MatchPick& pick,
            std::vector<std::string>& expired) const;
  void reclaim_expired(std::chrono::steady_clock::time_point now, int64_t& message_delta,
                       int64_t& byte_delta);
  void purge(const std::vector<std::string>& topics, std::chrono::steady_clock::time_point now,
             int64_t& message_delta, int64_t& byte_delta);
};

}  // namespace mqtt

#endif  // MQTT_RETAIN_STORE_H
