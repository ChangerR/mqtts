#ifndef MQTT_TOPIC_TREE_H
#define MQTT_TOPIC_TREE_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "mqtt_allocator.h"
#include "mqtt_define.h"
#include "mqtt_parser.h"
#include "mqtt_stl_allocator.h"

namespace mqtt {

/**
 * @brief 主题树节点的前向声明
 */
class TopicTreeNode;
class IntermediateNode;

// 使用统一的MQTT STL分配器，避免重复代码
template <typename T>
using TopicTreeAllocator = MQTTSTLAllocator<T>;

/**
 * @brief 主题树专用的自定义分配器（已废弃，使用MQTTSTLAllocator）
 */
/*template <typename T>
class TopicTreeAllocator
{
 public:
  typedef T value_type;
  typedef T* pointer;
  typedef const T* const_pointer;
  typedef T& reference;
  typedef const T& const_reference;
  typedef size_t size_type;
  typedef ptrdiff_t difference_type;

  template <typename U>
  struct rebind
  {
    typedef TopicTreeAllocator<U> other;
  };

  TopicTreeAllocator() : mqtt_allocator_(nullptr) {}

  explicit TopicTreeAllocator(MQTTAllocator* mqtt_allocator) : mqtt_allocator_(mqtt_allocator) {}

  template <typename U>
  TopicTreeAllocator(const TopicTreeAllocator<U>& other)
      : mqtt_allocator_(other.get_mqtt_allocator())
  {
  }

  TopicTreeAllocator(const TopicTreeAllocator& other) : mqtt_allocator_(other.mqtt_allocator_) {}

  TopicTreeAllocator& operator=(const TopicTreeAllocator& other)
  {
    if (this != &other) {
      mqtt_allocator_ = other.mqtt_allocator_;
    }
    return *this;
  }

  pointer allocate(size_type n, const void* = 0)
  {
    if (mqtt_allocator_) {
      pointer result = static_cast<pointer>(mqtt_allocator_->allocate(n * sizeof(T)));
      if (!result) {
        throw std::bad_alloc();
      }
      return result;
    } else {
      // Fallback to standard new if no MQTT allocator
      return static_cast<pointer>(::operator new(n * sizeof(T)));
    }
  }

  void deallocate(pointer p, size_type n)
  {
    if (mqtt_allocator_ && p) {
      mqtt_allocator_->deallocate(p, n * sizeof(T));
    } else if (p) {
      // Fallback to standard delete
      ::operator delete(p);
    }
  }

  template <typename U, typename... Args>
  void construct(U* p, Args&&... args)
  {
    new (p) U(std::forward<Args>(args)...);
  }

  template <typename U>
  void destroy(U* p)
  {
    p->~U();
  }

  size_type max_size() const { return size_t(-1) / sizeof(T); }

  MQTTAllocator* get_mqtt_allocator() const { return mqtt_allocator_; }

  bool operator==(const TopicTreeAllocator& other) const
  {
    return mqtt_allocator_ == other.mqtt_allocator_;
  }

  bool operator!=(const TopicTreeAllocator& other) const { return !(*this == other); }

 private:
  MQTTAllocator* mqtt_allocator_;
};*/

/**
 * @brief 使用自定义分配器的容器类型定义
 */
template <typename Key, typename Value>
using TopicTreeMap = std::unordered_map<Key, Value, std::hash<Key>, std::equal_to<Key>,
                                        TopicTreeAllocator<std::pair<const Key, Value>>>;

template <typename Value>
using TopicTreeSet =
    std::unordered_set<Value, std::hash<Value>, std::equal_to<Value>, TopicTreeAllocator<Value>>;

template <typename Value>
using TopicTreeVector = std::vector<Value, TopicTreeAllocator<Value>>;

// 专门为主题树定义的类型，统一使用 MQTTString
using TopicTreeChildrenMap = TopicTreeMap<MQTTString, std::shared_ptr<TopicTreeNode>>;
using TopicTreeLevelVector = TopicTreeVector<MQTTString>;

/**
 * @brief 订阅者信息
 */
struct SubscriberInfo
{
  MQTTString client_id;
  uint8_t qos;

