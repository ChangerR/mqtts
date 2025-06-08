#include "mqtt_allocator.h"
#include <pthread.h>
#include <memory>
#include <utility>
#include "logger.h"

// Initialize thread local root allocator
__thread MQTTAllocator* MQTTMemoryManager::thread_local_root_ = nullptr;

// MQTTAllocator implementation
MQTTAllocator::MQTTAllocator(const std::string& id, MQTTMemoryTag tag, size_t limit,
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

void MQTTMemoryManager::add_tag_usage(MQTTMemoryTag tag, size_t size)
{
  size_t index = static_cast<size_t>(tag);
  if (index < tag_usage_vector_.size()) {
    *(tag_usage_vector_[index]) += size;
  }
}

void MQTTMemoryManager::sub_tag_usage(MQTTMemoryTag tag, size_t size)
{
  size_t index = static_cast<size_t>(tag);
  if (index < tag_usage_vector_.size()) {
    *(tag_usage_vector_[index]) -= size;
  }
}

size_t MQTTMemoryManager::get_tag_usage(MQTTMemoryTag tag)
{
  size_t index = static_cast<size_t>(tag);
  if (index < tag_usage_vector_.size()) {
    return tag_usage_vector_[index]->load();
  }
  return 0;
}

void* MQTTAllocator::allocate(size_t size)
{
  // Check memory limit
  if (limit_ > 0 && usage_ + size > limit_) {
    LOG_WARN(
        "Memory allocation failed: limit exceeded for id {} tag {} (limit: {}, used: {}, "
        "requested: {})",
        id_.c_str(), get_memory_tag_str(tag_), limit_, usage_.load(), size);
    return nullptr;
  }
  // Allocate memory using tcmalloc
  void* ptr = tc_malloc(size);
  if (ptr) {
    usage_ += size;
    MQTTMemoryManager::get_instance().add_tag_usage(tag_, size);
    LOG_DEBUG("Allocated {} bytes for id {} tag {} (used: {}/{})", size, id_.c_str(),
              get_memory_tag_str(tag_), usage_.load(), limit_);
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
  LOG_DEBUG("Freed {} bytes from id {} tag {} (used: {}/{})", size, id_.c_str(),
            get_memory_tag_str(tag_), usage_.load(), limit_);
}

MQTTAllocator* MQTTAllocator::create_child(const std::string& child_id, MQTTMemoryTag tag,
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

MQTTMemoryTag MQTTAllocator::get_tag() const
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
  // Initialize vector with the size of enum count
  tag_usage_vector_.reserve(static_cast<size_t>(MQTTMemoryTag::MEM_TAG_COUNT));
  // Initialize all vector elements to 0
  for (size_t i = 0; i < static_cast<size_t>(MQTTMemoryTag::MEM_TAG_COUNT); ++i) {
    tag_usage_vector_.push_back(std::unique_ptr<std::atomic<size_t>>(new std::atomic<size_t>(0)));
  }
}

MQTTMemoryManager::~MQTTMemoryManager() {}

size_t MQTTMemoryManager::get_total_memory_usage()
{
  size_t total = 0;
  for (const auto& usage : tag_usage_vector_) {
    total += usage->load();
  }
  return total;
}
