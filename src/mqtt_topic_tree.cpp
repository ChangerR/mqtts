#include "mqtt_topic_tree.h"
#include "logger.h"
#include <algorithm>
#include <cassert>
#include <sstream>

namespace mqtt {

// ============================================================================
// IntermediateNode 实现
// ============================================================================

IntermediateNode::IntermediateNode(MQTTAllocator* allocator) 
    : allocator_(allocator)
    , children_(TopicTreeAllocator<std::pair<const std::string, std::shared_ptr<TopicTreeNode>>>(allocator))
    , subscribers_(TopicTreeAllocator<SubscriberInfo>(allocator))
    , ref_count_(1) {
}

IntermediateNode::~IntermediateNode() {
}

IntermediateNode* IntermediateNode::clone() const {
    // 使用allocator分配新的IntermediateNode
    void* memory = allocator_->allocate(sizeof(IntermediateNode));
    if (!memory) {
        throw std::bad_alloc();
    }
    
    IntermediateNode* new_node = new(memory) IntermediateNode(allocator_);
    new_node->children_ = children_;
    new_node->subscribers_ = subscribers_;
    return new_node;
}

void IntermediateNode::add_child(const std::string& level, std::shared_ptr<TopicTreeNode> child) {
    children_[level] = child;
}

void IntermediateNode::remove_child(const std::string& level) {
    children_.erase(level);
}

void IntermediateNode::add_subscriber(const SubscriberInfo& subscriber) {
    // 如果已存在，更新QoS
    auto it = subscribers_.find(subscriber);
    if (it != subscribers_.end()) {
        subscribers_.erase(it);
    }
    subscribers_.insert(subscriber);
}

void IntermediateNode::remove_subscriber(const std::string& client_id) {
    SubscriberInfo temp_info;
    temp_info.client_id = to_mqtt_string(client_id, nullptr);
    subscribers_.erase(temp_info);
}

std::shared_ptr<TopicTreeNode> IntermediateNode::get_child(const std::string& level) const {
    auto it = children_.find(level);
    return (it != children_.end()) ? it->second : nullptr;
}

// 引用计数管理
void intrusive_ptr_add_ref(IntermediateNode* node) {
    if (node) {
        node->ref_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

void intrusive_ptr_release(IntermediateNode* node) {
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
    : allocator_(allocator) {
    // 使用allocator分配IntermediateNode
    void* memory = allocator_->allocate(sizeof(IntermediateNode));
    if (!memory) {
        throw std::bad_alloc();
    }
    intermediate_node_.store(new(memory) IntermediateNode(allocator_));
}

TopicTreeNode::~TopicTreeNode() {
    IntermediateNode* node = intermediate_node_.load();
    if (node) {
        intrusive_ptr_release(node);
    }
}

IntermediateNode* TopicTreeNode::get_intermediate_node() const {
    IntermediateNode* node = intermediate_node_.load(std::memory_order_acquire);
    intrusive_ptr_add_ref(node);
    return node;
}

bool TopicTreeNode::compare_and_swap_intermediate_node(IntermediateNode* expected, IntermediateNode* desired) {
    intrusive_ptr_add_ref(desired);
    bool success = intermediate_node_.compare_exchange_weak(expected, desired, std::memory_order_acq_rel);
    if (success) {
        intrusive_ptr_release(expected);
    } else {
        intrusive_ptr_release(desired);
    }
    return success;
}

std::shared_ptr<TopicTreeNode> TopicTreeNode::get_child(const std::string& level) const {
    IntermediateNode* node = get_intermediate_node();
    std::shared_ptr<TopicTreeNode> result = node->get_child(level);
    intrusive_ptr_release(node);
    return result;
}

TopicTreeMap<std::string, std::shared_ptr<TopicTreeNode>> TopicTreeNode::get_children() const {
    IntermediateNode* node = get_intermediate_node();
    TopicTreeMap<std::string, std::shared_ptr<TopicTreeNode>> result = node->get_children();
    intrusive_ptr_release(node);
    return result;
}

SubscriberSet TopicTreeNode::get_subscribers() const {
    IntermediateNode* node = get_intermediate_node();
    SubscriberSet result = node->get_subscribers();
    intrusive_ptr_release(node);
    return result;
}

bool TopicTreeNode::has_subscribers() const {
    IntermediateNode* node = get_intermediate_node();
    bool result = !node->get_subscribers().empty();
    intrusive_ptr_release(node);
    return result;
}

bool TopicTreeNode::has_children() const {
    IntermediateNode* node = get_intermediate_node();
    bool result = !node->get_children().empty();
    intrusive_ptr_release(node);
    return result;
}

// ============================================================================
// ConcurrentTopicTree 实现
// ============================================================================

ConcurrentTopicTree::ConcurrentTopicTree(MQTTAllocator* allocator) 
    : allocator_(allocator)
    , root_(std::make_shared<TopicTreeNode>(allocator))
    , total_subscribers_(0)
    , total_nodes_(1) {
}

ConcurrentTopicTree::~ConcurrentTopicTree() {
}

std::vector<std::string> ConcurrentTopicTree::split_topic_levels(const MQTTString& topic) const {
    std::vector<std::string> levels;
    std::string topic_str = from_mqtt_string(topic);
    
    if (topic_str.empty()) {
        return levels;
    }
    
    size_t start = 0;
    size_t pos = 0;
    
    while ((pos = topic_str.find('/', start)) != std::string::npos) {
        if (pos > start) {  // 跳过空级别
            levels.push_back(topic_str.substr(start, pos - start));
        }
        start = pos + 1;
    }
    
    if (start < topic_str.size()) {
        levels.push_back(topic_str.substr(start));
    }
    
    return levels;
}

std::shared_ptr<TopicTreeNode> ConcurrentTopicTree::get_or_create_node(
    const std::vector<std::string>& levels, 
    bool create_if_not_exists) {
    
    std::shared_ptr<TopicTreeNode> current = root_;
    
    for (const std::string& level : levels) {
        std::shared_ptr<TopicTreeNode> child = current->get_child(level);
        
        if (!child) {
            if (!create_if_not_exists) {
                return nullptr;
            }
            
            // 创建新子节点
            child = std::make_shared<TopicTreeNode>(allocator_);
            
            // 无锁添加子节点
            while (true) {
                IntermediateNode* old_node = current->get_intermediate_node();
                IntermediateNode* new_node = old_node->clone();
                new_node->add_child(level, child);
                
                if (current->compare_and_swap_intermediate_node(old_node, new_node)) {
                    intrusive_ptr_release(old_node);
                    total_nodes_.fetch_add(1, std::memory_order_relaxed);
                    break;
                } else {
                    intrusive_ptr_release(old_node);
                    // 正确释放使用allocator分配的IntermediateNode
                    MQTTAllocator* allocator = new_node->allocator_;
                    new_node->~IntermediateNode();
                    allocator->deallocate(new_node, sizeof(IntermediateNode));
                    // 重试：可能其他线程已经创建了节点
                    child = current->get_child(level);
                    if (child) {
                        break;
                    }
                }
            }
        }
        
        current = child;
    }
    
    return current;
}

bool ConcurrentTopicTree::add_subscriber_to_node(
    std::shared_ptr<TopicTreeNode> node, 
    const SubscriberInfo& subscriber) {
    
    while (true) {
        IntermediateNode* old_node = node->get_intermediate_node();
        
        // 检查是否已经存在
        const auto& existing_subscribers = old_node->get_subscribers();
        bool already_exists = existing_subscribers.find(subscriber) != existing_subscribers.end();
        
        IntermediateNode* new_node = old_node->clone();
        new_node->add_subscriber(subscriber);
        
        if (node->compare_and_swap_intermediate_node(old_node, new_node)) {
            intrusive_ptr_release(old_node);
            if (!already_exists) {
                total_subscribers_.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        } else {
            intrusive_ptr_release(old_node);
            // 正确释放使用allocator分配的IntermediateNode
            MQTTAllocator* allocator = new_node->allocator_;
            new_node->~IntermediateNode();
            allocator->deallocate(new_node, sizeof(IntermediateNode));
            // 重试
        }
    }
    
    return false;
}

bool ConcurrentTopicTree::remove_subscriber_from_node(
    std::shared_ptr<TopicTreeNode> node, 
    const std::string& client_id) {
    
    while (true) {
        IntermediateNode* old_node = node->get_intermediate_node();
        
        // 检查是否存在
        SubscriberInfo temp_info;
        temp_info.client_id = to_mqtt_string(client_id, nullptr);
        const auto& existing_subscribers = old_node->get_subscribers();
        bool exists = existing_subscribers.find(temp_info) != existing_subscribers.end();
        
        if (!exists) {
            intrusive_ptr_release(old_node);
            return false;
        }
        
        IntermediateNode* new_node = old_node->clone();
        new_node->remove_subscriber(client_id);
        
        if (node->compare_and_swap_intermediate_node(old_node, new_node)) {
            intrusive_ptr_release(old_node);
            total_subscribers_.fetch_sub(1, std::memory_order_relaxed);
            return true;
        } else {
            intrusive_ptr_release(old_node);
            // 正确释放使用allocator分配的IntermediateNode
            MQTTAllocator* allocator = new_node->allocator_;
            new_node->~IntermediateNode();
            allocator->deallocate(new_node, sizeof(IntermediateNode));
            // 重试
        }
    }
    
    return false;
}

int ConcurrentTopicTree::subscribe(const MQTTString& topic_filter, const MQTTString& client_id, uint8_t qos) {
    if (topic_filter.empty() || client_id.empty()) {
        LOG_ERROR("Invalid topic filter or client ID for subscription");
        return MQ_ERR_PARAM_V2;
    }
    
    std::vector<std::string> levels = split_topic_levels(topic_filter);
    if (levels.empty()) {
        LOG_ERROR("Invalid topic filter format: {}", from_mqtt_string(topic_filter));
        return MQ_ERR_PARAM_V2;
    }
    
    // MQTT规范：主题过滤器不能以 # 开头，除非是单独的 #
    std::string topic_str = from_mqtt_string(topic_filter);
    if (topic_str.find('#') != std::string::npos) {
        // # 只能出现在末尾，且前面必须是 / 或者是单独的 #
        size_t hash_pos = topic_str.find('#');
        if (hash_pos != topic_str.size() - 1 ||  // # 不在末尾
            (hash_pos > 0 && topic_str[hash_pos - 1] != '/')) {  // # 前面不是 /
            if (topic_str != "#") {  // 除非是单独的 #
                LOG_ERROR("Invalid topic filter format: {}", topic_str);
                return MQ_ERR_PARAM_V2;
            }
        }
    }
    
    std::shared_ptr<TopicTreeNode> target_node = get_or_create_node(levels, true);
    if (!target_node) {
        LOG_ERROR("Failed to create node for topic filter: {}", from_mqtt_string(topic_filter));
        return MQ_ERR_INTERNAL;
    }
    
    SubscriberInfo subscriber(client_id, qos);
    if (!add_subscriber_to_node(target_node, subscriber)) {
        LOG_ERROR("Failed to add subscriber to node");
        return MQ_ERR_INTERNAL;
    }
    
    LOG_DEBUG("Subscribed client {} to topic filter {}", from_mqtt_string(client_id), from_mqtt_string(topic_filter));
    return MQ_SUCCESS;
}

int ConcurrentTopicTree::unsubscribe(const MQTTString& topic_filter, const MQTTString& client_id) {
    if (topic_filter.empty() || client_id.empty()) {
        LOG_ERROR("Invalid topic filter or client ID for unsubscription");
        return MQ_ERR_PARAM_V2;
    }
    
    std::vector<std::string> levels = split_topic_levels(topic_filter);
    if (levels.empty()) {
        LOG_ERROR("Invalid topic filter format: {}", from_mqtt_string(topic_filter));
        return MQ_ERR_PARAM_V2;
    }
    
    std::shared_ptr<TopicTreeNode> target_node = get_or_create_node(levels, false);
    if (!target_node) {
        LOG_WARN("Topic filter not found for unsubscription: {}", from_mqtt_string(topic_filter));
        return MQ_ERR_NOT_FOUND_V2;
    }
    
    std::string client_id_str = from_mqtt_string(client_id);
    if (!remove_subscriber_from_node(target_node, client_id_str)) {
        LOG_WARN("Subscriber not found for unsubscription: {} from {}", client_id_str, from_mqtt_string(topic_filter));
        return MQ_ERR_NOT_FOUND_V2;
    }
    
    LOG_DEBUG("Unsubscribed client {} from topic filter {}", client_id_str, from_mqtt_string(topic_filter));
    return MQ_SUCCESS;
}

void ConcurrentTopicTree::find_subscribers_recursive(
    const std::shared_ptr<TopicTreeNode>& node,
    const std::vector<std::string>& topic_levels,
    size_t level_index,
    TopicMatchResult& result) const {
    
    if (!node) {
        return;
    }
    
    // 获取当前节点的订阅者
    auto subscribers = node->get_subscribers();
    for (const SubscriberInfo& subscriber : subscribers) {
        result.subscribers.push_back(subscriber);
        result.total_count++;
    }
    
    // 如果已经到达主题末尾，检查多级通配符
    if (level_index >= topic_levels.size()) {
        std::shared_ptr<TopicTreeNode> wildcard_node = node->get_child("#");
        if (wildcard_node) {
            auto wildcard_subscribers = wildcard_node->get_subscribers();
            for (const SubscriberInfo& subscriber : wildcard_subscribers) {
                result.subscribers.push_back(subscriber);
                result.total_count++;
            }
        }
        return;
    }
    
    const std::string& current_level = topic_levels[level_index];
    
    // 1. 精确匹配
    std::shared_ptr<TopicTreeNode> exact_child = node->get_child(current_level);
    if (exact_child) {
        find_subscribers_recursive(exact_child, topic_levels, level_index + 1, result);
    }
    
    // 2. 单级通配符 '+'
    std::shared_ptr<TopicTreeNode> single_wildcard_child = node->get_child("+");
    if (single_wildcard_child) {
        find_subscribers_recursive(single_wildcard_child, topic_levels, level_index + 1, result);
    }
    
    // 3. 多级通配符 '#' (匹配当前级别及之后的所有级别)
    std::shared_ptr<TopicTreeNode> multi_wildcard_child = node->get_child("#");
    if (multi_wildcard_child) {
        auto multi_wildcard_subscribers = multi_wildcard_child->get_subscribers();
        for (const SubscriberInfo& subscriber : multi_wildcard_subscribers) {
            result.subscribers.push_back(subscriber);
            result.total_count++;
        }
    }
}

TopicMatchResult ConcurrentTopicTree::find_subscribers(const MQTTString& topic) const {
    TopicMatchResult result(allocator_);
    
    if (topic.empty()) {
        LOG_WARN("Empty topic for subscriber search");
        return result;
    }
    
    std::vector<std::string> levels = split_topic_levels(topic);
    
    // 从根节点开始递归查找
    find_subscribers_recursive(root_, levels, 0, result);
    
    // 去重（可能有重复的订阅者）
    std::sort(result.subscribers.begin(), result.subscribers.end(), 
              [](const SubscriberInfo& a, const SubscriberInfo& b) {
                  return from_mqtt_string(a.client_id) < from_mqtt_string(b.client_id);
              });
    
    result.subscribers.erase(
        std::unique(result.subscribers.begin(), result.subscribers.end()),
        result.subscribers.end());
    
    result.total_count = result.subscribers.size();
    
    LOG_DEBUG("Found {} subscribers for topic: {}", result.total_count, from_mqtt_string(topic));
    return result;
}

int ConcurrentTopicTree::unsubscribe_all(const MQTTString& client_id) {
    if (client_id.empty()) {
        LOG_ERROR("Invalid client ID for unsubscribe_all");
        return 0;
    }
    
    // 获取客户端的所有订阅
    std::vector<MQTTString> subscriptions = get_client_subscriptions(client_id);
    
    int unsubscribed_count = 0;
    for (const MQTTString& subscription : subscriptions) {
        if (unsubscribe(subscription, client_id) == MQ_SUCCESS) {
            unsubscribed_count++;
        }
    }
    
    LOG_DEBUG("Unsubscribed client {} from {} topic filters", 
              from_mqtt_string(client_id), unsubscribed_count);
    return unsubscribed_count;
}

void ConcurrentTopicTree::collect_client_subscriptions(
    const std::shared_ptr<TopicTreeNode>& node,
    const std::string& current_path,
    const std::string& client_id,
    std::vector<MQTTString>& result) const {
    
    if (!node) {
        return;
    }
    
    // 检查当前节点是否有目标客户端的订阅
    auto subscribers = node->get_subscribers();
    SubscriberInfo target_info;
    target_info.client_id = to_mqtt_string(client_id, nullptr);
    
    if (subscribers.find(target_info) != subscribers.end()) {
        result.push_back(to_mqtt_string(current_path, nullptr));
    }
    
    // 递归检查子节点
    auto children = node->get_children();
    for (const auto& child_pair : children) {
        std::string child_path = current_path.empty() ? child_pair.first : (current_path + "/" + child_pair.first);
        collect_client_subscriptions(child_pair.second, child_path, client_id, result);
    }
}

std::vector<MQTTString> ConcurrentTopicTree::get_client_subscriptions(const MQTTString& client_id) const {
    std::vector<MQTTString> result;
    std::string client_id_str = from_mqtt_string(client_id);
    collect_client_subscriptions(root_, "", client_id_str, result);
    return result;
}

size_t ConcurrentTopicTree::get_total_subscribers() const {
    return total_subscribers_.load(std::memory_order_relaxed);
}

size_t ConcurrentTopicTree::get_total_nodes() const {
    return total_nodes_.load(std::memory_order_relaxed);
}

bool ConcurrentTopicTree::cleanup_empty_nodes_recursive(std::shared_ptr<TopicTreeNode> node, const std::string& path) {
    if (!node) {
        return true;
    }
    
    auto children = node->get_children();
    
    // 递归清理子节点
    std::vector<std::string> children_to_remove;
    for (const auto& child_pair : children) {
        std::string child_path = path.empty() ? child_pair.first : (path + "/" + child_pair.first);
        if (cleanup_empty_nodes_recursive(child_pair.second, child_path)) {
            children_to_remove.push_back(child_pair.first);
        }
    }
    
    // 移除空的子节点
    for (const std::string& child_level : children_to_remove) {
        while (true) {
            IntermediateNode* old_node = node->get_intermediate_node();
            IntermediateNode* new_node = old_node->clone();
            new_node->remove_child(child_level);
            
            if (node->compare_and_swap_intermediate_node(old_node, new_node)) {
                intrusive_ptr_release(old_node);
                total_nodes_.fetch_sub(1, std::memory_order_relaxed);
                break;
            } else {
                intrusive_ptr_release(old_node);
                // 正确释放使用allocator分配的IntermediateNode
                MQTTAllocator* allocator = new_node->allocator_;
                new_node->~IntermediateNode();
                allocator->deallocate(new_node, sizeof(IntermediateNode));
                // 重试
            }
        }
    }
    
    // 判断当前节点是否应该被删除（没有订阅者且没有子节点）
    return !node->has_subscribers() && !node->has_children();
}

size_t ConcurrentTopicTree::cleanup_empty_nodes() {
    size_t original_count = total_nodes_.load(std::memory_order_relaxed);
    cleanup_empty_nodes_recursive(root_, "");
    size_t final_count = total_nodes_.load(std::memory_order_relaxed);
    size_t cleaned_count = original_count - final_count;
    
    if (cleaned_count > 0) {
        LOG_DEBUG("Cleaned up {} empty nodes", cleaned_count);
    }
    
    return cleaned_count;
}

size_t ConcurrentTopicTree::get_memory_usage() const {
    if (allocator_) {
        return allocator_->get_memory_usage();
    }
    return 0;
}

}  // namespace mqtt