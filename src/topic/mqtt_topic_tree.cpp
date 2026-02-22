#include "mqtt_topic_tree.h"
#include <algorithm>
#include <cassert>
#include <mutex>
#include <sstream>
#include <thread>
#include "logger.h"

namespace mqtt {

// ============================================================================
// Helper functions for safe STL operations
// ============================================================================

// Helper functions removed - using direct implementation in methods

// ============================================================================
// IntermediateNode 实现
// ============================================================================

IntermediateNode::IntermediateNode(MQTTAllocator* allocator)
    : allocator_(allocator),
      children_(TopicTreeAllocator<std::pair<const MQTTString, std::shared_ptr<TopicTreeNode>>>(
          allocator)),
      subscribers_(TopicTreeAllocator<SubscriberInfo>(allocator)),
      ref_count_(1)
{
}

IntermediateNode::~IntermediateNode() {}

int IntermediateNode::safe_copy_containers(IntermediateNode* dest) const
{
  int ret = MQ_SUCCESS;

  // Copy children map
  if (!children_.empty()) {
    if (dest->children_.max_size() < children_.size()) {
      LOG_ERROR("Destination children container too small");
      return MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
    }

    for (const auto& child_pair : children_) {
      auto result = dest->children_.insert(child_pair);
      if (!result.second) {
        LOG_ERROR("Failed to copy child node");
        return MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
      }
    }
  }

  // Copy subscribers set
  if (!subscribers_.empty()) {
    if (dest->subscribers_.max_size() < subscribers_.size()) {
      LOG_ERROR("Destination subscribers container too small");
      return MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
    }

    for (const auto& subscriber : subscribers_) {
      auto result = dest->subscribers_.insert(subscriber);
      if (!result.second) {
        LOG_ERROR("Failed to copy subscriber");
        return MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
      }
    }
  }

  return ret;
}

int IntermediateNode::clone(IntermediateNode*& cloned_node) const
{
  int ret = MQ_SUCCESS;

  if (MQ_ISNULL(allocator_)) {
    LOG_ERROR("Allocator is null");
    ret = MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
  } else {
    void* memory = allocator_->allocate(sizeof(IntermediateNode));
    if (MQ_ISNULL(memory)) {
      LOG_ERROR("Failed to allocate memory for IntermediateNode");
      ret = MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
    } else {
      // Use placement new without try-catch
      cloned_node = new (memory) IntermediateNode(allocator_);
      if (MQ_ISNULL(cloned_node)) {
        LOG_ERROR("Failed to construct IntermediateNode");
        allocator_->deallocate(memory, sizeof(IntermediateNode));
        ret = MQ_ERR_TOPIC_TREE_NODE_CREATE;
      } else {
        // Safe copy operations with pre-checks
        ret = safe_copy_containers(cloned_node);
        if (MQ_FAIL(ret)) {
          cloned_node->~IntermediateNode();
          allocator_->deallocate(memory, sizeof(IntermediateNode));
          cloned_node = nullptr;
        }
      }
    }
  }

  return ret;
}

int IntermediateNode::add_child(const MQTTString& level, std::shared_ptr<TopicTreeNode> child)
{
  int ret = MQ_SUCCESS;

  if (MQ_ISNULL(child)) {
    LOG_ERROR("Child node cannot be null");
    ret = MQ_ERR_PARAM_V2;
  } else {
    // Pre-check container capacity
    if (children_.size() >= children_.max_size()) {
      LOG_ERROR("Children container has reached maximum capacity");
      ret = MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
    } else {
      // Safe insert operation
      children_[level] = child;
    }
  }

  return ret;
}

int IntermediateNode::remove_child(const MQTTString& level)
{
  int ret = MQ_SUCCESS;

  if (children_.empty()) {
    LOG_DEBUG("No children to remove");
    ret = MQ_ERR_NOT_FOUND_V2;
  } else {
    auto it = children_.find(level);
    if (it != children_.end()) {
      children_.erase(it);
    } else {
      LOG_DEBUG("Child level '{}' not found for removal", level);
      ret = MQ_ERR_NOT_FOUND_V2;
    }
  }

  return ret;
}

int IntermediateNode::add_subscriber(const SubscriberInfo& subscriber)
{
  int ret = MQ_SUCCESS;

  if (from_mqtt_string(subscriber.client_id).empty()) {
    LOG_ERROR("Client ID cannot be empty");
    ret = MQ_ERR_TOPIC_TREE_INVALID_CLIENT;
  } else if (subscribers_.size() >= subscribers_.max_size()) {
    LOG_ERROR("Subscribers container has reached maximum capacity");
    ret = MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
  } else {
    // Safe find and erase operation
    auto it = subscribers_.find(subscriber);
    if (it != subscribers_.end()) {
      subscribers_.erase(it);
    }

    // Safe insert operation
    auto result = subscribers_.insert(subscriber);
    if (!result.second && result.first == subscribers_.end()) {
      LOG_ERROR("Failed to add subscriber - insert failed");
      ret = MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
    }
  }

  return ret;
}

int IntermediateNode::remove_subscriber(const MQTTString& client_id)
{
  int ret = MQ_SUCCESS;

  if (client_id.empty()) {
    LOG_ERROR("Client ID cannot be empty");
    ret = MQ_ERR_TOPIC_TREE_INVALID_CLIENT;
  } else if (subscribers_.empty()) {
    LOG_DEBUG("No subscribers to remove");
    ret = MQ_ERR_TOPIC_TREE_SUBSCRIBER_NOT_FOUND;
  } else {
    SubscriberInfo temp_info;
    temp_info.client_id = client_id;
    auto it = subscribers_.find(temp_info);
    if (it != subscribers_.end()) {
      subscribers_.erase(it);
    } else {
      LOG_DEBUG("Subscriber '{}' not found for removal", client_id);
      ret = MQ_ERR_TOPIC_TREE_SUBSCRIBER_NOT_FOUND;
    }
  }

  return ret;
}

int IntermediateNode::get_child(const MQTTString& level,
                                std::shared_ptr<TopicTreeNode>& child) const
{
  int ret = MQ_SUCCESS;

  {
    auto it = children_.find(level);
    if (it != children_.end()) {
      child = it->second;
    } else {
      child = nullptr;
      ret = MQ_ERR_NOT_FOUND_V2;
    }
  }

  return ret;
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

// 引用计数管理
void intrusive_ptr_add_ref(IntermediateNode* node)
{
  if (node) {
    node->ref_count_.fetch_add(1, std::memory_order_relaxed);
  }
}

void intrusive_ptr_release(IntermediateNode* node)
{
  if (node && node->ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    MQTTAllocator* allocator = node->allocator_;
    node->~IntermediateNode();
    allocator->deallocate(node, sizeof(IntermediateNode));
  }
}

// ============================================================================
// TopicTreeNode 实现
// ============================================================================

TopicTreeNode::TopicTreeNode(MQTTAllocator* allocator)
    : allocator_(allocator), intermediate_node_(nullptr)
{
  // 构造函数不做内存分配，由init()函数完成
}

int TopicTreeNode::init()
{
  if (MQ_ISNULL(allocator_)) {
    LOG_ERROR("Allocator is null");
    return MQ_ERR_PARAM_V2;
  }

  if (intermediate_node_.load() != nullptr) {
    LOG_ERROR("Node already initialized");
    return MQ_ERR_INVALID_STATE;
  }

  // 安全分配IntermediateNode
  void* memory = allocator_->allocate(sizeof(IntermediateNode));
  if (MQ_ISNULL(memory)) {
    LOG_ERROR("Failed to allocate memory for IntermediateNode");
    return MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
  }

  // 使用placement new构造对象（在内存已分配的情况下，placement new不会失败）
  IntermediateNode* node = new (memory) IntermediateNode(allocator_);
  // 注意：placement new在有效内存上不会返回nullptr

  intermediate_node_.store(node);
  return MQ_SUCCESS;
}

TopicTreeNode::~TopicTreeNode()
{
  IntermediateNode* node = intermediate_node_.load();
  if (node) {
    intrusive_ptr_release(node);
  }
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
  intrusive_ptr_add_ref(desired);
  bool success =
      intermediate_node_.compare_exchange_weak(expected, desired, std::memory_order_acq_rel);
  if (success) {
    intrusive_ptr_release(expected);
  } else {
    intrusive_ptr_release(desired);
  }
  return success;
}

int TopicTreeNode::get_child(const MQTTString& level, std::shared_ptr<TopicTreeNode>& child) const
{
  int ret = MQ_SUCCESS;
  IntermediateNode* node = nullptr;

  {
    node = get_intermediate_node();
    if (MQ_ISNULL(node)) {
      LOG_ERROR("Failed to get intermediate node");
      ret = MQ_ERR_INTERNAL;
    } else {
      ret = node->get_child(level, child);
      // 错误已经在IntermediateNode::get_child中处理
    }
  }

  if (node) {
    intrusive_ptr_release(node);
  }

  return ret;
}

int TopicTreeNode::get_children(TopicTreeChildrenMap& children) const
{
  int ret = MQ_SUCCESS;
  IntermediateNode* node = nullptr;
  const TopicTreeChildrenMap* children_ptr = nullptr;

  node = get_intermediate_node();
  if (MQ_ISNULL(node)) {
    LOG_ERROR("Failed to get intermediate node");
    ret = MQ_ERR_INTERNAL;
  } else {
    ret = node->get_children(children_ptr);
    if (MQ_SUCC(ret)) {
      if (MQ_ISNULL(children_ptr)) {
        LOG_ERROR("Children pointer is null");
        ret = MQ_ERR_INTERNAL;
      } else {
        // Safe copy operation
        if (children_ptr->empty()) {
          children.clear();
        } else {
          // Pre-check capacity
          if (children.max_size() < children_ptr->size()) {
            LOG_ERROR("Children container capacity insufficient");
            ret = MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
          } else {
            // Copy elements safely
            children.clear();
            for (const auto& child_pair : *children_ptr) {
              auto result = children.insert(child_pair);
              if (!result.second) {
                LOG_ERROR("Failed to copy child node");
                ret = MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
                break;
              }
            }
          }
        }
      }
    }
  }

  if (node) {
    intrusive_ptr_release(node);
  }

  return ret;
}

int TopicTreeNode::get_subscribers(SubscriberSet& subscribers) const
{
  int ret = MQ_SUCCESS;
  IntermediateNode* node = nullptr;
  const SubscriberSet* subscribers_ptr = nullptr;

  node = get_intermediate_node();
  if (MQ_ISNULL(node)) {
    LOG_ERROR("Failed to get intermediate node");
    ret = MQ_ERR_INTERNAL;
  } else {
    ret = node->get_subscribers(subscribers_ptr);
    if (MQ_SUCC(ret)) {
      if (MQ_ISNULL(subscribers_ptr)) {
        LOG_ERROR("Subscribers pointer is null");
        ret = MQ_ERR_INTERNAL;
      } else {
        // Safe copy operation
        if (subscribers_ptr->empty()) {
          subscribers.clear();
        } else {
          // Pre-check capacity
          if (subscribers.max_size() < subscribers_ptr->size()) {
            LOG_ERROR("Subscribers container capacity insufficient");
            ret = MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
          } else {
            // Copy elements safely
            subscribers.clear();
            for (const auto& subscriber : *subscribers_ptr) {
              auto result = subscribers.insert(subscriber);
              if (!result.second) {
                LOG_ERROR("Failed to copy subscriber");
                ret = MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
                break;
              }
            }
          }
        }
      }
    }
  }

  if (node) {
    intrusive_ptr_release(node);
  }

  return ret;
}

int TopicTreeNode::has_subscribers(bool& has_subs) const
{
  int ret = MQ_SUCCESS;
  IntermediateNode* node = nullptr;
  const SubscriberSet* subscribers_ptr = nullptr;

  node = get_intermediate_node();
  if (MQ_ISNULL(node)) {
    LOG_ERROR("Failed to get intermediate node");
    ret = MQ_ERR_INTERNAL;
  } else {
    ret = node->get_subscribers(subscribers_ptr);
    if (MQ_SUCC(ret)) {
      if (MQ_ISNULL(subscribers_ptr)) {
        LOG_ERROR("Subscribers pointer is null");
        ret = MQ_ERR_INTERNAL;
      } else {
        has_subs = !subscribers_ptr->empty();
      }
    }
  }

  if (node) {
    intrusive_ptr_release(node);
  }

  return ret;
}

int TopicTreeNode::has_children(bool& has_childs) const
{
  int ret = MQ_SUCCESS;
  IntermediateNode* node = nullptr;
  const TopicTreeChildrenMap* children_ptr = nullptr;

  node = get_intermediate_node();
  if (MQ_ISNULL(node)) {
    LOG_ERROR("Failed to get intermediate node");
    ret = MQ_ERR_INTERNAL;
  } else {
    ret = node->get_children(children_ptr);
    if (MQ_SUCC(ret)) {
      if (MQ_ISNULL(children_ptr)) {
        LOG_ERROR("Children pointer is null");
        ret = MQ_ERR_INTERNAL;
      } else {
        has_childs = !children_ptr->empty();
      }
    }
  }

  if (node) {
    intrusive_ptr_release(node);
  }

  return ret;
}

// ============================================================================
// ConcurrentTopicTree 实现
// ============================================================================

ConcurrentTopicTree::ConcurrentTopicTree(MQTTAllocator* allocator)
    : allocator_(allocator), root_(nullptr), total_subscribers_(0), total_nodes_(0)
{
  // 创建并初始化根节点
  root_ = std::make_shared<TopicTreeNode>(allocator);
  if (root_) {
    int ret = root_->init();
    if (MQ_SUCC(ret)) {
      total_nodes_.store(1);
    } else {
      LOG_ERROR("Failed to initialize root node: {}", ret);
      root_.reset();
    }
  }
}

ConcurrentTopicTree::~ConcurrentTopicTree() {}

int ConcurrentTopicTree::split_topic_levels(const MQTTString& topic,
                                            TopicTreeLevelVector& levels) const
{
  int ret = MQ_SUCCESS;

  levels.clear();
  std::string topic_str = from_mqtt_string(topic);

  if (topic_str.empty()) {
    LOG_ERROR("Topic string cannot be empty");
    ret = MQ_ERR_TOPIC_TREE_INVALID_TOPIC;
  } else {
    // Pre-check string length for safety
    if (topic_str.length() > 65535) {  // MQTT max topic length
      LOG_ERROR("Topic string too long: {}", topic_str.length());
      ret = MQ_ERR_TOPIC_TREE_INVALID_TOPIC;
    } else {
      size_t start = 0;
      size_t pos = 0;

      // Reserve space for estimated levels
      size_t estimated_levels = std::count(topic_str.begin(), topic_str.end(), '/') + 1;
      if (estimated_levels > 0 && estimated_levels <= 1000) {  // Reasonable limit
        levels.reserve(estimated_levels);
      }

      while ((pos = topic_str.find('/', start)) != std::string::npos) {
        if (pos < topic_str.length()) {  // Only check bounds, don't skip empty levels
          // Safe substring operation
          if (start <= topic_str.length() && pos <= topic_str.length()) {
            levels.emplace_back(to_mqtt_string(topic_str.substr(start, pos - start), allocator_));
          }
        }
        start = pos + 1;
      }

      if (start < topic_str.size()) {
        levels.emplace_back(to_mqtt_string(topic_str.substr(start), allocator_));
      }
    }
  }

  return ret;
}

int ConcurrentTopicTree::get_or_create_node(const TopicTreeLevelVector& levels,
                                            bool create_if_not_exists,
                                            std::shared_ptr<TopicTreeNode>& node)
{
  int ret = MQ_SUCCESS;
  std::shared_ptr<TopicTreeNode> current = root_;

  for (const MQTTString& level : levels) {
    std::shared_ptr<TopicTreeNode> child;
    ret = current->get_child(level, child);

    if (ret == MQ_ERR_NOT_FOUND_V2) {
      if (!create_if_not_exists) {
        return MQ_ERR_NOT_FOUND_V2;
      }

      // Use mutex to serialize node creation to avoid race conditions
      std::lock_guard<std::mutex> lock(node_creation_mutex_);
      
      // Double-check if another thread created the node while we were waiting
      ret = current->get_child(level, child);
      if (MQ_SUCC(ret)) {
        // Node was created by another thread, use it
        continue;
      }

      // Create new child node
      child = std::make_shared<TopicTreeNode>(allocator_);
      if (!child) {
        LOG_ERROR("Failed to create new TopicTreeNode");
        return MQ_ERR_TOPIC_TREE_MEMORY_ALLOC;
      }

      // Initialize the new node
      ret = child->init();
      if (MQ_FAIL(ret)) {
        LOG_ERROR("Failed to initialize new TopicTreeNode: {}", ret);
        return ret;
      }

      // Lock-free add child node with retry limit (still needed for subscriber updates)
      int retry_count = 0;
      const int max_retries = 50; // Reduced retries since we have mutex for node creation
      while (retry_count < max_retries) {
        IntermediateNode* old_node = current->get_intermediate_node();
        IntermediateNode* new_node = nullptr;
        ret = old_node->clone(new_node);
        if (MQ_FAIL(ret)) {
          intrusive_ptr_release(old_node);
          return ret;
        }

        ret = new_node->add_child(level, child);
        if (MQ_FAIL(ret)) {
          intrusive_ptr_release(old_node);
          MQTTAllocator* allocator = new_node->get_allocator();
          new_node->~IntermediateNode();
          allocator->deallocate(new_node, sizeof(IntermediateNode));
          return ret;
        }

        if (current->compare_and_swap_intermediate_node(old_node, new_node)) {
          intrusive_ptr_release(old_node);
          total_nodes_.fetch_add(1, std::memory_order_relaxed);
          break;
        } else {
          intrusive_ptr_release(old_node);
          MQTTAllocator* allocator = new_node->get_allocator();
          new_node->~IntermediateNode();
          allocator->deallocate(new_node, sizeof(IntermediateNode));
          
          retry_count++;
          // Small delay to reduce contention
          if (retry_count > 5) {
            std::this_thread::yield();
          }
        }
      }
      
      // Final check if we've exceeded retries
      if (retry_count >= max_retries) {
        LOG_ERROR("Failed to add child node after {} retries", max_retries);
        return MQ_ERR_TOPIC_TREE_CONCURRENT;
      }
    } else if (MQ_FAIL(ret)) {
      return ret;
    }

    current = child;
  }

  node = current;
  return ret;
}

int ConcurrentTopicTree::add_subscriber_to_node(std::shared_ptr<TopicTreeNode> node,
                                                const SubscriberInfo& subscriber)
{
  int ret = MQ_SUCCESS;
  int retry_count = 0;
  const int max_retries = 100; // Prevent infinite loops

  while (retry_count < max_retries) {
    IntermediateNode* old_node = node->get_intermediate_node();
    if (MQ_ISNULL(old_node)) {
      LOG_ERROR("Failed to get intermediate node");
      return MQ_ERR_INTERNAL;
    }

    // Check if already exists
    const SubscriberSet* existing_subscribers = nullptr;
    ret = old_node->get_subscribers(existing_subscribers);
    if (MQ_FAIL(ret)) {
      intrusive_ptr_release(old_node);
      return ret;
    }

    bool already_exists = (existing_subscribers &&
                           existing_subscribers->find(subscriber) != existing_subscribers->end());

    IntermediateNode* new_node = nullptr;
    ret = old_node->clone(new_node);
    if (MQ_FAIL(ret)) {
      intrusive_ptr_release(old_node);
      return ret;
    }

    ret = new_node->add_subscriber(subscriber);
    if (MQ_FAIL(ret)) {
      intrusive_ptr_release(old_node);
      MQTTAllocator* allocator = new_node->get_allocator();
      new_node->~IntermediateNode();
      allocator->deallocate(new_node, sizeof(IntermediateNode));
      return ret;
    }

    if (node->compare_and_swap_intermediate_node(old_node, new_node)) {
      intrusive_ptr_release(old_node);
      if (!already_exists) {
        total_subscribers_.fetch_add(1, std::memory_order_relaxed);
      }
      return MQ_SUCCESS;
    } else {
      intrusive_ptr_release(old_node);
      MQTTAllocator* allocator = new_node->get_allocator();
      new_node->~IntermediateNode();
      allocator->deallocate(new_node, sizeof(IntermediateNode));
      
      retry_count++;
      // Small delay to reduce contention
      if (retry_count > 10) {
        std::this_thread::yield();
      }
    }
  }

  LOG_ERROR("Failed to add subscriber after {} retries", max_retries);
  return MQ_ERR_TOPIC_TREE_CONCURRENT;
}

int ConcurrentTopicTree::remove_subscriber_from_node(std::shared_ptr<TopicTreeNode> node,
                                                     const MQTTString& client_id)
{
  int ret = MQ_SUCCESS;
  int retry_count = 0;
  const int max_retries = 100; // Prevent infinite loops

  while (retry_count < max_retries) {
    IntermediateNode* old_node = node->get_intermediate_node();
    if (MQ_ISNULL(old_node)) {
      LOG_ERROR("Failed to get intermediate node");
      return MQ_ERR_INTERNAL;
    }

    // Check if exists
    SubscriberInfo temp_info;
    temp_info.client_id = client_id;
    const SubscriberSet* existing_subscribers = nullptr;
    ret = old_node->get_subscribers(existing_subscribers);
    if (MQ_FAIL(ret)) {
      intrusive_ptr_release(old_node);
      return ret;
    }

    bool exists = (existing_subscribers &&
                   existing_subscribers->find(temp_info) != existing_subscribers->end());

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
      MQTTAllocator* allocator = new_node->get_allocator();
      new_node->~IntermediateNode();
      allocator->deallocate(new_node, sizeof(IntermediateNode));
      return ret;
    }

    if (node->compare_and_swap_intermediate_node(old_node, new_node)) {
      intrusive_ptr_release(old_node);
      total_subscribers_.fetch_sub(1, std::memory_order_relaxed);
      return MQ_SUCCESS;
    } else {
      intrusive_ptr_release(old_node);
      MQTTAllocator* allocator = new_node->get_allocator();
      new_node->~IntermediateNode();
      allocator->deallocate(new_node, sizeof(IntermediateNode));
      
      retry_count++;
      // Small delay to reduce contention
      if (retry_count > 10) {
        std::this_thread::yield();
      }
    }
  }

  LOG_ERROR("Failed to remove subscriber after {} retries", max_retries);
  return MQ_ERR_TOPIC_TREE_CONCURRENT;
}

int ConcurrentTopicTree::subscribe(const MQTTString& topic_filter, const MQTTString& client_id,
                                   uint8_t qos)
{
  if (!is_initialized()) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_TOPIC_TREE;
  }