  SubscriberInfo() : qos(0) {}
  SubscriberInfo(const MQTTString& id, uint8_t q) : client_id(id), qos(q) {}

  bool operator==(const SubscriberInfo& other) const
  {
    return from_mqtt_string(client_id) == from_mqtt_string(other.client_id);
  }
};

/**
 * @brief 订阅者哈希函数
 */
struct SubscriberInfoHash
{
  size_t operator()(const SubscriberInfo& info) const
  {
    return std::hash<std::string>{}(from_mqtt_string(info.client_id));
  }
};

// 专门用于SubscriberInfo的集合类型
using SubscriberSet =
    std::unordered_set<SubscriberInfo, SubscriberInfoHash, std::equal_to<SubscriberInfo>,
                       TopicTreeAllocator<SubscriberInfo>>;

/**
 * @brief 主题树节点
 * 使用无锁设计，通过中间节点（I-nodes）实现并发安全
 */
class TopicTreeNode
{
 public:
  explicit TopicTreeNode(MQTTAllocator* allocator);
  ~TopicTreeNode();

  /**
   * @brief 初始化节点（必须在构造后立即调用）
   * @return MQ_SUCCESS成功，其他值失败
   */
  int init();

  // 禁止拷贝和赋值
  TopicTreeNode(const TopicTreeNode&) = delete;
  TopicTreeNode& operator=(const TopicTreeNode&) = delete;

  /**
   * @brief 获取子节点
   * @param level 主题级别
   * @param child 输出参数：子节点指针
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_child(const MQTTString& level, std::shared_ptr<TopicTreeNode>& child) const;

  /**
   * @brief 获取所有子节点
   * @param children 输出参数：子节点映射的副本
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_children(TopicTreeChildrenMap& children) const;

  /**
   * @brief 获取当前节点的订阅者
   * @param subscribers 输出参数：订阅者集合的副本
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_subscribers(SubscriberSet& subscribers) const;

  /**
   * @brief 检查是否有订阅者
   * @param has_subs 输出参数：是否有订阅者
   * @return MQ_SUCCESS成功，其他值失败
   */
  int has_subscribers(bool& has_subs) const;

  /**
   * @brief 检查是否有子节点
   * @param has_childs 输出参数：是否有子节点
   * @return MQ_SUCCESS成功，其他值失败
   */
  int has_children(bool& has_childs) const;

  /**
   * @brief 获取节点使用的分配器
   * @return MQTT分配器指针
   */
  MQTTAllocator* get_allocator() const { return allocator_; }

 private:
  friend class ConcurrentTopicTree;
  friend class IntermediateNode;

  // MQTT分配器
  MQTTAllocator* allocator_;

  // 使用原子指针指向中间节点，实现无锁操作
  std::atomic<IntermediateNode*> intermediate_node_;

  // 获取当前的中间节点
  IntermediateNode* get_intermediate_node() const;

  // CAS更新中间节点
  bool compare_and_swap_intermediate_node(IntermediateNode* expected, IntermediateNode* desired);
};

/**
 * @brief 中间节点（I-node）
 * 存储节点的实际数据，支持写时复制（Copy-on-Write）
 */
class IntermediateNode
{
 public:
  explicit IntermediateNode(MQTTAllocator* allocator);
  ~IntermediateNode();

  // 禁止拷贝和赋值
  IntermediateNode(const IntermediateNode&) = delete;
  IntermediateNode& operator=(const IntermediateNode&) = delete;

  /**
   * @brief 创建副本
   * @param cloned_node 输出参数：新的中间节点指针
   * @return MQ_SUCCESS成功，其他值失败
   */
  int clone(IntermediateNode*& cloned_node) const;

  /**
   * @brief 添加子节点
   * @param level 主题级别
   * @param child 子节点
   * @return MQ_SUCCESS成功，其他值失败
   */
  int add_child(const MQTTString& level, std::shared_ptr<TopicTreeNode> child);

  /**
   * @brief 移除子节点
   * @param level 主题级别
   * @return MQ_SUCCESS成功，其他值失败
   */
  int remove_child(const MQTTString& level);

