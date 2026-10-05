#include "mqtt_append_log.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "mqtt_runtime.h"

namespace mqtt {
namespace journal {
namespace {
constexpr uint32_t magic = 0x314c514d;  // MQL1
constexpr size_t header_size = 24, max_record = 64 * 1024 * 1024;
void check(bool ok, const char* message)
{
  if (!ok)
    throw std::runtime_error(std::string(message) + ": " + std::strerror(errno));
}
void write_all(int fd, const std::string& data)
{
  size_t done = 0;
  while (done < data.size()) {
    ssize_t n = ::write(fd, data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR)
      continue;
    check(n > 0, "journal write");
    done += size_t(n);
  }
}
std::string read_at(int fd, uint64_t offset, size_t bytes)
{
  std::string data(bytes, '\0');
  size_t done = 0;
  while (done < bytes) {
    ssize_t n = pread(fd, &data[done], bytes - done, off_t(offset + done));
    if (n < 0 && errno == EINTR)
      continue;
    check(n >= 0, "journal read");
    if (n == 0)
      break;
    done += size_t(n);
  }
  data.resize(done);
  return data;
}
std::string parent(const std::string& path)
{
  auto pos = path.find_last_of('/');
  return pos == std::string::npos ? "." : (pos == 0 ? "/" : path.substr(0, pos));
}
void sync_parent(const std::string& path)
{
  int fd = open(parent(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  check(fd >= 0, "open journal parent");
  int result = fsync(fd);
  close(fd);
  check(result == 0, "sync journal parent");
}
struct Header
{
  uint32_t bytes, crc;
  uint64_t serial;
};
Header decode_header(const std::string& data)
{
  if (data.size() != header_size)
    throw std::runtime_error("incomplete journal header");
  Decoder d(data);
  if (d.u32() != magic)
    throw std::runtime_error("invalid journal magic/version");
  Header h;
  h.bytes = d.u32();
  h.serial = d.u64();
  h.crc = d.u32();
  if (d.u32() != checksum(data.data(), 20) || !h.bytes || h.bytes > max_record || !h.serial)
    throw std::runtime_error("corrupt journal header");
  return h;
}
std::string frame(uint64_t serial, const std::string& data)
{
  if (data.empty() || data.size() > max_record)
    throw std::runtime_error("invalid journal record size");
  Encoder e;
  e.u32(magic);
  e.u32(uint32_t(data.size()));
  e.u64(serial);
  e.u32(checksum(data.data(), data.size()));
  e.u32(checksum(e.data.data(), e.data.size()));
  e.data.append(data);
  return e.data;
}
}  // namespace
uint32_t checksum(const char* data, size_t size)
{
  static const auto table = [] {
    std::vector<uint32_t> values(256);
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int b = 0; b < 8; ++b)
        c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1)));
      values[i] = c;
    }
    return values;
  }();
  uint32_t c = 0xffffffffU;
  for (size_t i = 0; i < size; ++i)
    c = table[(c ^ uint8_t(data[i])) & 255] ^ (c >> 8);
  return c ^ 0xffffffffU;
}
uint64_t topic_hash(const std::string& text)
{
  uint64_t value = 14695981039346656037ULL;
  for (unsigned char c : text) {
    value ^= c;
    value *= 1099511628211ULL;
  }
  return value;
}
void ensure_directory(const std::string& path)
{
  if (mkdir(path.c_str(), 0700) == 0) {
    sync_parent(path);
    return;
  }
  struct stat st
  {
  };
  check(errno == EEXIST && lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode),
        "journal path must be a directory (legacy SQLite requires offline migration)");
}
void atomic_file(const std::string& path, const std::string& data)
{
  const std::string temporary = path + ".tmp";
  int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  check(fd >= 0, "open journal checkpoint");
  try {
    write_all(fd, data);
    check(fsync(fd) == 0, "sync journal checkpoint");
  } catch (...) {
    close(fd);
    throw;
  }
  close(fd);
  check(rename(temporary.c_str(), path.c_str()) == 0, "rename journal checkpoint");
  sync_parent(path);
}
std::string read_file(const std::string& path, size_t maximum)
{
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0 && errno == ENOENT)
    return {};
  check(fd >= 0, "open journal file");
  try {
    struct stat st
    {
    };
    check(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
              uint64_t(st.st_size) <= maximum,
          "journal file size");
    auto result = read_at(fd, 0, size_t(st.st_size));
    close(fd);
    return result;
  } catch (...) {
    close(fd);
    throw;
  }
}
Completion::Completion(bool waiter)
{
  if (waiter) {
    fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    check(fd >= 0, "journal completion eventfd");
  }
}
Completion::~Completion()
{
  if (fd >= 0)
    close(fd);
}
bool Completion::wait()
{
  while (!done.load(std::memory_order_acquire))
    runtime::current_runtime().wait_readable(fd, 100);
  return ok;
}
void Completion::finish()
{
  done.store(true, std::memory_order_release);
  if (fd >= 0) {
    uint64_t one = 1;
    (void)::write(fd, &one, sizeof(one));
  }
}
AppendLog::AppendLog(const std::string& directory, size_t segment_bytes, size_t disk_bytes)
    : directory_(directory), segment_bytes_(segment_bytes), max_disk_bytes_(disk_bytes)
{
  ensure_directory(directory_);
  directory_fd_ = open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  check(directory_fd_ >= 0, "open journal directory");
  try {
    recover();
    worker_ = std::thread(&AppendLog::work, this);
  } catch (...) {
    close(directory_fd_);
    throw;
  }
}
AppendLog::~AppendLog()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable())
    worker_.join();
  if (fd_ >= 0)
    close(fd_);
  if (directory_fd_ >= 0)
    close(directory_fd_);
}
std::string AppendLog::filename(uint64_t segment) const
{
  char name[40];
  std::snprintf(name, sizeof(name), "/%020llu.log", static_cast<unsigned long long>(segment));
  return directory_ + name;
}
void AppendLog::recover()
{
  DIR* dir = opendir(directory_.c_str());
  check(dir != nullptr, "list journal segments");
  while (auto* entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name == "." || name == "..")
      continue;
    if (name.size() != 24 || name.substr(20) != ".log" ||
        name.substr(0, 20).find_first_not_of("0123456789") != std::string::npos) {
      closedir(dir);
      throw std::runtime_error("unexpected file in journal partition");
    }
    uint64_t id = std::stoull(name.substr(0, 20));
    if (!id) {
      closedir(dir);
      throw std::runtime_error("invalid journal segment id");
    }
    segments_[id] = Segment();
    next_segment_ = std::max(next_segment_, id + 1);
  }
  closedir(dir);
  for (auto& segment : segments_)
    scan(segment.first, segment.first == segments_.rbegin()->first,
         [&](const Location& loc, const std::string&) {
           if (loc.serial <= serial_)
             throw std::runtime_error("journal sequence regression");
           serial_ = loc.serial;
         });
}
void AppendLog::scan(uint64_t segment, bool repair_tail, const Visitor& visit)
{
  int fd =
      open(filename(segment).c_str(), (repair_tail ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW);
  check(fd >= 0, "open journal segment");
  try {
    struct stat st
    {
    };
    check(fstat(fd, &st) == 0 && S_ISREG(st.st_mode), "stat journal segment");
    uint64_t offset = 0, last = 0;
    while (offset < uint64_t(st.st_size)) {
      auto raw = read_at(fd, offset, header_size);
      if (raw.size() < header_size) {
        if (!repair_tail)
          throw std::runtime_error("incomplete sealed journal segment");
        check(ftruncate(fd, off_t(offset)) == 0 && fsync(fd) == 0, "repair torn journal header");
        break;
      }
      auto h = decode_header(raw);
      auto data = read_at(fd, offset + header_size, h.bytes);
      if (data.size() != h.bytes) {
        if (!repair_tail)
          throw std::runtime_error("incomplete sealed journal record");
        check(ftruncate(fd, off_t(offset)) == 0 && fsync(fd) == 0, "repair torn journal body");
        break;
      }
      if (checksum(data.data(), data.size()) != h.crc || h.serial <= last)
        throw std::runtime_error("journal checksum/sequence mismatch");
      Location location;
      location.segment = segment;
      location.offset = offset;
      location.serial = h.serial;
      location.size = h.bytes;
      visit(location, data);
      last = h.serial;
      offset += header_size + h.bytes;
    }
    auto& info = segments_[segment];
    disk_bytes_ -= info.size;
    info.size = size_t(offset);
    info.last = last;
    disk_bytes_ += info.size;
    close(fd);
  } catch (...) {
    close(fd);
    throw;
  }
}
void AppendLog::replay(uint64_t after, const Visitor& visit)
{
  uint64_t expected = after + 1;
  for (const auto& segment : segments_)
    scan(segment.first, false, [&](const Location& loc, const std::string& data) {
      if (loc.serial <= after)
        return;
      if (loc.serial != expected++)
        throw std::runtime_error("missing journal records after checkpoint");
      visit(loc, data);
    });
  // Every checkpoint fence may outlive all its segments after reclamation.
  serial_ = std::max(serial_, after);
}
std::string AppendLog::read(const Location& location) const
{
  int fd = open(filename(location.segment).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  check(fd >= 0, "open referenced journal segment");
  try {
    auto h = decode_header(read_at(fd, location.offset, header_size));
    auto data = read_at(fd, location.offset + header_size, h.bytes);
    if (h.serial != location.serial || h.bytes != location.size || data.size() != h.bytes ||
        checksum(data.data(), data.size()) != h.crc)
      throw std::runtime_error("invalid checkpoint journal reference");
    close(fd);
    return data;
  } catch (...) {
    close(fd);
    throw;
  }
}
std::shared_ptr<Completion> AppendLog::append(std::string data, Callback applied, bool waiter)
{
  auto completion = std::make_shared<Completion>(waiter);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || failed_)
      throw std::runtime_error("journal writer unavailable");
    size_t bytes = data.size() + header_size, used = disk_bytes_.load();
    if (bytes > max_disk_bytes_ || used > max_disk_bytes_ - bytes ||
        reserved_bytes_ > max_disk_bytes_ - bytes - used)
      throw AdmissionFull("journal disk admission full; drain/checkpoint before retry");
    Entry e;
    e.data = std::move(data);
    e.applied = std::move(applied);
    e.completion = completion;
    queue_.push_back(std::move(e));
    reserved_bytes_ += bytes;
  }
  changed_.notify_one();
  return completion;
}
std::shared_ptr<Completion> AppendLog::seal()
{
  auto completion = std::make_shared<Completion>(true);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry e;
    e.seal = true;
    e.completion = completion;
    queue_.push_back(std::move(e));
  }
  changed_.notify_one();
  return completion;
}
void AppendLog::close_segment()
{
  if (fd_ >= 0) {
    check(fdatasync(fd_) == 0, "sync journal segment");
    close(fd_);
    fd_ = -1;
    active_ = 0;
  }
}
Location AppendLog::write_record(const std::string& data)
{
  auto bytes = frame(serial_ + 1, data);
  if (bytes.size() > max_disk_bytes_ || disk_bytes_ > max_disk_bytes_ - bytes.size())
    throw std::runtime_error("journal disk quota exhausted");
  if (fd_ >= 0 && segments_[active_].size + bytes.size() > segment_bytes_)
    close_segment();
  if (fd_ < 0) {
    active_ = next_segment_++;
    fd_ =
        open(filename(active_).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    check(fd_ >= 0, "create journal segment");
    check(fsync(directory_fd_) == 0, "sync new journal segment directory");
    segments_[active_] = Segment();
  }
  Location location;
  location.segment = active_;
  location.offset = segments_[active_].size;
  location.serial = ++serial_;
  location.size = uint32_t(data.size());
  write_all(fd_, bytes);
  auto& segment = segments_[active_];
  segment.size += bytes.size();
  segment.last = serial_;
  disk_bytes_ += bytes.size();
  return location;
}
void AppendLog::work()
{
  for (;;) {
    std::vector<Entry> batch;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      if (stopping_ && queue_.empty())
        break;
      changed_.wait_for(lock, std::chrono::milliseconds(1),
                        [&] { return stopping_ || queue_.size() >= 64; });
      while (!queue_.empty() && batch.size() < 64) {
        batch.push_back(std::move(queue_.front()));
        queue_.pop_front();
      }
    }
    std::string error;
    try {
      if (failed_)
        throw std::runtime_error("journal writer failed");
      for (auto& entry : batch) {
        if (entry.seal)
          close_segment();
        else
          entry.completion->location = write_record(entry.data);
      }
      if (fd_ >= 0)
        check(fdatasync(fd_) == 0, "commit journal batch");
    } catch (const std::exception& e) {
      error = e.what();
      std::lock_guard<std::mutex> lock(mutex_);
      failed_ = true;
    }
    for (auto& entry : batch) {
      auto& c = *entry.completion;
      c.ok = error.empty();
      c.error = error;
      try {
        if (entry.applied)
          entry.applied(c.ok, c.location);
      } catch (const std::exception& e) {
        c.ok = false;
        c.error = e.what();
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
      }
      if (!entry.seal) {
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_bytes_ -= entry.data.size() + header_size;
      }
      c.finish();
    }
  }
}
void AppendLog::prune(uint64_t cut, const std::set<uint64_t>& retained)
{
  bool changed = false;
  for (auto it = segments_.begin(); it != segments_.end();) {
    if (it->first != active_ && it->second.last <= cut && !retained.count(it->first)) {
      check(unlink(filename(it->first).c_str()) == 0, "reclaim journal segment");
      disk_bytes_ -= it->second.size;
      it = segments_.erase(it);
      changed = true;
    } else
      ++it;
  }
  if (changed)
    check(fsync(directory_fd_) == 0, "sync journal reclamation");
}
bool AppendLog::needs_checkpoint() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return disk_bytes_.load() + reserved_bytes_ >= max_disk_bytes_ * 3 / 4;
}
}  // namespace journal
}  // namespace mqtt