  if (topic_filter.empty() || client_id.empty()) {
    LOG_ERROR("Invalid topic filter or client ID for subscription");
    return MQ_ERR_PARAM_V2;
  }

  TopicTreeLevelVector levels;
  int ret = split_topic_levels(topic_filter, levels);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  if (levels.empty()) {
    LOG_ERROR("Invalid topic filter format: {}", from_mqtt_string(topic_filter));
    return MQ_ERR_PARAM_V2;
  }

  // MQTT规范：主题过滤器不能以 # 开头，除非是单独的 #
  std::string topic_str = from_mqtt_string(topic_filter);
  if (topic_str.find('#') != std::string::npos) {
    // # 只能出现在末尾，且前面必须是 / 或者是单独的 #
    size_t hash_pos = topic_str.find('#');
    if (hash_pos != topic_str.size() - 1 ||                  // # 不在末尾
        (hash_pos > 0 && topic_str[hash_pos - 1] != '/')) {  // # 前面不是 /
      if (topic_str != "#") {                                // 除非是单独的 #
        LOG_ERROR("Invalid topic filter format: {}", topic_str);
        return MQ_ERR_PARAM_V2;
      }
    }
  }

  std::shared_ptr<TopicTreeNode> target_node;
  ret = get_or_create_node(levels, true, target_node);
  if (MQ_FAIL(ret)) {
    LOG_ERROR("Failed to create node for topic filter: {}", from_mqtt_string(topic_filter));
    return ret;
  }

