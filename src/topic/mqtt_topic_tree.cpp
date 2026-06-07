#include "mqtt_topic_tree.h"
#include <algorithm>
#include <new>
#include <thread>
#include "logger.h"

namespace mqtt {
namespace {

TopicTreeChildrenMap make_children_map(MQTTAllocator* allocator)
{
  return TopicTreeChildrenMap(0, std::hash<MQTTString>(), std::equal_to<MQTTString>(),
                              TopicTreeAllocator<std::pair<const MQTTString, TopicTreeNodePtr>>(
                                  allocator));
}

SubscriberSet make_subscriber_set(MQTTAllocator* allocator)
{
  return SubscriberSet(0, SubscriberInfoHash(), std::equal_to<SubscriberInfo>(),
                       TopicTreeAllocator<SubscriberInfo>(allocator));
}

TopicTreeLevelVector make_level_vector(MQTTAllocator* allocator)
{
  return TopicTreeLevelVector(TopicTreeAllocator<MQTTString>(allocator));
}

void destroy_intermediate_node(IntermediateNode* node)
{
  if (MQ_ISNULL(node)) {
    return;
  }

  MQTTAllocator* allocator = node->get_allocator();
  node->~IntermediateNode();
  allocator->deallocate(node, sizeof(IntermediateNode));
}

void destroy_topic_tree_node(TopicTreeNode* node)
{
  if (MQ_ISNULL(node)) {
    return;
  }

  MQTTAllocator* allocator = node->get_allocator();
  node->~TopicTreeNode();
  allocator->deallocate(node, sizeof(TopicTreeNode));
}

int create_initialized_node(MQTTAllocator* allocator, TopicTreeNodePtr& node_ptr)
{
  if (MQ_ISNULL(allocator)) {
    return MQ_ERR_PARAM_V2;
  }

  void* memory = allocator->allocate(sizeof(TopicTreeNode));
  if (MQ_ISNULL(memory)) {
    LOG_ERROR("Failed to allocate memory for TopicTreeNode");
    return MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
  }

  TopicTreeNode* node = new (memory) TopicTreeNode(allocator);
  int ret = node->init();
  if (MQ_FAIL(ret)) {
    destroy_topic_tree_node(node);
    return ret;
  }

  node_ptr = TopicTreeNodePtr::adopt(node);
  return MQ_SUCCESS;
}

template <typename Container>
MQTTAllocator* container_allocator(const Container& container)
{
  return container.get_allocator().get_allocator();
}

MQTTString join_topic_path(const MQTTString& current_path, const MQTTString& child,
                           MQTTAllocator* allocator)
{
  MQTTString result{MQTTStrAllocator(allocator)};
  if (current_path.empty()) {
    result.assign(child.begin(), child.end());
    return result;
  }

  result.reserve(current_path.size() + 1 + child.size());
  result.append(current_path.begin(), current_path.end());
  result.push_back('/');
  result.append(child.begin(), child.end());
  return result;
}

int validate_topic_filter(const MQTTString& topic_filter)
{
  if (topic_filter.empty()) {
    return MQ_ERR_PARAM_V2;
  }

  for (size_t i = 0; i < topic_filter.size(); ++i) {
    char ch = topic_filter[i];
    if (ch == '#') {
      bool valid_prefix = (i == 0) || (topic_filter[i - 1] == '/');
      bool valid_suffix = (i == topic_filter.size() - 1);
      if (!valid_prefix || !valid_suffix) {
        return MQ_ERR_PARAM_V2;
      }
    } else if (ch == '+') {
      bool valid_prefix = (i == 0) || (topic_filter[i - 1] == '/');
      bool valid_suffix = (i == topic_filter.size() - 1) || (topic_filter[i + 1] == '/');
      if (!valid_prefix || !valid_suffix) {
        return MQ_ERR_PARAM_V2;
      }
    }
  }

  return MQ_SUCCESS;
}

}  // namespace

MQTTString clone_to_tree_string(const MQTTString& input, MQTTAllocator* tree_allocator)
{
  return MQTTString(input.begin(), input.end(), MQTTStrAllocator(tree_allocator));
}

SubscriberInfo::SubscriberInfo(MQTTAllocator* allocator)
    : client_id(MQTTStrAllocator(allocator)), qos(0)
{
}

SubscriberInfo::SubscriberInfo(const MQTTString& id, uint8_t q, MQTTAllocator* allocator)
    : client_id(clone_to_tree_string(id, allocator)), qos(q)
{
}

bool SubscriberInfo::operator==(const SubscriberInfo& other) const
{
  return client_id == other.client_id;
}

size_t SubscriberInfoHash::operator()(const SubscriberInfo& info) const
{
  return std::hash<MQTTString>{}(info.client_id);
}

IntermediateNode::IntermediateNode(MQTTAllocator* allocator)
    : allocator_(allocator),
      children_(make_children_map(allocator)),
      subscribers_(make_subscriber_set(allocator)),
      ref_count_(1)
{
}

IntermediateNode::~IntermediateNode() {}

int IntermediateNode::safe_copy_containers(IntermediateNode* dest) const
{
  if (MQ_ISNULL(dest)) {
    return MQ_ERR_PARAM_V2;
  }

  for (const auto& child_pair : children_) {
    auto insert_result = dest->children_.insert(child_pair);
    if (!insert_result.second) {
      insert_result.first->second = child_pair.second;
    }
  }

  for (const auto& subscriber : subscribers_) {
    auto insert_result = dest->subscribers_.insert(subscriber);
    if (!insert_result.second) {
      dest->subscribers_.erase(insert_result.first);
      dest->subscribers_.insert(subscriber);
    }
  }

  return MQ_SUCCESS;
}

int IntermediateNode::clone(IntermediateNode*& cloned_node) const
{
  cloned_node = nullptr;
  if (MQ_ISNULL(allocator_)) {
    return MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
  }

  void* memory = allocator_->allocate(sizeof(IntermediateNode));
  if (MQ_ISNULL(memory)) {
    LOG_ERROR("Failed to allocate memory for IntermediateNode");
    return MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
  }

  cloned_node = new (memory) IntermediateNode(allocator_);
  int ret = safe_copy_containers(cloned_node);
  if (MQ_FAIL(ret)) {
    destroy_intermediate_node(cloned_node);
    cloned_node = nullptr;
  }
  return ret;
}

int IntermediateNode::add_child(const MQTTString& level, const TopicTreeNodePtr& child)
{
  if (!child) {
    return MQ_ERR_PARAM_V2;
  }

  MQTTString owned_level = clone_to_tree_string(level, allocator_);
  auto it = children_.find(owned_level);
  if (it != children_.end()) {
    it->second = child;
    return MQ_SUCCESS;
  }

  auto result = children_.insert(std::make_pair(std::move(owned_level), child));
  return result.second ? MQ_SUCCESS : MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
}

int IntermediateNode::remove_child(const MQTTString& level)
{
  auto it = children_.find(level);
  if (it == children_.end()) {
    return MQ_ERR_NOT_FOUND_V2;
  }
  children_.erase(it);
  return MQ_SUCCESS;
}

int IntermediateNode::add_subscriber(const SubscriberInfo& subscriber)
{
  if (subscriber.client_id.empty()) {
    return MQ_ERR_TOPIC_TREE_INVALID_CLIENT;
  }

  SubscriberInfo owned_subscriber(subscriber.client_id, subscriber.qos, allocator_);
  auto existing = subscribers_.find(owned_subscriber);
  if (existing != subscribers_.end()) {
    subscribers_.erase(existing);
  }

  auto result = subscribers_.insert(owned_subscriber);
  return result.second ? MQ_SUCCESS : MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
}

int IntermediateNode::remove_subscriber(const MQTTString& client_id)
{
  if (client_id.empty()) {
    return MQ_ERR_TOPIC_TREE_INVALID_CLIENT;
  }

  SubscriberInfo lookup(client_id, 0, allocator_);
  auto it = subscribers_.find(lookup);
  if (it == subscribers_.end()) {
    return MQ_ERR_TOPIC_TREE_SUBSCRIBER_NOT_FOUND;
  }

  subscribers_.erase(it);
  return MQ_SUCCESS;
}

int IntermediateNode::get_child(const MQTTString& level, TopicTreeNodePtr& child) const
{
  auto it = children_.find(level);
  if (it == children_.end()) {
    child = TopicTreeNodePtr();
    return MQ_ERR_NOT_FOUND_V2;
  }

  child = it->second;
  return MQ_SUCCESS;
}

int IntermediateNode::get_children(const TopicTreeChildrenMap*& children) const
{
  children = &children_;
  return MQ_SUCCESS;
}

int IntermediateNode::get_subscribers(const SubscriberSet*& subscribers) const
{
  subscribers = &subscribers_;
  return MQ_SUCCESS;
}

void intrusive_ptr_add_ref(IntermediateNode* node)
{
  if (node) {
    node->ref_count_.fetch_add(1, std::memory_order_relaxed);
  }
}

void intrusive_ptr_release(IntermediateNode* node)
{
  if (node && node->ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    destroy_intermediate_node(node);
  }
}

TopicTreeNode::TopicTreeNode(MQTTAllocator* allocator)
    : allocator_(allocator), intermediate_node_(nullptr), ref_count_(1)
{
}

TopicTreeNode::~TopicTreeNode()
{
  IntermediateNode* node = intermediate_node_.load(std::memory_order_acquire);
  if (node) {
    intrusive_ptr_release(node);
  }
}

int TopicTreeNode::init()
{
  if (MQ_ISNULL(allocator_)) {
    return MQ_ERR_PARAM_V2;
  }

  void* memory = allocator_->allocate(sizeof(IntermediateNode));
  if (MQ_ISNULL(memory)) {
    LOG_ERROR("Failed to allocate memory for IntermediateNode");
    return MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
  }

  IntermediateNode* node = new (memory) IntermediateNode(allocator_);
  intermediate_node_.store(node, std::memory_order_release);
  return MQ_SUCCESS;
}

IntermediateNode* TopicTreeNode::get_intermediate_node() const
{
  IntermediateNode* node = intermediate_node_.load(std::memory_order_acquire);
  intrusive_ptr_add_ref(node);
  return node;
}

bool TopicTreeNode::compare_and_swap_intermediate_node(IntermediateNode* expected,
                                                       IntermediateNode* desired)
{
  bool success =
      intermediate_node_.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
  if (success) {
    intrusive_ptr_release(expected);
  }
  return success;
}

int TopicTreeNode::get_child(const MQTTString& level, TopicTreeNodePtr& child) const
{
  IntermediateNode* node = get_intermediate_node();
  if (MQ_ISNULL(node)) {
    return MQ_ERR_INTERNAL;
  }

  int ret = node->get_child(level, child);
  intrusive_ptr_release(node);
  return ret;
}

int TopicTreeNode::get_children(TopicTreeChildrenMap& children) const
{
  IntermediateNode* node = get_intermediate_node();
  if (MQ_ISNULL(node)) {
    return MQ_ERR_INTERNAL;
  }

  const TopicTreeChildrenMap* children_ptr = nullptr;
  int ret = node->get_children(children_ptr);
  if (MQ_SUCC(ret)) {
    MQTTAllocator* allocator = container_allocator(children);
    children.clear();
    for (const auto& child_pair : *children_ptr) {
      auto insert_result =
          children.insert(std::make_pair(clone_to_tree_string(child_pair.first, allocator),
                                         child_pair.second));
      if (!insert_result.second) {
        insert_result.first->second = child_pair.second;
      }
    }
  }

  intrusive_ptr_release(node);
  return ret;
}

int TopicTreeNode::get_subscribers(SubscriberSet& subscribers) const
{
  IntermediateNode* node = get_intermediate_node();
  if (MQ_ISNULL(node)) {
    return MQ_ERR_INTERNAL;
  }

  const SubscriberSet* subscribers_ptr = nullptr;
  int ret = node->get_subscribers(subscribers_ptr);
  if (MQ_SUCC(ret)) {
    MQTTAllocator* allocator = container_allocator(subscribers);
    subscribers.clear();
    for (const auto& subscriber : *subscribers_ptr) {
      subscribers.insert(SubscriberInfo(subscriber.client_id, subscriber.qos, allocator));
    }
  }

  intrusive_ptr_release(node);
  return ret;
}

int TopicTreeNode::has_subscribers(bool& has_subs) const
{
  IntermediateNode* node = get_intermediate_node();
  if (MQ_ISNULL(node)) {
    return MQ_ERR_INTERNAL;
  }

  const SubscriberSet* subscribers_ptr = nullptr;
  int ret = node->get_subscribers(subscribers_ptr);
  if (MQ_SUCC(ret)) {
    has_subs = !subscribers_ptr->empty();
  }

  intrusive_ptr_release(node);
  return ret;
}

int TopicTreeNode::has_children(bool& has_childs) const
{
  IntermediateNode* node = get_intermediate_node();
  if (MQ_ISNULL(node)) {
    return MQ_ERR_INTERNAL;
  }

  const TopicTreeChildrenMap* children_ptr = nullptr;
  int ret = node->get_children(children_ptr);
  if (MQ_SUCC(ret)) {
    has_childs = !children_ptr->empty();
  }

  intrusive_ptr_release(node);
  return ret;
}

void intrusive_ptr_add_ref(TopicTreeNode* node)
{
  if (node) {
    node->ref_count_.fetch_add(1, std::memory_order_relaxed);
  }
}

void intrusive_ptr_release(TopicTreeNode* node)
{
  if (node && node->ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    destroy_topic_tree_node(node);
  }
}

ConcurrentTopicTree::ConcurrentTopicTree(MQTTAllocator* allocator)
    : allocator_(allocator),
      root_(),
      total_subscribers_(0),
      total_nodes_(0),
      single_level_wildcard_("+", MQTTStrAllocator(allocator)),
      multi_level_wildcard_("#", MQTTStrAllocator(allocator)),
      empty_path_(MQTTStrAllocator(allocator))
{
  if (MQ_ISNULL(allocator_)) {
    LOG_ERROR("Topic tree allocator is null");
    return;
  }

  int ret = create_initialized_node(allocator_, root_);
  if (MQ_SUCC(ret)) {
    total_nodes_.store(1, std::memory_order_relaxed);
  } else {
    LOG_ERROR("Failed to initialize root node: {}", ret);
  }
}

ConcurrentTopicTree::~ConcurrentTopicTree() {}

int ConcurrentTopicTree::split_topic_levels(const MQTTString& topic, TopicTreeLevelVector& levels) const
{
  levels.clear();
  if (topic.empty()) {
    return MQ_ERR_TOPIC_TREE_INVALID_TOPIC;
  }

  if (topic.size() > 65535) {
    return MQ_ERR_TOPIC_TREE_INVALID_TOPIC;
  }

  size_t estimated_levels = std::count(topic.begin(), topic.end(), '/') + 1;
  if (estimated_levels > 0 && estimated_levels <= 1000) {
    levels.reserve(estimated_levels);
  }

  auto start = topic.begin();
  for (auto it = topic.begin(); it != topic.end(); ++it) {
    if (*it == '/') {
      levels.emplace_back(start, it, MQTTStrAllocator(allocator_));
      start = it + 1;
    }
  }

  if (start != topic.end()) {
    levels.emplace_back(start, topic.end(), MQTTStrAllocator(allocator_));
  }

  return MQ_SUCCESS;
}

int ConcurrentTopicTree::get_or_create_node(const TopicTreeLevelVector& levels,
                                            bool create_if_not_exists, TopicTreeNodePtr& node)
{
  TopicTreeNodePtr current = root_;
  for (const MQTTString& level : levels) {
    TopicTreeNodePtr child;
    int ret = current->get_child(level, child);
    if (ret == MQ_ERR_NOT_FOUND_V2) {
      if (!create_if_not_exists) {
        return MQ_ERR_NOT_FOUND_V2;
      }

      std::lock_guard<std::mutex> lock(node_creation_mutex_);
      ret = current->get_child(level, child);
      if (MQ_SUCC(ret)) {
        current = child;
        continue;
      }

      ret = create_initialized_node(allocator_, child);
      if (MQ_FAIL(ret)) {
        return ret;
      }

      int retry_count = 0;
      const int max_retries = 50;
      while (retry_count < max_retries) {
        IntermediateNode* old_node = current->get_intermediate_node();
        if (MQ_ISNULL(old_node)) {
          return MQ_ERR_INTERNAL;
        }

        IntermediateNode* new_node = nullptr;
        ret = old_node->clone(new_node);
        if (MQ_FAIL(ret)) {
          intrusive_ptr_release(old_node);
          return ret;
        }

        ret = new_node->add_child(level, child);
        if (MQ_FAIL(ret)) {
          intrusive_ptr_release(old_node);
          destroy_intermediate_node(new_node);
          return ret;
        }

        if (current->compare_and_swap_intermediate_node(old_node, new_node)) {
          intrusive_ptr_release(old_node);
          total_nodes_.fetch_add(1, std::memory_order_relaxed);
          break;
        }

        intrusive_ptr_release(old_node);
        destroy_intermediate_node(new_node);
        ++retry_count;
        if (retry_count > 5) {
          std::this_thread::yield();
        }
      }

      if (retry_count >= max_retries) {
        return MQ_ERR_TOPIC_TREE_CONCURRENT;
      }
    } else if (MQ_FAIL(ret)) {
      return ret;
    }

    current = child;
  }

  node = current;
  return MQ_SUCCESS;
}

int ConcurrentTopicTree::add_subscriber_to_node(const TopicTreeNodePtr& node,
                                                const SubscriberInfo& subscriber)
{
  int retry_count = 0;
  const int max_retries = 100;

  while (retry_count < max_retries) {
    IntermediateNode* old_node = node->get_intermediate_node();
    if (MQ_ISNULL(old_node)) {
      return MQ_ERR_INTERNAL;
    }

    const SubscriberSet* existing_subscribers = nullptr;
    int ret = old_node->get_subscribers(existing_subscribers);
    if (MQ_FAIL(ret)) {
      intrusive_ptr_release(old_node);
      return ret;
    }

    bool already_exists =
        existing_subscribers && existing_subscribers->find(subscriber) != existing_subscribers->end();

    IntermediateNode* new_node = nullptr;
    ret = old_node->clone(new_node);
    if (MQ_FAIL(ret)) {
      intrusive_ptr_release(old_node);
      return ret;
    }

    ret = new_node->add_subscriber(subscriber);
    if (MQ_FAIL(ret)) {
      intrusive_ptr_release(old_node);
      destroy_intermediate_node(new_node);
      return ret;
    }

    if (node->compare_and_swap_intermediate_node(old_node, new_node)) {
      intrusive_ptr_release(old_node);
      if (!already_exists) {
        total_subscribers_.fetch_add(1, std::memory_order_relaxed);
      }
      return MQ_SUCCESS;
    }

    intrusive_ptr_release(old_node);
    destroy_intermediate_node(new_node);
    ++retry_count;
    if (retry_count > 10) {
      std::this_thread::yield();
    }
  }

  return MQ_ERR_TOPIC_TREE_CONCURRENT;
}

int ConcurrentTopicTree::remove_subscriber_from_node(const TopicTreeNodePtr& node,
                                                     const MQTTString& client_id)
{
  int retry_count = 0;
  const int max_retries = 100;

  while (retry_count < max_retries) {
    IntermediateNode* old_node = node->get_intermediate_node();
    if (MQ_ISNULL(old_node)) {
      return MQ_ERR_INTERNAL;
    }

    SubscriberInfo lookup(client_id, 0, allocator_);
    const SubscriberSet* existing_subscribers = nullptr;
    int ret = old_node->get_subscribers(existing_subscribers);
    if (MQ_FAIL(ret)) {
      intrusive_ptr_release(old_node);
      return ret;
    }

    bool exists =
        existing_subscribers && existing_subscribers->find(lookup) != existing_subscribers->end();
    if (!exists) {
      intrusive_ptr_release(old_node);
      return MQ_ERR_NOT_FOUND_V2;
    }

    IntermediateNode* new_node = nullptr;
    ret = old_node->clone(new_node);
    if (MQ_FAIL(ret)) {
      intrusive_ptr_release(old_node);
      return ret;
    }

    ret = new_node->remove_subscriber(client_id);
    if (MQ_FAIL(ret)) {
      intrusive_ptr_release(old_node);
      destroy_intermediate_node(new_node);
      return ret;
    }

    if (node->compare_and_swap_intermediate_node(old_node, new_node)) {
      intrusive_ptr_release(old_node);
      total_subscribers_.fetch_sub(1, std::memory_order_relaxed);
      return MQ_SUCCESS;
    }

    intrusive_ptr_release(old_node);
    destroy_intermediate_node(new_node);
    ++retry_count;
    if (retry_count > 10) {
      std::this_thread::yield();
    }
  }

  return MQ_ERR_TOPIC_TREE_CONCURRENT;
}

int ConcurrentTopicTree::subscribe(const MQTTString& topic_filter, const MQTTString& client_id,
                                   uint8_t qos)
{
  if (!is_initialized()) {
    return MQ_ERR_TOPIC_TREE;
  }

  if (client_id.empty()) {
    return MQ_ERR_PARAM_V2;
  }

  int ret = validate_topic_filter(topic_filter);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  TopicTreeLevelVector levels = make_level_vector(allocator_);
  ret = split_topic_levels(topic_filter, levels);
  if (MQ_FAIL(ret) || levels.empty()) {
    return MQ_ERR_PARAM_V2;
  }

  TopicTreeNodePtr target_node;
  ret = get_or_create_node(levels, true, target_node);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  SubscriberInfo subscriber(client_id, qos, allocator_);
  return add_subscriber_to_node(target_node, subscriber);
}

int ConcurrentTopicTree::unsubscribe(const MQTTString& topic_filter, const MQTTString& client_id)
{
  if (!is_initialized()) {
    return MQ_ERR_TOPIC_TREE;
  }

  if (topic_filter.empty() || client_id.empty()) {
    return MQ_ERR_PARAM_V2;
  }

  TopicTreeLevelVector levels = make_level_vector(allocator_);
  int ret = split_topic_levels(topic_filter, levels);
  if (MQ_FAIL(ret) || levels.empty()) {
    return MQ_ERR_PARAM_V2;
  }

  TopicTreeNodePtr target_node;
  ret = get_or_create_node(levels, false, target_node);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  return remove_subscriber_from_node(target_node, client_id);
}

int ConcurrentTopicTree::find_subscribers_recursive(const TopicTreeNodePtr& node,
                                                    const TopicTreeLevelVector& topic_levels,
                                                    size_t level_index,
                                                    TopicMatchResult& result) const
{
  if (!node) {
    return MQ_SUCCESS;
  }

  SubscriberSet subscribers = make_subscriber_set(allocator_);
  if (level_index >= topic_levels.size()) {
    int ret = node->get_subscribers(subscribers);
    if (MQ_SUCC(ret)) {
      MQTTAllocator* result_allocator = container_allocator(result.subscribers);
      for (const auto& subscriber : subscribers) {
        result.subscribers.emplace_back(subscriber.client_id, subscriber.qos, result_allocator);
        ++result.total_count;
      }
    }

    TopicTreeNodePtr wildcard_node;
    ret = node->get_child(multi_level_wildcard_, wildcard_node);
    if (ret == MQ_SUCCESS && wildcard_node) {
      SubscriberSet wildcard_subscribers = make_subscriber_set(allocator_);
      ret = wildcard_node->get_subscribers(wildcard_subscribers);
      if (MQ_SUCC(ret)) {
        MQTTAllocator* result_allocator = container_allocator(result.subscribers);
        for (const auto& subscriber : wildcard_subscribers) {
          result.subscribers.emplace_back(subscriber.client_id, subscriber.qos, result_allocator);
          ++result.total_count;
        }
      }
    }

    return MQ_SUCCESS;
  }

  int ret = MQ_SUCCESS;
  const MQTTString& current_level = topic_levels[level_index];

  TopicTreeNodePtr exact_child;
  ret = node->get_child(current_level, exact_child);
  if (ret == MQ_SUCCESS && exact_child) {
    ret = find_subscribers_recursive(exact_child, topic_levels, level_index + 1, result);
    if (MQ_FAIL(ret)) {
      return ret;
    }
  }

  TopicTreeNodePtr single_wildcard_child;
  ret = node->get_child(single_level_wildcard_, single_wildcard_child);
  if (ret == MQ_SUCCESS && single_wildcard_child) {
    ret = find_subscribers_recursive(single_wildcard_child, topic_levels, level_index + 1, result);
    if (MQ_FAIL(ret)) {
      return ret;
    }
  }

  TopicTreeNodePtr multi_wildcard_child;
  ret = node->get_child(multi_level_wildcard_, multi_wildcard_child);
  if (ret == MQ_SUCCESS && multi_wildcard_child) {
    SubscriberSet multi_wildcard_subscribers = make_subscriber_set(allocator_);
    ret = multi_wildcard_child->get_subscribers(multi_wildcard_subscribers);
    if (MQ_SUCC(ret)) {
      MQTTAllocator* result_allocator = container_allocator(result.subscribers);
      for (const auto& subscriber : multi_wildcard_subscribers) {
        result.subscribers.emplace_back(subscriber.client_id, subscriber.qos, result_allocator);
        ++result.total_count;
      }
    }
  }

  return MQ_SUCCESS;
}

int ConcurrentTopicTree::find_subscribers(const MQTTString& topic, TopicMatchResult& result) const
{
  if (!is_initialized()) {
    return MQ_ERR_TOPIC_TREE;
  }

  if (topic.empty()) {
    return MQ_ERR_PARAM_V2;
  }

  result.subscribers.clear();
  result.total_count = 0;

  TopicTreeLevelVector levels = make_level_vector(allocator_);
  int ret = split_topic_levels(topic, levels);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  ret = find_subscribers_recursive(root_, levels, 0, result);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  if (!result.subscribers.empty()) {
    std::sort(result.subscribers.begin(), result.subscribers.end(),
              [](const SubscriberInfo& a, const SubscriberInfo& b) {
                return from_mqtt_string(a.client_id) < from_mqtt_string(b.client_id);
              });
    result.subscribers.erase(std::unique(result.subscribers.begin(), result.subscribers.end()),
                             result.subscribers.end());
    result.total_count = result.subscribers.size();
  }

  return MQ_SUCCESS;
}

int ConcurrentTopicTree::unsubscribe_all(const MQTTString& client_id)
{
  if (client_id.empty()) {
    return 0;
  }

  TopicTreeLevelVector subscriptions = make_level_vector(allocator_);
  int ret = get_client_subscriptions(client_id, subscriptions);
  if (MQ_FAIL(ret)) {
    return 0;
  }

  int unsubscribed_count = 0;
  for (const auto& subscription : subscriptions) {
    if (unsubscribe(subscription, client_id) == MQ_SUCCESS) {
      ++unsubscribed_count;
    }
  }

  return unsubscribed_count;
}

int ConcurrentTopicTree::collect_client_subscriptions(const TopicTreeNodePtr& node,
                                                      const MQTTString& current_path,
                                                      const MQTTString& client_id,
                                                      TopicTreeLevelVector& result) const
{
  if (!node) {
    return MQ_SUCCESS;
  }

  SubscriberSet subscribers = make_subscriber_set(allocator_);
  int ret = node->get_subscribers(subscribers);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  SubscriberInfo target_info(client_id, 0, allocator_);
  if (subscribers.find(target_info) != subscribers.end()) {
    result.emplace_back(clone_to_tree_string(current_path, container_allocator(result)));
  }

  TopicTreeChildrenMap children = make_children_map(allocator_);
  ret = node->get_children(children);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  for (const auto& child_pair : children) {
    MQTTString child_path = join_topic_path(current_path, child_pair.first, allocator_);
    ret = collect_client_subscriptions(child_pair.second, child_path, client_id, result);
    if (MQ_FAIL(ret)) {
      return ret;
    }
  }

  return MQ_SUCCESS;
}

int ConcurrentTopicTree::get_client_subscriptions(const MQTTString& client_id,
                                                  TopicTreeLevelVector& subscriptions) const
{
  subscriptions.clear();
  if (!is_initialized()) {
    return MQ_ERR_TOPIC_TREE;
  }

  return collect_client_subscriptions(root_, empty_path_, client_id, subscriptions);
}

int ConcurrentTopicTree::get_total_subscribers(size_t& total_count) const
{
  total_count = total_subscribers_.load(std::memory_order_relaxed);
  return MQ_SUCCESS;
}

int ConcurrentTopicTree::get_total_nodes(size_t& total_count) const
{
  total_count = total_nodes_.load(std::memory_order_relaxed);
  return MQ_SUCCESS;
}

int ConcurrentTopicTree::cleanup_empty_nodes_recursive(const TopicTreeNodePtr& node,
                                                       const MQTTString& path, bool& should_delete)
{
  should_delete = false;
  if (!node) {
    should_delete = true;
    return MQ_SUCCESS;
  }

  TopicTreeChildrenMap children = make_children_map(allocator_);
  int ret = node->get_children(children);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  TopicTreeLevelVector children_to_remove = make_level_vector(allocator_);
  for (const auto& child_pair : children) {
    MQTTString child_path = join_topic_path(path, child_pair.first, allocator_);
    bool child_should_delete = false;
    ret = cleanup_empty_nodes_recursive(child_pair.second, child_path, child_should_delete);
    if (MQ_FAIL(ret)) {
      return ret;
    }
    if (child_should_delete) {
      children_to_remove.push_back(clone_to_tree_string(child_pair.first, allocator_));
    }
  }

  for (const auto& child_level : children_to_remove) {
    while (true) {
      IntermediateNode* old_node = node->get_intermediate_node();
      if (MQ_ISNULL(old_node)) {
        return MQ_ERR_INTERNAL;
      }

      IntermediateNode* new_node = nullptr;
      ret = old_node->clone(new_node);
      if (MQ_FAIL(ret)) {
        intrusive_ptr_release(old_node);
        return ret;
      }

      ret = new_node->remove_child(child_level);
      if (MQ_FAIL(ret)) {
        intrusive_ptr_release(old_node);
        destroy_intermediate_node(new_node);
        return ret;
      }

      if (node->compare_and_swap_intermediate_node(old_node, new_node)) {
        intrusive_ptr_release(old_node);
        total_nodes_.fetch_sub(1, std::memory_order_relaxed);
        break;
      }

      intrusive_ptr_release(old_node);
      destroy_intermediate_node(new_node);
    }
  }

  bool has_subs = false;
  bool has_childs = false;
  ret = node->has_subscribers(has_subs);
  if (MQ_FAIL(ret)) {
    return ret;
  }
  ret = node->has_children(has_childs);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  should_delete = !has_subs && !has_childs;
  return MQ_SUCCESS;
}

int ConcurrentTopicTree::cleanup_empty_nodes(size_t& cleaned_count)
{
  size_t original_count = total_nodes_.load(std::memory_order_relaxed);
  bool should_delete = false;
  int ret = cleanup_empty_nodes_recursive(root_, empty_path_, should_delete);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  size_t final_count = total_nodes_.load(std::memory_order_relaxed);
  cleaned_count = original_count - final_count;
  return MQ_SUCCESS;
}

int ConcurrentTopicTree::get_memory_usage(size_t& memory_usage) const
{
  memory_usage = allocator_ ? allocator_->get_memory_usage() : 0;
  return MQ_SUCCESS;
}

}  // namespace mqtt
