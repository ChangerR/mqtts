#include "mqtt_allocator.h"
#include <pthread.h>
#include <utility>
#include "logger.h"

// Initialize thread local root allocator
__thread MQTTAllocator* MQTTMemoryManager::thread_local_root_ = nullptr;

// MQTTAllocator implementation
MQTTAllocator::MQTTAllocator(const std::string& id, const std::string& tag, size_t limit,
                             MQTTAllocator* parent)
    : id_(id), tag_(tag), limit_(limit), parent_(parent), usage_(0)
{
}

MQTTAllocator::~MQTTAllocator()
{
  for (std::unordered_map<std::string, MQTTAllocator*>::iterator it = children_.begin();
       it != children_.end(); ++it) {
    delete it->second;
  }
  children_.clear();
}

void MQTTMemoryManager::add_tag_usage(const std::string& tag, size_t size)
{
  pthread_rwlock_rdlock(&tag_usage_rwlock_);
  std::unordered_map<std::string, std::atomic<size_t>>::iterator it = tag_usage_map_.find(tag);
  if (it != tag_usage_map_.end()) {
    it->second += size;
  }
  pthread_rwlock_unlock(&tag_usage_rwlock_);
}

void MQTTMemoryManager::sub_tag_usage(const std::string& tag, size_t size)
{
  pthread_rwlock_rdlock(&tag_usage_rwlock_);
  std::unordered_map<std::string, std::atomic<size_t>>::iterator it = tag_usage_map_.find(tag);
  if (it != tag_usage_map_.end()) {
    it->second -= size;
  }
  pthread_rwlock_unlock(&tag_usage_rwlock_);
}

size_t MQTTMemoryManager::get_tag_usage(const std::string& tag)
{
  pthread_rwlock_rdlock(&tag_usage_rwlock_);
  std::unordered_map<std::string, std::atomic<size_t>>::iterator it = tag_usage_map_.find(tag);
  size_t usage = it != tag_usage_map_.end() ? it->second.load() : 0;
  pthread_rwlock_unlock(&tag_usage_rwlock_);
  return usage;
}

void* MQTTAllocator::allocate(size_t size)
{
  // Check memory limit
  if (limit_ > 0 && usage_ + size > limit_) {
    LOG_WARN(
        "Memory allocation failed: limit exceeded for id {} tag {} (limit: {}, used: {}, "
        "requested: {})",
        id_.c_str(), tag_.c_str(), limit_, usage_.load(), size);
    return nullptr;
  }
  // Allocate memory using tcmalloc
  void* ptr = tc_malloc(size);
  if (ptr) {
    usage_ += size;
    MQTTMemoryManager::get_instance().add_tag_usage(tag_, size);
    LOG_DEBUG("Allocated {} bytes for id {} tag {} (used: {}/{})", size, id_.c_str(), tag_.c_str(),
              usage_.load(), limit_);
  }
  return ptr;
}

void MQTTAllocator::deallocate(void* ptr, size_t size)
{
  if (!ptr)
    return;

  tc_free(ptr);
  usage_ -= size;
  MQTTMemoryManager::get_instance().sub_tag_usage(tag_, size);
  LOG_DEBUG("Freed {} bytes from id {} tag {} (used: {}/{})", size, id_.c_str(), tag_.c_str(),
            usage_.load(), limit_);
}

MQTTAllocator* MQTTAllocator::create_child(const std::string& child_id, const std::string& tag,
                                           size_t limit)
{
  if (children_.count(child_id))
    return nullptr;
  MQTTAllocator* child = new MQTTAllocator(child_id, tag, limit, this);
  children_[child_id] = child;
  return child;
}

void MQTTAllocator::remove_child(const std::string& child_id)
{
  std::unordered_map<std::string, MQTTAllocator*>::iterator it = children_.find(child_id);
  if (it != children_.end()) {
    delete it->second;
    children_.erase(it);
  }
}

size_t MQTTAllocator::get_memory_usage() const
{
  return usage_.load();
}

size_t MQTTAllocator::get_memory_limit() const
{
  return limit_;
}

const std::string& MQTTAllocator::get_id() const
{
  return id_;
}

const std::string& MQTTAllocator::get_tag() const
{
  return tag_;
}

MQTTAllocator* MQTTAllocator::get_parent() const
{
  return parent_;
}

size_t MQTTAllocator::get_total_memory_usage() const
{
  size_t total = usage_.load();
  for (std::unordered_map<std::string, MQTTAllocator*>::const_iterator it = children_.begin();
       it != children_.end(); ++it) {
    total += it->second->get_total_memory_usage();
  }
  return total;
}

// MQTTMemoryManager implementation
MQTTMemoryManager::MQTTMemoryManager()
{
  pthread_rwlock_init(&tag_usage_rwlock_, nullptr);
}

MQTTMemoryManager::~MQTTMemoryManager()
{
  pthread_rwlock_destroy(&tag_usage_rwlock_);
}

size_t MQTTMemoryManager::get_total_memory_usage()
{
  pthread_rwlock_rdlock(&tag_usage_rwlock_);
  size_t total = 0;
  for (const std::pair<const std::string, std::atomic<size_t>>& pair : tag_usage_map_) {
    total += pair.second.load();
  }
  pthread_rwlock_unlock(&tag_usage_rwlock_);
  return total;
}