  SubscriberInfo subscriber(client_id, qos);
  ret = add_subscriber_to_node(target_node, subscriber);
  if (MQ_FAIL(ret)) {
    LOG_ERROR("Failed to add subscriber to node");
    return ret;
  }

  LOG_DEBUG("Subscribed client {} to topic filter {}", from_mqtt_string(client_id),
            from_mqtt_string(topic_filter));
  return MQ_SUCCESS;
}

int ConcurrentTopicTree::unsubscribe(const MQTTString& topic_filter, const MQTTString& client_id)
{
  if (!is_initialized()) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_TOPIC_TREE;
  }

  if (topic_filter.empty() || client_id.empty()) {
    LOG_ERROR("Invalid topic filter or client ID for unsubscription");
    return MQ_ERR_PARAM_V2;
  }

  TopicTreeLevelVector levels;
  int ret = split_topic_levels(topic_filter, levels);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  if (levels.empty()) {
    LOG_ERROR("Invalid topic filter format: {}", from_mqtt_string(topic_filter));
    return MQ_ERR_PARAM_V2;
  }

  std::shared_ptr<TopicTreeNode> target_node;
  ret = get_or_create_node(levels, false, target_node);
  if (MQ_FAIL(ret)) {
    LOG_WARN("Topic filter not found for unsubscription: {}", from_mqtt_string(topic_filter));
    return ret;
  }

  ret = remove_subscriber_from_node(target_node, client_id);
  if (MQ_FAIL(ret)) {
    LOG_WARN("Subscriber not found for unsubscription: {} from {}", from_mqtt_string(client_id),
             from_mqtt_string(topic_filter));
    return ret;
  }

  LOG_DEBUG("Unsubscribed client {} from topic filter {}", from_mqtt_string(client_id),
            from_mqtt_string(topic_filter));
  return MQ_SUCCESS;
}