  /**
   * @brief 添加订阅者
   * @param subscriber 订阅者信息
   * @return MQ_SUCCESS成功，其他值失败
   */
  int add_subscriber(const SubscriberInfo& subscriber);

  /**
   * @brief 移除订阅者
   * @param client_id 客户端ID
   * @return MQ_SUCCESS成功，其他值失败
   */
  int remove_subscriber(const MQTTString& client_id);

  // 数据访问接口
  /**
   * @brief 获取子节点
   * @param level 主题级别
   * @param child 输出参数：子节点指针
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_child(const MQTTString& level, std::shared_ptr<TopicTreeNode>& child) const;

  /**
   * @brief 获取所有子节点的引用
   * @param children 输出参数：子节点映射的引用
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_children(const TopicTreeChildrenMap*& children) const;

  /**
   * @brief 获取订阅者集合的引用
   * @param subscribers 输出参数：订阅者集合的引用
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_subscribers(const SubscriberSet*& subscribers) const;

  /**
   * @brief 获取分配器指针
   * @return MQTT分配器指针（永远不会失败）
   */
  MQTTAllocator* get_allocator() const { return allocator_; }

  /**
   * @brief 安全复制容器数据
   * @param dest 目标节点
   * @return MQ_SUCCESS成功，其他值失败
   */
  int safe_copy_containers(IntermediateNode* dest) const;

 private:
  // MQTT分配器
  MQTTAllocator* allocator_;

  // 子节点映射（使用自定义分配器和MQTTString）
  TopicTreeChildrenMap children_;

  // 订阅者集合（使用自定义分配器）
  SubscriberSet subscribers_;

  // 引用计数，用于安全回收
  std::atomic<int> ref_count_;

  friend void intrusive_ptr_add_ref(IntermediateNode* node);
  friend void intrusive_ptr_release(IntermediateNode* node);
};

/**
 * @brief 引用计数管理函数
 */
void intrusive_ptr_add_ref(IntermediateNode* node);
void intrusive_ptr_release(IntermediateNode* node);

/**
 * @brief 主题匹配结果
 */
struct TopicMatchResult
{
  TopicTreeVector<SubscriberInfo> subscribers;
  size_t total_count;

  explicit TopicMatchResult(MQTTAllocator* allocator)
      : subscribers(TopicTreeAllocator<SubscriberInfo>(allocator)), total_count(0)
  {
  }

  // 默认构造函数（使用标准allocator作为fallback）
  TopicMatchResult() : subscribers(), total_count(0) {}
};

/**
 * @brief 并发主题树
 * 高性能的MQTT主题匹配树，支持通配符和无锁并发操作
 */
class ConcurrentTopicTree
{
 public:
  explicit ConcurrentTopicTree(MQTTAllocator* allocator);
  ~ConcurrentTopicTree();

  // 禁止拷贝和赋值
  ConcurrentTopicTree(const ConcurrentTopicTree&) = delete;
  ConcurrentTopicTree& operator=(const ConcurrentTopicTree&) = delete;

  /**
   * @brief 订阅主题
   * @param topic_filter 主题过滤器（支持通配符）
   * @param client_id 客户端ID
   * @param qos QoS级别
   * @return 0成功，非0失败
   */
  int subscribe(const MQTTString& topic_filter, const MQTTString& client_id, uint8_t qos = 0);

  /**
   * @brief 取消订阅
   * @param topic_filter 主题过滤器
   * @param client_id 客户端ID
   * @return 0成功，非0失败
   */
  int unsubscribe(const MQTTString& topic_filter, const MQTTString& client_id);

  /**
   * @brief 取消客户端的所有订阅
   * @param client_id 客户端ID
   * @return 取消的订阅数量
   */
  int unsubscribe_all(const MQTTString& client_id);

  /**
   * @brief 查找匹配主题的订阅者
   * @param topic 发布主题
   * @param result 输出参数：匹配结果
   * @return MQ_SUCCESS成功，其他值失败
   */
  int find_subscribers(const MQTTString& topic, TopicMatchResult& result) const;

