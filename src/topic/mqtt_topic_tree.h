#ifndef MQTT_TOPIC_TREE_H
#define MQTT_TOPIC_TREE_H

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_define.h"
#include "mqtt_parser.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {

class TopicTreeNode;
class IntermediateNode;

void intrusive_ptr_add_ref(TopicTreeNode* node);
void intrusive_ptr_release(TopicTreeNode* node);
void intrusive_ptr_add_ref(IntermediateNode* node);
void intrusive_ptr_release(IntermediateNode* node);

template <typename T>
using TopicTreeAllocator = MQTTSTLAllocator<T>;

class TopicTreeNodePtr
{
 public:
  TopicTreeNodePtr() : node_(nullptr) {}

  explicit TopicTreeNodePtr(TopicTreeNode* node) : node_(node)
  {
    intrusive_ptr_add_ref(node_);
  }

  TopicTreeNodePtr(const TopicTreeNodePtr& other) : node_(other.node_)
  {
    intrusive_ptr_add_ref(node_);
  }

  TopicTreeNodePtr(TopicTreeNodePtr&& other) noexcept : node_(other.node_)
  {
    other.node_ = nullptr;
  }

  ~TopicTreeNodePtr()
  {
    intrusive_ptr_release(node_);
  }

  TopicTreeNodePtr& operator=(TopicTreeNodePtr other) noexcept
  {
    swap(other);
    return *this;
  }

  static TopicTreeNodePtr adopt(TopicTreeNode* node)
  {
    TopicTreeNodePtr ptr;
    ptr.node_ = node;
    return ptr;
  }

  void swap(TopicTreeNodePtr& other) noexcept
  {
    std::swap(node_, other.node_);
  }

  TopicTreeNode* get() const { return node_; }
  TopicTreeNode* operator->() const { return node_; }
  TopicTreeNode& operator*() const { return *node_; }
  explicit operator bool() const { return node_ != nullptr; }
  bool operator==(const TopicTreeNodePtr& other) const { return node_ == other.node_; }
  bool operator!=(const TopicTreeNodePtr& other) const { return node_ != other.node_; }

 private:
  TopicTreeNode* node_;
};

template <typename Key, typename Value>
using TopicTreeMap = std::unordered_map<Key, Value, std::hash<Key>, std::equal_to<Key>,
                                        TopicTreeAllocator<std::pair<const Key, Value>>>;

template <typename Value>
using TopicTreeSet =
    std::unordered_set<Value, std::hash<Value>, std::equal_to<Value>, TopicTreeAllocator<Value>>;

template <typename Value>
using TopicTreeVector = std::vector<Value, TopicTreeAllocator<Value>>;

using TopicTreeChildrenMap = TopicTreeMap<MQTTString, TopicTreeNodePtr>;
using TopicTreeLevelVector = TopicTreeVector<MQTTString>;

MQTTString clone_to_tree_string(const MQTTString& input, MQTTAllocator* tree_allocator);

struct SubscriberInfo
{
  MQTTString client_id;
  uint8_t qos;

  explicit SubscriberInfo(MQTTAllocator* allocator = nullptr);
  SubscriberInfo(const MQTTString& id, uint8_t q, MQTTAllocator* allocator);

  bool operator==(const SubscriberInfo& other) const;
};

struct SubscriberInfoHash
{
  size_t operator()(const SubscriberInfo& info) const;
};

using SubscriberSet =
    std::unordered_set<SubscriberInfo, SubscriberInfoHash, std::equal_to<SubscriberInfo>,
                       TopicTreeAllocator<SubscriberInfo>>;

class TopicTreeNode
{
 public:
  explicit TopicTreeNode(MQTTAllocator* allocator);
  ~TopicTreeNode();

  int init();

  TopicTreeNode(const TopicTreeNode&) = delete;
  TopicTreeNode& operator=(const TopicTreeNode&) = delete;

  int get_child(const MQTTString& level, TopicTreeNodePtr& child) const;
  int get_children(TopicTreeChildrenMap& children) const;
  int get_subscribers(SubscriberSet& subscribers) const;
  int has_subscribers(bool& has_subs) const;
  int has_children(bool& has_childs) const;

  MQTTAllocator* get_allocator() const { return allocator_; }

 private:
  friend class ConcurrentTopicTree;
  friend class IntermediateNode;
  friend void intrusive_ptr_add_ref(TopicTreeNode* node);
  friend void intrusive_ptr_release(TopicTreeNode* node);