int ConcurrentTopicTree::find_subscribers_recursive(const std::shared_ptr<TopicTreeNode>& node,
                                                    const TopicTreeLevelVector& topic_levels,
                                                    size_t level_index,
                                                    TopicMatchResult& result) const
{
  if (!node) {
    return MQ_SUCCESS;
  }

  int ret = MQ_SUCCESS;

  // 如果已经到达主题末尾，收集当前节点的订阅者和多级通配符
  if (level_index >= topic_levels.size()) {
    // 收集当前节点的订阅者（完全匹配的订阅）
    SubscriberSet subscribers;
    ret = node->get_subscribers(subscribers);
    if (MQ_SUCC(ret)) {
      for (const SubscriberInfo& subscriber : subscribers) {
        result.subscribers.push_back(subscriber);
        result.total_count++;
      }
    }

    // 检查多级通配符
    std::shared_ptr<TopicTreeNode> wildcard_node;
    ret = node->get_child(to_mqtt_string("#", allocator_), wildcard_node);
    if (ret == MQ_SUCCESS && wildcard_node) {
      SubscriberSet wildcard_subscribers;
      ret = wildcard_node->get_subscribers(wildcard_subscribers);
      if (MQ_SUCC(ret)) {
        for (const SubscriberInfo& subscriber : wildcard_subscribers) {
          result.subscribers.push_back(subscriber);
          result.total_count++;
        }
      }
    }
    return MQ_SUCCESS;
  }

  const MQTTString& current_level = topic_levels[level_index];

  // 1. 精确匹配
  std::shared_ptr<TopicTreeNode> exact_child;
  ret = node->get_child(current_level, exact_child);
  if (ret == MQ_SUCCESS && exact_child) {
    ret = find_subscribers_recursive(exact_child, topic_levels, level_index + 1, result);
    if (MQ_FAIL(ret)) {
      return ret;
    }
  }

  // 2. 单级通配符 '+'
  std::shared_ptr<TopicTreeNode> single_wildcard_child;
  ret = node->get_child(to_mqtt_string("+", allocator_), single_wildcard_child);
  if (ret == MQ_SUCCESS && single_wildcard_child) {
    ret = find_subscribers_recursive(single_wildcard_child, topic_levels, level_index + 1, result);
    if (MQ_FAIL(ret)) {
      return ret;
    }
  }

  // 3. 多级通配符 '#' (匹配当前级别及之后的所有级别)
  std::shared_ptr<TopicTreeNode> multi_wildcard_child;
  ret = node->get_child(to_mqtt_string("#", allocator_), multi_wildcard_child);
  if (ret == MQ_SUCCESS && multi_wildcard_child) {
    SubscriberSet multi_wildcard_subscribers;
    ret = multi_wildcard_child->get_subscribers(multi_wildcard_subscribers);
    if (MQ_SUCC(ret)) {
      for (const SubscriberInfo& subscriber : multi_wildcard_subscribers) {
        result.subscribers.push_back(subscriber);
        result.total_count++;
      }
    }
  }

  return MQ_SUCCESS;
}

