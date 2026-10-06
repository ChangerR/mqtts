#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "mqtt_log_codec.h"

namespace mqtt {
namespace journal {
class AdmissionFull : public std::runtime_error
{
 public:
  explicit AdmissionFull(const char* message) : std::runtime_error(message) {}
};
struct Location
{
  uint64_t segment = 0, offset = 0, serial = 0;
  uint32_t size = 0;
};
struct Completion
{
  explicit Completion(bool waiter);
  ~Completion();
  std::atomic<bool> done{false};
  bool ok = false;
  std::string error;
  Location location;
  int fd = -1;
  bool wait();
  void finish();
};

// Each log owns its append descriptor and OS worker. No SQLite or network dependency.
class AppendLog
{
 public:
  using Callback = std::function<void(bool, const Location&)>;
  using Visitor = std::function<void(const Location&, const std::string&)>;
  AppendLog(const std::string& directory, size_t segment_bytes, size_t disk_bytes);
  ~AppendLog();
  AppendLog(const AppendLog&) = delete;
  std::shared_ptr<Completion> append(std::string data, Callback applied, bool waiter = true);
  // Called by maintenance with store admission frozen; callbacks finish before the fence.
  std::shared_ptr<Completion> seal();
  void replay(uint64_t after, const Visitor& visit);
  std::string read(const Location& location) const;
  void prune(uint64_t cut, const std::set<uint64_t>& retained);
  uint64_t serial() const { return serial_; }
  size_t disk_bytes() const { return disk_bytes_.load(); }
  bool needs_checkpoint() const;
  const std::string& directory() const { return directory_; }

 private:
  struct Entry
  {
    std::string data;
    Callback applied;
    std::shared_ptr<Completion> completion;
    bool seal = false;
    bool reclaim = false;
    uint64_t cut = 0;
    std::set<uint64_t> retained;
  };
  struct Segment
  {
    size_t size = 0;
    uint64_t last = 0;
  };
  std::string directory_;
  size_t segment_bytes_, max_disk_bytes_, reserved_bytes_ = 0;
  std::atomic<size_t> disk_bytes_{0};
  std::map<uint64_t, Segment> segments_;
  uint64_t serial_ = 0, next_segment_ = 1, active_ = 0;
  int fd_ = -1, directory_fd_ = -1;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Entry> queue_;
  bool stopping_ = false, failed_ = false;
  std::thread worker_;
  std::string filename(uint64_t segment) const;
  void work();
  void recover();
  void scan(uint64_t segment, bool repair_tail, const Visitor& visit);
  void close_segment();
  void prune_segments(uint64_t cut, const std::set<uint64_t>& retained);
  Location write_record(const std::string& data);
};

void ensure_directory(const std::string& path);
void atomic_file(const std::string& path, const std::string& data);
std::string read_file(const std::string& path, size_t maximum);
}  // namespace journal
}  // namespace mqtt