  MQTTAllocator* allocator_;
  std::atomic<IntermediateNode*> intermediate_node_;
  std::atomic<int> ref_count_;

  IntermediateNode* get_intermediate_node() const;
  bool compare_and_swap_intermediate_node(IntermediateNode* expected, IntermediateNode* desired);
};

class IntermediateNode
{
 public:
  explicit IntermediateNode(MQTTAllocator* allocator);
  ~IntermediateNode();

  IntermediateNode(const IntermediateNode&) = delete;
  IntermediateNode& operator=(const IntermediateNode&) = delete;

  int clone(IntermediateNode*& cloned_node) const;
  int add_child(const MQTTString& level, const TopicTreeNodePtr& child);
  int remove_child(const MQTTString& level);
  int add_subscriber(const SubscriberInfo& subscriber);
  int remove_subscriber(const MQTTString& client_id);
  int get_child(const MQTTString& level, TopicTreeNodePtr& child) const;
  int get_children(const TopicTreeChildrenMap*& children) const;
  int get_subscribers(const SubscriberSet*& subscribers) const;
  MQTTAllocator* get_allocator() const { return allocator_; }
  int safe_copy_containers(IntermediateNode* dest) const;

 private:
  MQTTAllocator* allocator_;
  TopicTreeChildrenMap children_;
  SubscriberSet subscribers_;
  std::atomic<int> ref_count_;

  friend void intrusive_ptr_add_ref(IntermediateNode* node);
  friend void intrusive_ptr_release(IntermediateNode* node);
};

struct TopicMatchResult
{
  TopicTreeVector<SubscriberInfo> subscribers;
  size_t total_count;

  explicit TopicMatchResult(MQTTAllocator* allocator)
      : subscribers(TopicTreeAllocator<SubscriberInfo>(allocator)), total_count(0)
  {
  }
};

class ConcurrentTopicTree
{
 public:
  explicit ConcurrentTopicTree(MQTTAllocator* allocator);
  ~ConcurrentTopicTree();

  ConcurrentTopicTree(const ConcurrentTopicTree&) = delete;
  ConcurrentTopicTree& operator=(const ConcurrentTopicTree&) = delete;

  int subscribe(const MQTTString& topic_filter, const MQTTString& client_id, uint8_t qos = 0);
  int unsubscribe(const MQTTString& topic_filter, const MQTTString& client_id);
  int unsubscribe_all(const MQTTString& client_id);
  int find_subscribers(const MQTTString& topic, TopicMatchResult& result) const;
  int get_total_subscribers(size_t& total_count) const;
  int get_total_nodes(size_t& total_count) const;
  int cleanup_empty_nodes(size_t& cleaned_count);
  int get_client_subscriptions(const MQTTString& client_id,
                               TopicTreeLevelVector& subscriptions) const;
  MQTTAllocator* get_allocator() const { return allocator_; }
  bool is_initialized() const { return static_cast<bool>(root_); }
  int get_memory_usage(size_t& memory_usage) const;

 private:
  MQTTAllocator* allocator_;
  TopicTreeNodePtr root_;
  mutable std::atomic<size_t> total_subscribers_;
  mutable std::atomic<size_t> total_nodes_;
  mutable std::mutex node_creation_mutex_;
  MQTTString single_level_wildcard_;
  MQTTString multi_level_wildcard_;
  MQTTString empty_path_;

  int split_topic_levels(const MQTTString& topic, TopicTreeLevelVector& levels) const;
  int get_or_create_node(const TopicTreeLevelVector& levels, bool create_if_not_exists,
                         TopicTreeNodePtr& node);
  int find_subscribers_recursive(const TopicTreeNodePtr& node,
                                 const TopicTreeLevelVector& topic_levels, size_t level_index,
                                 TopicMatchResult& result) const;
  int add_subscriber_to_node(const TopicTreeNodePtr& node, const SubscriberInfo& subscriber);
  int remove_subscriber_from_node(const TopicTreeNodePtr& node, const MQTTString& client_id);
  int collect_client_subscriptions(const TopicTreeNodePtr& node, const MQTTString& current_path,
                                   const MQTTString& client_id,
                                   TopicTreeLevelVector& result) const;
  int cleanup_empty_nodes_recursive(const TopicTreeNodePtr& node, const MQTTString& path,
                                    bool& should_delete);
};

}  // namespace mqtt

#endif  // MQTT_TOPIC_TREE_H