int ConcurrentTopicTree::find_subscribers(const MQTTString& topic, TopicMatchResult& result) const
{
  if (!is_initialized()) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_TOPIC_TREE;
  }

  if (topic.empty()) {
    LOG_WARN("Empty topic for subscriber search");
    return MQ_ERR_PARAM_V2;
  }

  TopicTreeLevelVector levels;
  int ret = split_topic_levels(topic, levels);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  // 从根节点开始递归查找
  ret = find_subscribers_recursive(root_, levels, 0, result);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  // 去重（可能有重复的订阅者）
  if (!result.subscribers.empty()) {
    std::sort(result.subscribers.begin(), result.subscribers.end(),
              [](const SubscriberInfo& a, const SubscriberInfo& b) {
                return from_mqtt_string(a.client_id) < from_mqtt_string(b.client_id);
              });

    result.subscribers.erase(std::unique(result.subscribers.begin(), result.subscribers.end()),
                             result.subscribers.end());

    result.total_count = result.subscribers.size();
  }

  LOG_DEBUG("Found {} subscribers for topic: {}", result.total_count, from_mqtt_string(topic));
  return MQ_SUCCESS;
}

int ConcurrentTopicTree::unsubscribe_all(const MQTTString& client_id)
{
  if (client_id.empty()) {
    LOG_ERROR("Invalid client ID for unsubscribe_all");
    return 0;
  }

  // 获取客户端的所有订阅
  std::vector<MQTTString> subscriptions;
  int ret = get_client_subscriptions(client_id, subscriptions);
  if (MQ_FAIL(ret)) {
    LOG_ERROR("Failed to get client subscriptions");
    return 0;
  }

  int unsubscribed_count = 0;
  for (const MQTTString& subscription : subscriptions) {
    if (unsubscribe(subscription, client_id) == MQ_SUCCESS) {
      unsubscribed_count++;
    }
  }

  LOG_DEBUG("Unsubscribed client {} from {} topic filters", from_mqtt_string(client_id),
            unsubscribed_count);
  return unsubscribed_count;
}

