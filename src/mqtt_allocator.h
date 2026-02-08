#ifndef MQTT_ALLOCATOR_H
#define MQTT_ALLOCATOR_H

#include <pthread.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "gperftools/tcmalloc.h"
#include "mqtt_define.h"
#include "mqtt_memory_tags.h"

class MQTTAllocator
{
 public:
  MQTTAllocator(const std::string& id, MQTTMemoryTag tag, size_t limit = 0,
                MQTTAllocator* parent = nullptr);
  virtual ~MQTTAllocator();

  // Memory allocation methods - thread unsafe
  void* allocate(size_t size);
  void deallocate(void* ptr, size_t size);

  // Child allocator management - thread unsafe
  MQTTAllocator* create_child(const std::string& child_id, MQTTMemoryTag tag, size_t limit = 0);
  void remove_child(const std::string& child_id);
  MQTTAllocator* get_child(const std::string& child_id) const
  {
    std::unordered_map<std::string, MQTTAllocator*>::const_iterator it = children_.find(child_id);
    return it != children_.end() ? it->second : nullptr;
  }

  // Memory usage statistics - thread unsafe
  size_t get_memory_usage() const;
  size_t get_memory_limit() const;
  const std::string& get_id() const;
  MQTTMemoryTag get_tag() const;
  MQTTAllocator* get_parent() const;
  size_t get_total_memory_usage() const;

 private:
  std::string id_;             // Unique identifier (e.g. client id)
  MQTTMemoryTag tag_;          // Memory usage category
  size_t limit_;               // 0 means no limit
  std::atomic<size_t> usage_;  // Use atomic for memory usage tracking
  MQTTAllocator* parent_;
  std::unordered_map<std::string, MQTTAllocator*> children_;
};

// Global memory manager - thread safe
class MQTTMemoryManager
{
 public:
  static MQTTMemoryManager& get_instance()
  {
    static MQTTMemoryManager instance;
    return instance;
  }

  // Get thread local root allocator
  MQTTAllocator* get_root_allocator()
  {
    if (!thread_local_root_) {
      std::stringstream ss;
      ss << "root_" << pthread_self();
      thread_local_root_ = new MQTTAllocator(ss.str(), MQTTMemoryTag::MEM_TAG_ROOT, 0);
    }
    return thread_local_root_;
  }
  
  // Cleanup thread local allocator
  static void cleanup_thread_local()
  {
    if (thread_local_root_) {
      delete thread_local_root_;
      thread_local_root_ = nullptr;
    }
  }

  // Get allocator for a specific client - thread safe
  MQTTAllocator* get_allocator(const std::string& client_id) const
  {
    if (!thread_local_root_) {
      MQTTMemoryManager& manager = const_cast<MQTTMemoryManager&>(*this);
      thread_local_root_ = manager.get_root_allocator();
    }
    return thread_local_root_->get_child(client_id);
  }

  // Tag usage statistics - thread safe
  void add_tag_usage(MQTTMemoryTag tag, size_t size);
  void sub_tag_usage(MQTTMemoryTag tag, size_t size);
  size_t get_tag_usage(MQTTMemoryTag tag);
  size_t get_tag_memory_usage(MQTTMemoryTag tag);
  size_t get_total_memory_usage();

 private:
  MQTTMemoryManager();
  ~MQTTMemoryManager();

  // Thread local root allocator
  static __thread MQTTAllocator* thread_local_root_;

  // Global tag usage statistics using vector of unique_ptr to atomic
  std::vector<std::unique_ptr<std::atomic<size_t>>> tag_usage_vector_;
};

// Memory allocation macros
#define MQ_MEM_MANAGER MQTTMemoryManager::get_instance()
#define MQ_MEM_ALLOC(client_id, size) (MQ_MEM_MANAGER.get_allocator(client_id)->allocate(size))
#define MQ_MEM_FREE(client_id, ptr, size) \
  if (ptr)                                \
  MQ_MEM_MANAGER.get_allocator(client_id)->deallocate(ptr, size)

#endif