  /**
   * @brief 获取订阅者数量统计
   * @param total_count 输出参数：总订阅者数量
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_total_subscribers(size_t& total_count) const;

  /**
   * @brief 获取节点数量统计
   * @param total_count 输出参数：总节点数量
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_total_nodes(size_t& total_count) const;

  /**
   * @brief 清理空节点
   * @param cleaned_count 输出参数：清理的节点数量
   * @return MQ_SUCCESS成功，其他值失败
   */
  int cleanup_empty_nodes(size_t& cleaned_count);

  /**
   * @brief 获取客户端的所有订阅
   * @param client_id 客户端ID
   * @param subscriptions 输出参数：订阅的主题过滤器列表
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_client_subscriptions(const MQTTString& client_id,
                               std::vector<MQTTString>& subscriptions) const;

  /**
   * @brief 获取使用的分配器
   * @return MQTT分配器指针
   */
  MQTTAllocator* get_allocator() const { return allocator_; }

  /**
   * @brief 检查主题树是否正确初始化
   * @return true如果初始化成功，false否则
   */
  bool is_initialized() const { return root_ != nullptr; }

  /**
   * @brief 获取分配器的内存使用统计
   * @param memory_usage 输出参数：内存使用字节数
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_memory_usage(size_t& memory_usage) const;

 private:
  // MQTT分配器
  MQTTAllocator* allocator_;

  // 根节点
  std::shared_ptr<TopicTreeNode> root_;

  // 统计信息
  mutable std::atomic<size_t> total_subscribers_;
  mutable std::atomic<size_t> total_nodes_;
  
  // Mutex for node creation to prevent race conditions
  mutable std::mutex node_creation_mutex_;

  /**
   * @brief 分割主题为级别
   * @param topic 主题字符串
   * @param levels 输出参数：级别向量
   * @return MQ_SUCCESS成功，其他值失败
   */
  int split_topic_levels(const MQTTString& topic, TopicTreeLevelVector& levels) const;

  /**
   * @brief 获取或创建节点
   * @param levels 主题级别
   * @param create_if_not_exists 不存在时是否创建
   * @param node 输出参数：节点指针
   * @return MQ_SUCCESS成功，其他值失败
   */
  int get_or_create_node(const TopicTreeLevelVector& levels, bool create_if_not_exists,
                         std::shared_ptr<TopicTreeNode>& node);

  /**
   * @brief 递归查找匹配的订阅者
   * @param node 当前节点
   * @param topic_levels 剩余主题级别
   * @param level_index 当前级别索引
   * @param result 结果累积器
   * @return MQ_SUCCESS成功，其他值失败
   */
  int find_subscribers_recursive(const std::shared_ptr<TopicTreeNode>& node,
                                 const TopicTreeLevelVector& topic_levels, size_t level_index,
                                 TopicMatchResult& result) const;

  /**
   * @brief 添加订阅者到节点（无锁实现）
   * @param node 目标节点
   * @param subscriber 订阅者信息
   * @return MQ_SUCCESS成功，其他值失败
   */
  int add_subscriber_to_node(std::shared_ptr<TopicTreeNode> node, const SubscriberInfo& subscriber);

  /**
   * @brief 从节点移除订阅者（无锁实现）
   * @param node 目标节点
   * @param client_id 客户端ID
   * @return MQ_SUCCESS成功，其他值失败
   */
  int remove_subscriber_from_node(std::shared_ptr<TopicTreeNode> node,
                                  const MQTTString& client_id);

  /**
   * @brief 递归收集客户端订阅
   * @param node 当前节点
   * @param current_path 当前路径
   * @param client_id 客户端ID
   * @param result 结果集合
   * @return MQ_SUCCESS成功，其他值失败
   */
  int collect_client_subscriptions(const std::shared_ptr<TopicTreeNode>& node,
                                   const MQTTString& current_path, const MQTTString& client_id,
                                   std::vector<MQTTString>& result) const;

  /**
   * @brief 递归清理空节点
   * @param node 当前节点
   * @param path 节点路径
   * @param should_delete 输出参数：是否应该删除此节点
   * @return MQ_SUCCESS成功，其他值失败
   */
  int cleanup_empty_nodes_recursive(std::shared_ptr<TopicTreeNode> node, const MQTTString& path,
                                    bool& should_delete);
};

}  // namespace mqtt

#endif  // MQTT_TOPIC_TREE_H