int ConcurrentTopicTree::collect_client_subscriptions(const std::shared_ptr<TopicTreeNode>& node,
                                                      const MQTTString& current_path,
                                                      const MQTTString& client_id,
                                                      std::vector<MQTTString>& result) const
{
  if (!node) {
    return MQ_SUCCESS;
  }

  int ret = MQ_SUCCESS;

  // 检查当前节点是否有目标客户端的订阅
  SubscriberSet subscribers;
  ret = node->get_subscribers(subscribers);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  SubscriberInfo target_info;
  target_info.client_id = client_id;

  if (subscribers.find(target_info) != subscribers.end()) {
    result.push_back(current_path);
  }

  // 递归检查子节点
  TopicTreeChildrenMap children;
  ret = node->get_children(children);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  for (const auto& child_pair : children) {
    MQTTString child_path = current_path.empty() ? child_pair.first : 
        to_mqtt_string(from_mqtt_string(current_path) + "/" + from_mqtt_string(child_pair.first), allocator_);
    ret = collect_client_subscriptions(child_pair.second, child_path, client_id, result);
    if (MQ_FAIL(ret)) {
      return ret;
    }
  }

  return MQ_SUCCESS;
}

int ConcurrentTopicTree::get_client_subscriptions(const MQTTString& client_id,
                                                  std::vector<MQTTString>& subscriptions) const
{
  subscriptions.clear();

  if (!is_initialized()) {
    LOG_ERROR("Topic tree not initialized");
    return MQ_ERR_TOPIC_TREE;
  }

  MQTTString empty_path = to_mqtt_string("", allocator_);
  return collect_client_subscriptions(root_, empty_path, client_id, subscriptions);
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

int ConcurrentTopicTree::cleanup_empty_nodes_recursive(std::shared_ptr<TopicTreeNode> node,
                                                       const MQTTString& path, bool& should_delete)
{
  should_delete = false;

  if (!node) {
    should_delete = true;
    return MQ_SUCCESS;
  }

  int ret = MQ_SUCCESS;
  TopicTreeChildrenMap children;
  ret = node->get_children(children);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  // 递归清理子节点
  TopicTreeLevelVector children_to_remove;
  for (const auto& child_pair : children) {
    MQTTString child_path = path.empty() ? child_pair.first : 
        to_mqtt_string(from_mqtt_string(path) + "/" + from_mqtt_string(child_pair.first), allocator_);
    bool child_should_delete = false;
    ret = cleanup_empty_nodes_recursive(child_pair.second, child_path, child_should_delete);
    if (MQ_FAIL(ret)) {
      return ret;
    }
    if (child_should_delete) {
      children_to_remove.push_back(child_pair.first);
    }
  }

  // 移除空的子节点
  for (const MQTTString& child_level : children_to_remove) {
    while (true) {
      IntermediateNode* old_node = node->get_intermediate_node();
      if (MQ_ISNULL(old_node)) {
        LOG_ERROR("Failed to get intermediate node");
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
        MQTTAllocator* allocator = new_node->get_allocator();
        new_node->~IntermediateNode();
        allocator->deallocate(new_node, sizeof(IntermediateNode));
        return ret;
      }

      if (node->compare_and_swap_intermediate_node(old_node, new_node)) {
        intrusive_ptr_release(old_node);
        total_nodes_.fetch_sub(1, std::memory_order_relaxed);
        break;
      } else {
        intrusive_ptr_release(old_node);
        MQTTAllocator* allocator = new_node->get_allocator();
        new_node->~IntermediateNode();
        allocator->deallocate(new_node, sizeof(IntermediateNode));
        // 重试
      }
    }
  }

  // 判断当前节点是否应该被删除（没有订阅者且没有子节点）
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
  MQTTString empty_path = to_mqtt_string("", allocator_);
  int ret = cleanup_empty_nodes_recursive(root_, empty_path, should_delete);
  if (MQ_FAIL(ret)) {
    return ret;
  }

  size_t final_count = total_nodes_.load(std::memory_order_relaxed);
  cleaned_count = original_count - final_count;

  if (cleaned_count > 0) {
    LOG_DEBUG("Cleaned up {} empty nodes", cleaned_count);
  }

  return MQ_SUCCESS;
}

int ConcurrentTopicTree::get_memory_usage(size_t& memory_usage) const
{
  if (allocator_) {
    memory_usage = allocator_->get_memory_usage();
  } else {
    memory_usage = 0;
  }
  return MQ_SUCCESS;
}

}  // namespace mqtt