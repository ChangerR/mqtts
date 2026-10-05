#include "mqtt_durable_store.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include "logger.h"
#include "mqtt_append_log.h"
#include "mqtt_log_codec.h"
#include "mqtt_runtime.h"

namespace mqtt {
using journal::AppendLog;
using journal::Decoder;
using journal::Encoder;
using journal::Location;
using Ticket = std::shared_ptr<journal::Completion>;
namespace {
enum Record : uint8_t {
  CONNECT = 1,
  DISCONNECT = 2,
  SUBSCRIBE = 3,
  UNSUBSCRIBE = 4,
  ACK = 5,
  CLAIM = 6,
  HEARTBEAT = 7,
  MESSAGE = 8
};
struct Deferred
{
};
struct MaintenancePause
{
};
size_t subscription_cost(const std::string& filter)
{
  // Bound the flat topic trie as well as the saved filter text, including deeply
  // nested adversarial filters whose per-level nodes dominate their byte length.
  return 256 + filter.size() * 2 + size_t(std::count(filter.begin(), filter.end(), '/')) * 192;
}
struct DeliveryReservation
{
  explicit DeliveryReservation(const std::shared_ptr<std::atomic<size_t>>& counter)
      : counter(counter)
  {
  }
  ~DeliveryReservation() { counter->fetch_sub(bytes); }
  std::shared_ptr<std::atomic<size_t>> counter;
  size_t bytes = 0;
};
DurableStore::Result success()
{
  DurableStore::Result r;
  r.ok = true;
  return r;
}
DurableStore::Result stale()
{
  DurableStore::Result r;
  r.stale = true;
  r.error = "stale persistent connection";
  return r;
}
Location location_read(Decoder& d)
{
  Location p;
  p.segment = d.u64();
  p.offset = d.u64();
  p.serial = d.u64();
  p.size = d.u32();
  return p;
}
std::string checked(const std::string& value)
{
  Encoder e;
  e.u32(journal::checksum(value.data(), value.size()));
  e.data.append(value);
  return e.data;
}
std::string unchecked(const std::string& value)
{
  Decoder d(value);
  auto crc = d.u32();
  auto body = value.substr(4);
  if (journal::checksum(body.data(), body.size()) != crc)
    throw std::runtime_error("checkpoint checksum mismatch");
  return body;
}
}  // namespace
DurableStore::Signal::Signal()
{
  fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd_ < 0)
    throw std::runtime_error("persistent notification eventfd unavailable");
}
DurableStore::Signal::~Signal()
{
  if (fd_ >= 0)
    close(fd_);
}
void DurableStore::Signal::notify()
{
  revision_.fetch_add(1, std::memory_order_release);
  uint64_t one = 1;
  (void)::write(fd_, &one, sizeof(one));
}
void DurableStore::Signal::wait(uint64_t observed, int timeout_ms)
{
  uint64_t value;
  while (::read(fd_, &value, sizeof(value)) > 0) {
  }
  if (load() != observed)
    return;
  runtime::current_runtime().wait_readable(fd_, timeout_ms);
}
int64_t DurableStore::now_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct DurableStore::Impl
{
  struct Session;
  struct Message
  {
    int64_t id = 0, expires = 0;
    size_t partition = 0;
    size_t references = 0;
    size_t expiry_cursor = 0;
    std::vector<std::weak_ptr<Session>> recipients;
    Location location;
    std::string wire;
    bool committed = false;
  };
  struct DeliveryState
  {
    std::shared_ptr<Message> message;
    uint16_t packet = 0;
    bool attempted = false, started = false, ack_pending = false, claim_pending = false;
  };
  struct Session
  {
    std::string client, owner;
    uint64_t generation = 0, epoch = 0;
    uint32_t expiry = 0;
    int64_t deadline = 0;
    int64_t scheduled_expiry = 0;
    bool online = false, busy = false;
    uint16_t next_packet = 1;
    size_t bytes = 0;
    std::map<std::string, uint8_t> subscriptions;
    std::map<int64_t, DeliveryState> pending;
    std::unordered_map<uint16_t, int64_t> packets;
    std::vector<int64_t> recovery_acks;
    std::unordered_map<int64_t, uint16_t> recovery_claims;
    std::shared_ptr<Signal> signal;
  };
  struct TopicNode
  {
    std::unordered_map<std::string, size_t> children;
    std::unordered_map<std::string, std::shared_ptr<Session>> clients;
  };
  PersistenceConfig config;
  mutable std::mutex mutex;
  std::mutex checkpoint_mutex;
  std::condition_variable stopped;
  bool stopping = false, failed = false, indexing = false;
  std::atomic<bool> frozen{false};
  int lock_fd = -1;
  uint64_t next_id = 0;
  uint32_t format_version = 3;
  std::atomic<size_t> checkpoint_bytes{0};
  size_t pending = 0, bytes = 0, unique_bytes = 0, requests = 0, request_bytes = 0,
         metadata_bytes = 0, metadata_reserved = 0;
  std::shared_ptr<std::atomic<size_t>> delivery_bytes{new std::atomic<size_t>(0)};
  std::atomic<size_t> subscriptions{0};
  std::atomic<size_t> discarded_denied{0}, discarded_oversize{0}, discarded_malformed{0};
  std::map<std::string, std::shared_ptr<Session>> sessions;
  std::vector<TopicNode> index;
  std::vector<size_t> free_nodes;
  std::map<std::pair<int64_t, int64_t>, std::weak_ptr<Message>> message_expiry;
  std::map<std::pair<int64_t, std::string>, std::weak_ptr<Session>> session_expiry;
  std::vector<std::unique_ptr<AppendLog>> messages, states;
  std::vector<uint64_t> message_cuts, state_cuts;
  std::thread maintenance;
  explicit Impl(const PersistenceConfig& cfg);
  ~Impl();
  void fail()
  {
    failed = true;
    for (auto& item : sessions)
      if (item.second->signal)
        item.second->signal->notify();
  }
  Result call(const std::function<Ticket(const std::shared_ptr<Result>&)>& fn, bool wait = true,
              bool inspect_unavailable = false)
  {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (;;) {
      auto r = std::make_shared<Result>();
      Ticket ticket;
      try {
        {
          // A coroutine must never block its event thread behind snapshot work.
          std::unique_lock<std::mutex> guard(mutex, std::try_to_lock);
          if (!guard.owns_lock() && frozen.load())
            throw MaintenancePause();
          if (!guard.owns_lock())
            throw Deferred();
          if ((failed || stopping) && !inspect_unavailable) {
            r->error = "persistent journal unavailable";
            r->stale = true;
            return *r;
          }
          if (frozen && !inspect_unavailable)
            throw MaintenancePause();
          ticket = fn(r);
        }
        if (ticket && wait && !ticket->wait()) {
          r->ok = false;
          r->error = ticket->error;
        }
        return *r;
      } catch (const MaintenancePause&) {
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        runtime::current_runtime().wait(-1, 0, 1);
      } catch (const Deferred&) {
        if (std::chrono::steady_clock::now() >= deadline) {
          r->ok = false;
          r->error = "persistent admission timeout";
          return *r;
        }
        runtime::current_runtime().wait(-1, 0, 1);
      } catch (const std::exception& e) {
        r->ok = false;
        r->error = e.what();
        return *r;
      }
    }
  }
  Ticket enqueue(AppendLog& log, std::string data, const AppendLog::Callback& apply,
                 bool wait = true)
  {
    size_t size = data.size() + 256;
    if (size > config.max_request_bytes)
      throw std::runtime_error("persistent request exceeds byte limit");
    if (requests >= config.max_requests || request_bytes > config.max_request_bytes - size ||
        delivery_bytes->load() > config.max_request_bytes - size - request_bytes)
      throw Deferred();
    // Callers hold the state mutex. Writers publish completions after reservations
    // and pending-delivery entries have been installed, and only after fdatasync.
    auto ticket = log.append(
        std::move(data),
        [=](bool ok, const Location& location) {
          std::lock_guard<std::mutex> guard(mutex);
          --requests;
          request_bytes -= size;
          if (!ok)
            fail();
          try {
            apply(ok, location);
          } catch (...) {
            fail();
            throw;
          }
        },
        wait);
    ++requests;
    request_bytes += size;
    return ticket;
  }
  AppendLog& state_log(const std::string& client)
  {
    return *states[journal::topic_hash(client) % states.size()];
  }
  std::shared_ptr<Session> current(const std::string& client, uint64_t epoch)
  {
    auto it = sessions.find(client);
    if (it == sessions.end() || it->second->epoch != epoch || !it->second->online)
      return {};
    if (it->second->busy)
      throw Deferred();
    return it->second;
  }
  void notify(const std::shared_ptr<Session>& s)
  {
    if (s->signal)
      s->signal->notify();
  }
  size_t session_cost(const std::shared_ptr<Session>& s) const
  {
    return 512 + s->client.size() * 2 + s->owner.size();
  }
  void index_subscription(const std::shared_ptr<Session>& s, const std::string& filter, bool add)
  {
    size_t node = 0, offset = 0;
    std::vector<std::pair<size_t, std::string>> path;
    for (;;) {
      size_t end = filter.find('/', offset);
      std::string level = filter.substr(offset, end == std::string::npos ? end : end - offset);
      auto found = index[node].children.find(level);
      size_t next;
      if (found == index[node].children.end()) {
        if (!add)
          return;
        if (free_nodes.empty()) {
          next = index.size();
          index.emplace_back();
        } else {
          next = free_nodes.back();
          free_nodes.pop_back();
        }
        index[node].children.emplace(level, next);
      } else
        next = found->second;
      path.emplace_back(node, level);
      node = next;
      if (end == std::string::npos)
        break;
      offset = end + 1;
    }
    if (add) {
      index[node].clients[s->client] = s;
      return;
    }
    index[node].clients.erase(s->client);
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      if (!index[node].clients.empty() || !index[node].children.empty())
        break;
      index[it->first].children.erase(it->second);
      index[node] = TopicNode();
      free_nodes.push_back(node);
      node = it->first;
    }
  }
  void set_subscription(const std::shared_ptr<Session>& s, const std::string& filter, uint8_t qos,
                        bool remove = false)
  {
    auto previous = s->subscriptions.find(filter);
    if (previous != s->subscriptions.end()) {
      if (previous->second) {
        index_subscription(s, filter, false);
        --subscriptions;
      }
      metadata_bytes -= subscription_cost(filter);
      s->subscriptions.erase(previous);
    }
    if (!remove) {
      s->subscriptions[filter] = qos;
      metadata_bytes += subscription_cost(filter);
      if (qos) {
        index_subscription(s, filter, true);
        ++subscriptions;
      }
    }
  }
  void schedule_session(const std::shared_ptr<Session>& s, int64_t deadline = 0)
  {
    if (s->scheduled_expiry)
      session_expiry.erase({s->scheduled_expiry, s->client});
    s->scheduled_expiry = deadline;
    if (deadline)
      session_expiry[{deadline, s->client}] = s;
  }
  void schedule_message(const std::shared_ptr<Message>& m)
  {
    if (m->references && m->committed && m->expires > 0)
      message_expiry[{m->expires, m->id}] = m;
  }
  void rebuild_indexes()
  {
    size_t count = 0;
    metadata_bytes = 0;
    index.clear();
    index.emplace_back();
    free_nodes.clear();
    message_expiry.clear();
    session_expiry.clear();
    for (const auto& s : sessions) {
      metadata_bytes += session_cost(s.second);
      for (const auto& sub : s.second->subscriptions) {
        metadata_bytes += subscription_cost(sub.first);
        if (sub.second) {
          ++count;
          index_subscription(s.second, sub.first, true);
        }
      }
      s.second->scheduled_expiry = 0;
      if (!s.second->online)
        schedule_session(s.second, s.second->deadline);
      for (const auto& delivery : s.second->pending)
        schedule_message(delivery.second.message);
    }
    subscriptions.store(count);
    indexing = true;
  }
  void check_metadata(size_t extra, size_t deliveries, size_t payload = 0)
  {
    extra += metadata_reserved + unique_bytes + payload;
    uint64_t budget = config.max_disk_bytes / 8, overhead = 1024 + config.partitions * 16;
    if (extra > budget || metadata_bytes > budget - extra ||
        metadata_bytes + extra > budget - std::min(budget, overhead) ||
        deliveries > (budget - overhead - metadata_bytes - extra) / 80)
      throw std::runtime_error("persistent metadata/checkpoint capacity exhausted");
    if (metadata_bytes + extra - unique_bytes - payload > config.max_request_bytes * 4ULL)
      throw std::runtime_error("persistent metadata memory limit");
  }
  void reference(const std::shared_ptr<Message>& message, const std::shared_ptr<Session>& s)
  {
    if (!message->references++)
      unique_bytes += message->wire.size();
    message->recipients.push_back(s);
  }
  void drop(const std::shared_ptr<Session>& s, std::map<int64_t, DeliveryState>::iterator entry)
  {
    auto& delivery = entry->second;
    if (!--delivery.message->references) {
      unique_bytes -= delivery.message->wire.size();
      message_expiry.erase({delivery.message->expires, delivery.message->id});
    }
    bytes -= delivery.message->wire.size();
    s->bytes -= delivery.message->wire.size();
    --pending;
    if (delivery.packet) {
      auto p = s->packets.find(delivery.packet);
      if (p != s->packets.end() && p->second == entry->first)
        s->packets.erase(p);
    }
    s->pending.erase(entry);
  }
  void erase_session(const std::shared_ptr<Session>& s)
  {
    notify(s);
    while (!s->pending.empty())
      drop(s, s->pending.begin());
    schedule_session(s);
    if (indexing) {
      while (!s->subscriptions.empty())
        set_subscription(s, s->subscriptions.begin()->first, 0, true);
      metadata_bytes -= session_cost(s);
    }
    sessions.erase(s->client);
  }
  void expire(bool startup = false)
  {
    auto now = DurableStore::now_ms();
    if (!startup) {
      // Bound each heartbeat's work independently of unrelated backlog size.
      size_t budget = 256;
      while (budget && !message_expiry.empty() && message_expiry.begin()->first.first <= now) {
        const auto key = message_expiry.begin()->first;
        auto m = message_expiry.begin()->second.lock();
        if (!m) {
          message_expiry.erase(key);
          --budget;
          continue;
        }
        while (budget && m->expiry_cursor < m->recipients.size()) {
          --budget;
          auto s = m->recipients[m->expiry_cursor++].lock();
          if (!s)
            continue;
          auto entry = s->pending.find(m->id);
          if (entry != s->pending.end() && !entry->second.attempted) {
            drop(s, entry);
            notify(s);
          }
        }
        if (m->expiry_cursor == m->recipients.size())
          message_expiry.erase(key);
      }
      budget = 256;
      while (budget && !session_expiry.empty() && session_expiry.begin()->first.first <= now) {
        auto s = session_expiry.begin()->second.lock();
        session_expiry.erase(session_expiry.begin());
        --budget;
        if (!s)
          continue;
        s->scheduled_expiry = 0;
        if (s->busy) {
          schedule_session(s, now + 100);
          continue;
        }
        while (budget && !s->pending.empty()) {
          drop(s, s->pending.begin());
          --budget;
        }
        if (s->pending.empty())
          erase_session(s);
        else
          schedule_session(s, now + 100);
      }
      return;
    }
    for (auto it = sessions.begin(); it != sessions.end();) {
      auto s = it->second;
      ++it;
      if (startup)
        s->online = false;
      if (!s->busy && !s->online && s->deadline <= now) {
        erase_session(s);
        continue;
      }
      for (auto entry = s->pending.begin(); entry != s->pending.end();) {
        auto old = entry++;
        if (!old->second.attempted && old->second.message->committed &&
            old->second.message->expires > 0 && old->second.message->expires <= now) {
          drop(s, old);
          notify(s);
        }
      }
    }
  }
  uint16_t available_packet(const std::shared_ptr<Session>& s,
                            const std::set<uint16_t>& reserved = {})
  {
    if (s->packets.size() + reserved.size() >= 65535)
      return 0;
    uint16_t candidate = s->next_packet;
    for (size_t i = 0; i < 65535; ++i) {
      if (!s->packets.count(candidate) && !reserved.count(candidate))
        return candidate;
      candidate = uint16_t(candidate % 65535 + 1);
    }
    return 0;
  }
  std::vector<std::shared_ptr<Session>> match(const std::string& topic);
  void replay_control(const std::string& data, bool acknowledgements);
  std::shared_ptr<Message> decode_message(const std::string& data, size_t partition,
                                          const Location& location, bool attach);
  void restore();
  void checkpoint();
  void maintain();
  void heartbeat();
  void validate_state();
};

DurableStore::Impl::Impl(const PersistenceConfig& cfg) : config(cfg)
{
  if (!config.partitions || config.partitions > 32 || config.segment_bytes < 4096 ||
      config.max_disk_bytes / (config.partitions * 4) < config.segment_bytes ||
      config.checkpoint_interval_ms < 100)
    throw std::runtime_error("invalid partition journal configuration");
  lock_fd = open((config.path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    if (lock_fd >= 0)
      close(lock_fd);
    throw std::runtime_error("persistent store missing or owned by another broker");
  }
  try {
    journal::ensure_directory(config.path);
    if (access((config.path + "/IMPORTING").c_str(), F_OK) == 0)
      throw std::runtime_error(
          "incomplete offline import; preserve source and retry into a fresh directory");
    auto format = journal::read_file(config.path + "/FORMAT", 4096);
    if (format.empty()) {
      DIR* directory = opendir(config.path.c_str());
      if (!directory)
        throw std::runtime_error("cannot inspect journal directory");
      bool empty = true;
      while (auto* entry = readdir(directory)) {
        std::string name = entry->d_name;
        if (name != "." && name != ".." && name != "FORMAT.tmp")
          empty = false;
      }
      closedir(directory);
      if (!empty)
        throw std::runtime_error("journal FORMAT missing from nonempty directory");
      Encoder e;
      e.text("MQTTS-PARTITION-LOG");
      e.u32(format_version);
      e.u32(uint32_t(config.partitions));
      journal::atomic_file(config.path + "/FORMAT", checked(e.data));
    } else {
      auto body = unchecked(format);
      Decoder d(body);
      if (d.text() != "MQTTS-PARTITION-LOG")
        throw std::runtime_error("invalid journal format");
      format_version = d.u32();
      if ((format_version != 2 && format_version != 3) || d.u32() != config.partitions)
        throw std::runtime_error(
            "journal format/partition count differs; offline migration required");
      d.end();
    }
    size_t disk_per_log =
        size_t((config.max_disk_bytes - config.max_disk_bytes / 4) / (2 * config.partitions));
    for (size_t p = 0; p < config.partitions; ++p) {
      messages.emplace_back(new AppendLog(config.path + "/messages-" + std::to_string(p),
                                          config.segment_bytes, disk_per_log));
      states.emplace_back(new AppendLog(config.path + "/sessions-" + std::to_string(p),
                                        config.segment_bytes, disk_per_log));
    }
    message_cuts.resize(config.partitions);
    state_cuts.resize(config.partitions);
    restore();
    maintenance = std::thread(&Impl::maintain, this);
  } catch (...) {
    messages.clear();
    states.clear();
    close(lock_fd);
    lock_fd = -1;
    throw;
  }
}
DurableStore::Impl::~Impl()
{
  {
    std::lock_guard<std::mutex> guard(mutex);
    stopping = true;
  }
  stopped.notify_all();
  if (maintenance.joinable())
    maintenance.join();
  messages.clear();
  states.clear();
  if (lock_fd >= 0)
    close(lock_fd);
}
DurableStore::DurableStore(const PersistenceConfig& config) : impl_(new Impl(config)) {}
DurableStore::~DurableStore() = default;
bool DurableStore::has_subscriptions() const
{
  return impl_->subscriptions.load() != 0;
}
uint32_t DurableStore::max_session_expiry() const
{
  return impl_->config.max_session_expiry_seconds;
}
DurableStore::Statistics DurableStore::statistics() const
{
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Statistics s;
  s.sessions = impl_->sessions.size();
  s.pending = impl_->pending;
  s.bytes = impl_->bytes;
  s.unique_bytes = impl_->unique_bytes;
  s.checkpoint_bytes = impl_->checkpoint_bytes.load();
  for (const auto& log : impl_->messages)
    s.message_partition_bytes.push_back(log->disk_bytes());
  for (const auto& log : impl_->states)
    s.session_partition_bytes.push_back(log->disk_bytes());
  s.discarded_denied = impl_->discarded_denied.load();
  s.discarded_oversize = impl_->discarded_oversize.load();
  s.discarded_malformed = impl_->discarded_malformed.load();
  return s;
}
void DurableStore::checkpoint()
{
  impl_->checkpoint();
}

DurableStore::Result DurableStore::connect(const std::string& client, const std::string& owner,
                                           bool clean, uint32_t expiry)
{
  auto& v = *impl_;
  expiry = std::min(expiry, v.config.max_session_expiry_seconds);
  return v.call(
      [&](const std::shared_ptr<Result>& r) -> Ticket {
        auto it = v.sessions.find(client);
        std::shared_ptr<Impl::Session> old = it == v.sessions.end() ? nullptr : it->second;
        if (!old && expiry == 0 && !v.stopping) {
          *r = success();
          return {};
        }
        if (v.failed || v.stopping) {
          r->error = "persistent journal unavailable";
          return {};
        }
        if (v.frozen)
          throw MaintenancePause();
        if (old && old->busy)
          throw Deferred();
        if (old && !old->online && old->deadline <= now_ms()) {
          v.erase_session(old);
          old.reset();
        }
        if (old && old->owner != owner) {
          r->not_authorized = true;
          r->error = "persistent session belongs to another identity";
          return {};
        }
        if (!old && expiry == 0) {
          *r = success();
          return {};
        }
        if (!old && v.sessions.size() >= v.config.max_sessions)
          throw std::runtime_error("persistent session limit");
        if (!old)
          v.check_metadata(512 + client.size() * 2 + owner.size(), v.pending);
        auto s = (old && !clean) ? old : std::make_shared<Impl::Session>();
        uint64_t epoch = ++v.next_id, generation = (old && !clean) ? old->generation : epoch;
        int64_t deadline = now_ms() + int64_t(expiry) * 1000;
        auto signal = std::make_shared<Signal>();
        Encoder e;
        e.u8(CONNECT);
        e.text(client);
        e.text(owner);
        e.u64(generation);
        e.u64(epoch);
        e.u32(expiry);
        e.u64(deadline);
        e.u8(clean);
        auto ticket =
            v.enqueue(v.state_log(client), std::move(e.data), [=, &v](bool ok, const Location&) {
              if (old)
                old->busy = false;
              if (!ok)
                return;
              if (old)
                v.notify(old);
              if (old && clean)
                v.erase_session(old);
              if (clean && expiry == 0) {
                *r = success();
                return;
              }
              s->client = client;
              s->owner = owner;
              s->generation = generation;
              s->epoch = epoch;
              s->expiry = expiry;
              s->deadline = deadline;
              s->online = true;
              v.schedule_session(s);
              s->busy = false;
              s->signal = signal;
              v.sessions[client] = s;
              if (old && clean)
                v.metadata_bytes += v.session_cost(s);
              *r = success();
              r->present = bool(old && !clean);
              r->epoch = epoch;
              r->revision = signal;
              for (const auto& sub : s->subscriptions)
                r->subscriptions.push_back(sub);
              v.notify(s);
            });
        if (old)
          old->busy = true;
        else {
          s->client = client;
          s->owner = owner;
          s->busy = true;
          v.sessions[client] = s;
          v.metadata_bytes += v.session_cost(s);
        }
        return ticket;
      },
      true, true);
}
DurableStore::Result DurableStore::disconnect(const std::string& client, uint64_t epoch,
                                              int64_t expiry_override)
{
  auto& v = *impl_;
  return v.call([&](const std::shared_ptr<Result>& r) -> Ticket {
    auto s = v.current(client, epoch);
    if (!s) {
      *r = stale();
      return {};
    }
    uint32_t expiry =
        expiry_override < 0
            ? s->expiry
            : uint32_t(std::min<int64_t>(expiry_override, v.config.max_session_expiry_seconds));
    int64_t deadline = now_ms() + int64_t(expiry) * 1000;
    Encoder e;
    e.u8(DISCONNECT);
    e.text(client);
    e.u64(s->generation);
    e.u64(epoch);
    e.u32(expiry);
    e.u64(deadline);
    auto ticket =
        v.enqueue(v.state_log(client), std::move(e.data), [=, &v](bool ok, const Location&) {
          s->busy = false;
          if (!ok)
            return;
          s->online = false;
          s->expiry = expiry;
          s->deadline = deadline;
          if (!expiry)
            v.erase_session(s);
          else {
            v.schedule_session(s, deadline);
            v.notify(s);
          }
          *r = success();
        });
    s->busy = true;
    return ticket;
  });
}
DurableStore::Result DurableStore::subscribe(const std::string& client, uint64_t epoch,
                                             const std::string& filter, uint8_t qos)
{
  auto& v = *impl_;
  return v.call([&](const std::shared_ptr<Result>& r) -> Ticket {
    auto s = v.current(client, epoch);
    if (!s) {
      *r = stale();
      return {};
    }
    if (!s->subscriptions.count(filter) &&
        s->subscriptions.size() >= v.config.max_subscriptions_per_session)
      throw std::runtime_error("persistent subscription limit");
    if (!s->subscriptions.count(filter))
      v.check_metadata(subscription_cost(filter), v.pending);
    size_t reservation = s->subscriptions.count(filter) ? 0 : subscription_cost(filter);
    const auto previous = s->subscriptions.find(filter);
    const bool existed = previous != s->subscriptions.end();
    const uint8_t previous_qos = existed ? previous->second : 0;
    Encoder e;
    e.u8(SUBSCRIBE);
    e.text(client);
    e.u64(s->generation);
    e.text(filter);
    e.u8(qos);
    auto ticket =
        v.enqueue(v.state_log(client), std::move(e.data), [=, &v](bool ok, const Location&) {
          v.metadata_reserved -= reservation;
          s->busy = false;
          if (!ok)
            return;
          v.set_subscription(s, filter, qos);
          *r = success();
          r->present = existed;
          if (existed)
            r->subscriptions.emplace_back(filter, previous_qos);
        });
    v.metadata_reserved += reservation;
    s->busy = true;
    return ticket;
  });
}
DurableStore::Result DurableStore::unsubscribe(const std::string& client, uint64_t epoch,
                                               const std::string& filter)
{
  auto& v = *impl_;
  return v.call([&](const std::shared_ptr<Result>& r) -> Ticket {
    auto s = v.current(client, epoch);
    if (!s) {
      *r = stale();
      return {};
    }
    Encoder e;
    e.u8(UNSUBSCRIBE);
    e.text(client);
    e.u64(s->generation);
    e.text(filter);
    auto ticket =
        v.enqueue(v.state_log(client), std::move(e.data), [=, &v](bool ok, const Location&) {
          s->busy = false;
          if (!ok)
            return;
          v.set_subscription(s, filter, 0, true);
          *r = success();
        });
    s->busy = true;
    return ticket;
  });
}

std::vector<std::shared_ptr<DurableStore::Impl::Session>> DurableStore::Impl::match(
    const std::string& topic)
{
  std::set<std::shared_ptr<Session>> targets;
  std::vector<std::pair<size_t, size_t>> todo;
  todo.emplace_back(0, 0);
  while (!todo.empty()) {
    auto item = todo.back();
    todo.pop_back();
    const auto& node = index[item.first];
    bool system = item.first == 0 && !topic.empty() && topic[0] == '$';
    auto hash = node.children.find("#");
    if (!system && hash != node.children.end())
      for (const auto& s : index[hash->second].clients)
        targets.insert(s.second);
    if (item.second > topic.size()) {
      for (const auto& s : node.clients)
        targets.insert(s.second);
      continue;
    }
    size_t end = topic.find('/', item.second),
           next = end == std::string::npos ? topic.size() + 1 : end + 1;
    auto level = topic.substr(item.second, end == std::string::npos ? end : end - item.second);
    auto exact = node.children.find(level);
    if (exact != node.children.end())
      todo.emplace_back(exact->second, next);
    auto plus = node.children.find("+");
    if (!system && plus != node.children.end())
      todo.emplace_back(plus->second, next);
  }
  std::vector<std::shared_ptr<Session>> live;
  for (const auto& s : targets) {
    if (s->busy)
      throw Deferred();
    if (s->online || s->deadline > DurableStore::now_ms())
      live.push_back(s);
  }
  return live;
}
DurableStore::Result DurableStore::publish(const std::string& topic, const std::string& wire,
                                           const std::string& sender, int64_t expires)
{
  auto& v = *impl_;
  return v.call([&](const std::shared_ptr<Result>& r) -> Ticket {
    *r = success();
    if (expires > 0 && expires <= now_ms())
      return {};
    auto targets = v.match(topic);
    targets.erase(std::remove_if(
                      targets.begin(), targets.end(),
                      [&](const std::shared_ptr<Impl::Session>& s) { return s->client == sender; }),
                  targets.end());
    if (targets.empty())
      return {};
    v.check_metadata(0, v.pending + targets.size(), wire.size());
    if (targets.size() > v.config.max_messages ||
        v.pending > v.config.max_messages - targets.size() ||
        wire.size() > v.config.max_bytes / targets.size() ||
        v.bytes > v.config.max_bytes - wire.size() * targets.size())
      throw std::runtime_error("persistent message capacity exhausted");
    for (const auto& s : targets)
      if (s->pending.size() >= v.config.max_messages_per_session)
        throw std::runtime_error("persistent client backlog exhausted");
    auto message = std::make_shared<Impl::Message>();
    message->id = ++v.next_id;
    message->expires = expires;
    message->partition = journal::topic_hash(topic) % v.messages.size();
    message->wire = wire;
    Encoder e;
    e.u8(MESSAGE);
    e.u64(message->id);
    e.u64(expires);
    e.text(wire);
    e.u32(uint32_t(targets.size()));
    std::vector<uint16_t> packets;
    for (const auto& s : targets) {
      uint16_t packet = s->pending.size() < 65535 ? v.available_packet(s) : 0;
      packets.push_back(packet);
      e.text(s->client);
      e.u64(s->generation);
      e.u32(packet);
      r->targets.push_back(s->client);
    }
    // The live router uses binary_search to exclude recipients already queued
    // here. Topic-index session pointers are not ordered by Client ID.
    std::sort(r->targets.begin(), r->targets.end());
    auto ticket = v.enqueue(*v.messages[message->partition], std::move(e.data),
                            [=, &v](bool ok, const Location& location) {
                              if (!ok)
                                return;
                              message->location = location;
                              message->committed = true;
                              v.schedule_message(message);
                              for (const auto& s : targets)
                                v.notify(s);
                            });
    for (size_t i = 0; i < targets.size(); ++i) {
      auto s = targets[i];
      Impl::DeliveryState entry;
      entry.message = message;
      entry.packet = packets[i];
      s->pending.emplace(message->id, entry);
      v.reference(message, s);
      if (entry.packet) {
        s->packets[entry.packet] = message->id;
        s->next_packet = uint16_t(entry.packet % 65535 + 1);
      }
      s->bytes += wire.size();
      v.bytes += wire.size();
      ++v.pending;
    }
    return ticket;
  });
}
DurableStore::Result DurableStore::fetch(const std::string& client, uint64_t epoch, int64_t after,
                                         uint16_t receive_maximum)
{
  auto& v = *impl_;
  return v.call([&](const std::shared_ptr<Result>& r) -> Ticket {
    auto s = v.current(client, epoch);
    if (!s) {
      *r = stale();
      return {};
    }
    *r = success();
    size_t inflight = 0, returned_bytes = 0;
    auto reservation = std::make_shared<DeliveryReservation>(v.delivery_bytes);
    r->reservation = reservation;
    for (auto it = s->pending.begin(); it != s->pending.end() && it->first <= after; ++it)
      ++inflight;
    size_t limit =
        std::min<size_t>(v.config.max_inflight, receive_maximum ? receive_maximum : 65535);
    std::vector<std::pair<int64_t, uint16_t>> claims;
    std::set<uint16_t> reserved;
    for (auto it = s->pending.upper_bound(after);
         it != s->pending.end() && r->deliveries.size() < 32 && inflight < limit;) {
      auto current = it++;
      auto& entry = current->second;
      auto message = entry.message;
      if (!message->committed || entry.claim_pending)
        break;
      if (!entry.attempted && message->expires > 0 && message->expires <= now_ms()) {
        v.drop(s, current);
        continue;
      }
      size_t budget = v.config.max_request_bytes / 2;
      if (message->wire.size() > budget ||
          v.delivery_bytes->load() > budget - message->wire.size() ||
          v.request_bytes >
              v.config.max_request_bytes - message->wire.size() - v.delivery_bytes->load()) {
        if (r->deliveries.empty()) {
          r->ok = false;
          r->error = "persistent delivery buffer full";
        }
        break;
      }
      v.delivery_bytes->fetch_add(message->wire.size());
      reservation->bytes += message->wire.size();
      Delivery delivery;
      delivery.sequence = message->id;
      delivery.expires = message->expires;
      delivery.wire = message->wire;
      delivery.packet_id = entry.packet;
      delivery.dup = entry.started;
      if (!delivery.packet_id) {
        delivery.packet_id = v.available_packet(s, reserved);
        if (!delivery.packet_id)
          break;
        reserved.insert(delivery.packet_id);
        claims.emplace_back(message->id, delivery.packet_id);
      }
      r->deliveries.push_back(std::move(delivery));
      ++inflight;
      returned_bytes += message->wire.size();
      if (returned_bytes >= 256 * 1024)
        break;
    }
    if (claims.empty()) {
      for (const auto& delivery : r->deliveries)
        s->pending.at(delivery.sequence).attempted = true;
      return {};
    }
    Encoder e;
    e.u8(CLAIM);
    e.text(client);
    e.u64(s->generation);
    e.u32(uint32_t(claims.size()));
    for (const auto& claim : claims) {
      e.u64(claim.first);
      e.u32(claim.second);
    }
    auto ticket =
        v.enqueue(v.state_log(client), std::move(e.data), [=, &v](bool ok, const Location&) {
          if (!ok)
            return;
          for (const auto& claim : claims) {
            auto found = s->pending.find(claim.first);
            if (found == s->pending.end())
              continue;
            found->second.packet = claim.second;
            found->second.claim_pending = false;
          }
          for (const auto& delivery : r->deliveries) {
            auto found = s->pending.find(delivery.sequence);
            if (found != s->pending.end())
              found->second.attempted = true;
          }
          if (s->epoch != epoch)
            *r = stale();
        });
    for (const auto& claim : claims) {
      s->packets[claim.second] = claim.first;
      s->pending.at(claim.first).claim_pending = true;
      s->next_packet = uint16_t(claim.second % 65535 + 1);
    }
    return ticket;
  });
}
DurableStore::Result DurableStore::begin_delivery(const std::string& client, uint64_t epoch,
                                                  int64_t sequence, uint16_t packet_id)
{
  auto& v = *impl_;
  return v.call([&](const std::shared_ptr<Result>& r) -> Ticket {
    auto s = v.current(client, epoch);
    if (!s) {
      *r = stale();
      return {};
    }
    auto it = s->pending.find(sequence);
    if (it == s->pending.end() || it->second.packet != packet_id || it->second.ack_pending) {
      *r = stale();
      return {};
    }
    it->second.started = true;
    *r = success();
    return {};
  });
}

DurableStore::Result DurableStore::acknowledge(const std::string& client, uint64_t epoch,
                                               uint16_t packet_id, bool wait_for_commit)
{
  auto& v = *impl_;
  return v.call(
      [&](const std::shared_ptr<Result>& r) -> Ticket {
        auto s = v.current(client, epoch);
        if (!s) {
          *r = stale();
          return {};
        }
        *r = success();
        auto p = s->packets.find(packet_id);
        if (p == s->packets.end())
          return {};
        auto found = s->pending.find(p->second);
        if (found == s->pending.end() || !found->second.attempted)
          return {};
        r->present = true;
        if (found->second.ack_pending) {
          if (wait_for_commit)
            throw Deferred();
          return {};
        }
        int64_t id = found->first;
        Encoder e;
        e.u8(ACK);
        e.text(client);
        e.u64(s->generation);
        e.u64(id);
        e.u32(packet_id);
        auto ticket = v.enqueue(
            v.state_log(client), std::move(e.data),
            [=, &v](bool ok, const Location&) {
              if (!ok)
                return;
              auto item = s->pending.find(id);
              if (item != s->pending.end())
                v.drop(s, item);
              v.notify(s);
            },
            wait_for_commit);
        found->second.ack_pending = true;
        return ticket;
      },
      wait_for_commit);
}

DurableStore::Result DurableStore::discard(const std::string& client, uint64_t epoch,
                                           uint16_t packet_id, DiscardReason reason)
{
  // The exact delivery's removal is durable before the pump advances. Never
  // count a failed disk write or an already removed packet as a discard.
  auto result = acknowledge(client, epoch, packet_id);
  if (result.ok && result.present) {
    const char* label = "malformed";
    if (reason == DiscardReason::NotAuthorized) {
      ++impl_->discarded_denied;
      label = "not_authorized";
    } else if (reason == DiscardReason::PacketTooLarge) {
      ++impl_->discarded_oversize;
      label = "packet_too_large";
    } else {
      ++impl_->discarded_malformed;
    }
    LOG_WARN("Durable delivery discarded: client {}, epoch {}, packet {}, reason {}", client, epoch,
             packet_id, label);
  }
  return result;
}

void DurableStore::Impl::replay_control(const std::string& data, bool acknowledgements)
{
  Decoder d(data);
  uint8_t kind = d.u8();
  if (kind < CONNECT || kind > HEARTBEAT)
    throw std::runtime_error("invalid session journal record");
  if (acknowledgements != (kind == ACK || kind == CLAIM))
    return;
  if (kind == HEARTBEAT) {
    uint32_t count = d.u32();
    for (uint32_t i = 0; i < count; ++i) {
      auto name = d.text();
      auto generation = d.u64(), epoch = d.u64();
      int64_t deadline = d.u64();
      auto found = sessions.find(name);
      if (found != sessions.end() && found->second->generation == generation &&
          found->second->epoch == epoch && found->second->online)
        found->second->deadline = deadline;
    }
    d.end();
    return;
  }
  auto name = d.text();
  if (kind == CONNECT) {
    auto owner = d.text();
    auto generation = d.u64(), epoch = d.u64();
    auto expiry = d.u32();
    int64_t deadline = d.u64();
    bool clean = d.u8();
    d.end();
    if (!generation || !epoch)
      throw std::runtime_error("invalid session generation");
    next_id = std::max(next_id, std::max(generation, epoch));
    auto found = sessions.find(name);
    std::shared_ptr<Session> s = found == sessions.end() ? nullptr : found->second;
    if (s && (clean || s->generation != generation)) {
      erase_session(s);
      s.reset();
    }
    if (clean && !expiry)
      return;
    if (!s)
      s = std::make_shared<Session>();
    s->client = name;
    s->owner = owner;
    s->generation = generation;
    s->epoch = epoch;
    s->expiry = expiry;
    s->deadline = deadline;
    s->online = true;
    sessions[name] = s;
    return;
  }
  auto generation = d.u64();
  auto found = sessions.find(name);
  auto s = (found != sessions.end() && found->second->generation == generation) ? found->second
                                                                                : nullptr;
  if (kind == DISCONNECT) {
    auto epoch = d.u64();
    auto expiry = d.u32();
    int64_t deadline = d.u64();
    if (s && s->epoch == epoch) {
      s->expiry = expiry;
      s->deadline = deadline;
      s->online = false;
      if (!expiry)
        erase_session(s);
    }
  } else if (kind == SUBSCRIBE) {
    auto filter = d.text();
    auto qos = d.u8();
    if (qos > 2)
      throw std::runtime_error("invalid stored subscription QoS");
    if (s)
      s->subscriptions[filter] = qos;
  } else if (kind == UNSUBSCRIBE) {
    auto filter = d.text();
    if (s)
      s->subscriptions.erase(filter);
  } else if (kind == ACK) {
    int64_t id = d.u64();
    uint32_t packet = d.u32();
    if (!packet || packet > 65535)
      throw std::runtime_error("invalid stored ACK packet");
    if (s) {
      auto entry = s->pending.find(id);
      if (entry != s->pending.end()) {
        if (entry->second.packet != packet)
          throw std::runtime_error("stored ACK refers to a different packet");
        drop(s, entry);
      } else
        s->recovery_acks.push_back(id);
      s->recovery_claims.erase(id);
    }
  } else if (kind == CLAIM) {
    uint32_t count = d.u32();
    for (uint32_t i = 0; i < count; ++i) {
      int64_t id = d.u64();
      uint32_t packet = d.u32();
      if (!packet || packet > 65535)
        throw std::runtime_error("invalid stored packet claim");
      if (s) {
        auto entry = s->pending.find(id);
        if (entry != s->pending.end())
          entry->second.packet = uint16_t(packet);
        else
          s->recovery_claims[id] = uint16_t(packet);
      }
    }
  }
  d.end();
}
std::shared_ptr<DurableStore::Impl::Message> DurableStore::Impl::decode_message(
    const std::string& data, size_t partition, const Location& location, bool attach)
{
  Decoder d(data);
  if (d.u8() != MESSAGE)
    throw std::runtime_error("invalid message journal record");
  auto m = std::make_shared<Message>();
  m->id = d.u64();
  m->expires = d.u64();
  m->wire = d.text();
  m->partition = partition;
  m->location = location;
  m->committed = true;
  if (m->id <= 0)
    throw std::runtime_error("invalid stored message sequence");
  next_id = std::max(next_id, uint64_t(m->id));
  uint32_t count = d.u32();
  for (uint32_t i = 0; i < count; ++i) {
    auto name = d.text();
    auto generation = d.u64();
    uint32_t packet = d.u32();
    if (packet > 65535)
      throw std::runtime_error("invalid stored packet identifier");
    auto found = sessions.find(name);
    if (!attach || found == sessions.end() || found->second->generation != generation)
      continue;
    auto s = found->second;
    if (std::binary_search(s->recovery_acks.begin(), s->recovery_acks.end(), m->id))
      continue;
    auto claim = s->recovery_claims.find(m->id);
    if (claim != s->recovery_claims.end())
      packet = claim->second;
    if (!packet && m->expires > 0 && m->expires <= DurableStore::now_ms())
      continue;
    DeliveryState entry;
    entry.message = m;
    entry.packet = uint16_t(packet);
    entry.attempted = entry.started = packet != 0;
    if (!s->pending.emplace(m->id, entry).second)
      throw std::runtime_error("duplicate journal message identity");
    reference(m, s);
    ++pending;
    bytes += m->wire.size();
    s->bytes += m->wire.size();
  }
  d.end();
  return m;
}
void DurableStore::Impl::restore()
{
  auto file = journal::read_file(config.path + "/CHECKPOINT", size_t(config.max_disk_bytes / 8));
  if (!file.empty()) {
    auto body = unchecked(file);
    Decoder d(body);
    const auto magic = d.text();
    const bool embedded = magic == "MQTTS-CHECKPOINT-3";
    if ((!embedded && magic != "MQTTS-CHECKPOINT-2") || (embedded && format_version < 3) ||
        d.u32() != config.partitions)
      throw std::runtime_error("checkpoint format/partition mismatch");
    checkpoint_bytes = file.size();
    next_id = d.u64();
    for (size_t p = 0; p < config.partitions; ++p) {
      message_cuts[p] = d.u64();
      state_cuts[p] = d.u64();
    }
    std::map<int64_t, std::shared_ptr<Message>> referenced;
    uint32_t count = d.u32();
    if (count > config.max_messages)
      throw std::runtime_error("checkpoint exceeds configured message limit");
    for (uint32_t i = 0; i < count; ++i) {
      int64_t id = d.u64();
      size_t partition = d.u32();
      if (partition >= messages.size() || id <= 0 || uint64_t(id) > next_id)
        throw std::runtime_error("invalid checkpoint partition/cut");
      std::shared_ptr<Message> m;
      if (embedded) {
        m = std::make_shared<Message>();
        m->id = id;
        m->partition = partition;
        m->expires = d.u64();
        m->wire = d.text();
        m->committed = true;
      } else {
        auto location = location_read(d);
        if (location.serial > message_cuts[partition])
          throw std::runtime_error("invalid checkpoint message cut");
        m = decode_message(messages[partition]->read(location), partition, location, false);
      }
      if (m->id != id || !referenced.emplace(id, m).second)
        throw std::runtime_error("checkpoint message identity mismatch");
    }
    count = d.u32();
    if (count > config.max_sessions)
      throw std::runtime_error("checkpoint exceeds configured session limit");
    for (uint32_t i = 0; i < count; ++i) {
      auto s = std::make_shared<Session>();
      s->client = d.text();
      s->owner = d.text();
      s->generation = d.u64();
      s->epoch = d.u64();
      s->expiry = d.u32();
      s->deadline = d.u64();
      s->online = d.u8();
      uint32_t next = d.u32();
      if (!next || next > 65535)
        throw std::runtime_error("invalid checkpoint packet cursor");
      s->next_packet = uint16_t(next);
      uint32_t subs = d.u32();
      if (subs > config.max_subscriptions_per_session)
        throw std::runtime_error("checkpoint subscription limit");
      for (uint32_t j = 0; j < subs; ++j) {
        auto filter = d.text();
        auto qos = d.u8();
        s->subscriptions[filter] = qos;
      }
      uint32_t entries = d.u32();
      if (entries > config.max_messages_per_session)
        throw std::runtime_error("checkpoint client backlog limit");
      for (uint32_t j = 0; j < entries; ++j) {
        int64_t id = d.u64();
        uint32_t packet = d.u32();
        auto message = referenced.find(id);
        if (message == referenced.end() || packet > 65535)
          throw std::runtime_error("invalid checkpoint delivery");
        DeliveryState entry;
        entry.message = message->second;
        entry.packet = uint16_t(packet);
        entry.attempted = entry.started = packet != 0;
        if (!s->pending.emplace(id, entry).second)
          throw std::runtime_error("duplicate checkpoint delivery");
        reference(entry.message, s);
        ++pending;
        bytes += entry.message->wire.size();
        s->bytes += entry.message->wire.size();
      }
      if (!sessions.emplace(s->client, s).second)
        throw std::runtime_error("duplicate checkpoint session");
    }
    d.end();
  }
  // Controls establish the final generation for each client. Data records carry
  // their original recipient generations; an old Clean Start can never resurrect.
  for (size_t p = 0; p < states.size(); ++p)
    states[p]->replay(state_cuts[p], [&](const Location&, const std::string& record) {
      replay_control(record, false);
    });
  expire(true);
  // Read compact ACK identities first. Replaying payloads before ACKs would
  // temporarily materialize an entire checkpoint interval of already-consumed
  // traffic in memory. Exact identities preserve holes without keeping payloads.
  for (size_t p = 0; p < states.size(); ++p)
    states[p]->replay(state_cuts[p], [&](const Location&, const std::string& record) {
      replay_control(record, true);
    });
  for (const auto& item : sessions)
    std::sort(item.second->recovery_acks.begin(), item.second->recovery_acks.end());
  for (size_t p = 0; p < messages.size(); ++p)
    messages[p]->replay(message_cuts[p],
                        [&, p](const Location& location, const std::string& record) {
                          decode_message(record, p, location, true);
                        });
  for (const auto& item : sessions) {
    std::vector<int64_t>().swap(item.second->recovery_acks);
    item.second->recovery_claims.clear();
  }
  expire(true);
  rebuild_indexes();
  validate_state();
}
void DurableStore::Impl::validate_state()
{
  check_metadata(0, pending);
  if (sessions.size() > config.max_sessions || pending > config.max_messages ||
      bytes > config.max_bytes)
    throw std::runtime_error(
        "recovered journal exceeds configured capacity; raise limits before opening");
  for (const auto& item : sessions) {
    auto s = item.second;
    if (!s->generation || !s->epoch ||
        s->subscriptions.size() > config.max_subscriptions_per_session ||
        s->pending.size() > config.max_messages_per_session)
      throw std::runtime_error("invalid recovered session or configured capacity");
    s->packets.clear();
    for (const auto& delivery : s->pending)
      if (delivery.second.packet &&
          !s->packets.emplace(delivery.second.packet, delivery.first).second)
        throw std::runtime_error("duplicate live packet identifier in recovered session");
  }
}
void DurableStore::Impl::checkpoint()
{
  std::lock_guard<std::mutex> single(checkpoint_mutex);
  {
    std::lock_guard<std::mutex> guard(mutex);
    if (failed)
      throw std::runtime_error("failed journal cannot checkpoint");
    frozen = true;
  }
  try {
    std::vector<Ticket> fences;
    for (auto& log : messages)
      fences.push_back(log->seal());
    for (auto& log : states)
      fences.push_back(log->seal());
    for (const auto& fence : fences)
      if (!fence->wait())
        throw std::runtime_error(fence->error);
    struct SnapshotSession
    {
      std::string client, owner;
      uint64_t generation, epoch;
      uint32_t expiry;
      int64_t deadline;
      bool online;
      uint16_t next_packet;
      std::map<std::string, uint8_t> subscriptions;
      std::vector<std::pair<std::shared_ptr<Message>, uint16_t>> pending;
    };
    uint64_t snapshot_id;
    std::vector<SnapshotSession> snapshot;
    std::vector<uint64_t> message_fences(messages.size()), state_fences(states.size());
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (failed || requests)
        throw std::runtime_error("checkpoint fence failed");
      snapshot_id = next_id;
      for (size_t p = 0; p < config.partitions; ++p) {
        message_fences[p] = messages[p]->serial();
        state_fences[p] = states[p]->serial();
      }
      snapshot.reserve(sessions.size());
      for (const auto& item : sessions) {
        const auto& s = *item.second;
        SnapshotSession copy{s.client,   s.owner,  s.generation,  s.epoch,         s.expiry,
                             s.deadline, s.online, s.next_packet, s.subscriptions, {}};
        copy.pending.reserve(s.pending.size());
        for (const auto& entry : s.pending)
          copy.pending.emplace_back(entry.second.message, entry.second.packet);
        snapshot.push_back(std::move(copy));
      }
      // Writers can continue into new segments. Snapshot references pin the
      // sealed prefix until its checkpoint is durably installed.
      frozen = false;
    }
    Encoder e;
    e.text("MQTTS-CHECKPOINT-3");
    e.u32(uint32_t(config.partitions));
    e.u64(snapshot_id);
    for (size_t p = 0; p < config.partitions; ++p) {
      e.u64(message_fences[p]);
      e.u64(state_fences[p]);
    }
    std::map<int64_t, std::shared_ptr<Message>> live;
    for (const auto& s : snapshot)
      for (const auto& delivery : s.pending)
        live.emplace(delivery.first->id, delivery.first);
    e.u32(uint32_t(live.size()));
    for (const auto& item : live) {
      const auto& m = *item.second;
      if (!m.committed)
        throw std::runtime_error("uncommitted checkpoint message");
      e.u64(m.id);
      e.u32(uint32_t(m.partition));
      e.u64(m.expires);
      e.text(m.wire);
    }
    e.u32(uint32_t(snapshot.size()));
    for (const auto& s : snapshot) {
      e.text(s.client);
      e.text(s.owner);
      e.u64(s.generation);
      e.u64(s.epoch);
      e.u32(s.expiry);
      e.u64(s.deadline);
      e.u8(s.online);
      e.u32(s.next_packet);
      e.u32(uint32_t(s.subscriptions.size()));
      for (const auto& sub : s.subscriptions) {
        e.text(sub.first);
        e.u8(sub.second);
      }
      e.u32(uint32_t(s.pending.size()));
      for (const auto& entry : s.pending) {
        e.u64(entry.first->id);
        e.u32(entry.second);
      }
    }
    if (e.data.size() + 4 > config.max_disk_bytes / 8)
      throw std::runtime_error("checkpoint exceeds reserved disk budget");
    if (format_version < 3) {
      // Upgrade the marker first: old readers must refuse a compacted store.
      // New readers accept a v3 marker with either checkpoint generation after
      // a crash between the two atomic replacements.
      Encoder marker;
      marker.text("MQTTS-PARTITION-LOG");
      marker.u32(3);
      marker.u32(uint32_t(config.partitions));
      journal::atomic_file(config.path + "/FORMAT", checked(marker.data));
      format_version = 3;
    }
    journal::atomic_file(config.path + "/CHECKPOINT", checked(e.data));
    checkpoint_bytes = e.data.size() + 4;
    message_cuts = message_fences;
    state_cuts = state_fences;
    // Reclamation runs on each owning writer, serialized with new appends.
    for (size_t p = 0; p < config.partitions; ++p) {
      const size_t before = messages[p]->disk_bytes() + states[p]->disk_bytes();
      messages[p]->prune(message_fences[p], {});
      states[p]->prune(state_fences[p], {});
      LOG_INFO(
          "Durable checkpoint partition {}: before_bytes {}, message_bytes {}, "
          "session_bytes {}, checkpoint_bytes {}",
          p, before, messages[p]->disk_bytes(), states[p]->disk_bytes(), checkpoint_bytes.load());
    }
  } catch (...) {
    std::lock_guard<std::mutex> guard(mutex);
    frozen = false;
    // A failed checkpoint leaves the old checkpoint and its logs usable.
    // Genuine append/flush failures are already fenced by enqueue().
    throw;
  }
}
void DurableStore::Impl::heartbeat()
{
  std::lock_guard<std::mutex> guard(mutex);
  if (frozen || failed || stopping)
    return;
  expire();
  std::vector<std::vector<std::shared_ptr<Session>>> shards(states.size());
  for (const auto& item : sessions)
    if (item.second->online && !item.second->busy)
      shards[journal::topic_hash(item.first) % states.size()].push_back(item.second);
  for (size_t p = 0; p < states.size(); ++p) {
    const auto& online = shards[p];
    for (size_t offset = 0; offset < online.size();) {
      std::vector<std::shared_ptr<Session>> live;
      size_t record_bytes = 5;
      while (offset < online.size() && live.size() < 64) {
        size_t bytes = online[offset]->client.size() + 28;
        if (record_bytes + bytes + 256 > config.max_request_bytes)
          break;
        live.push_back(online[offset++]);
        record_bytes += bytes;
      }
      if (live.empty())
        throw std::runtime_error("session identity exceeds heartbeat admission limit");
      Encoder e;
      e.u8(HEARTBEAT);
      e.u32(uint32_t(live.size()));
      std::vector<uint64_t> epochs;
      std::vector<int64_t> deadlines;
      for (const auto& s : live) {
        epochs.push_back(s->epoch);
        int64_t deadline = DurableStore::now_ms() + int64_t(s->expiry) * 1000;
        deadlines.push_back(deadline);
        e.text(s->client);
        e.u64(s->generation);
        e.u64(s->epoch);
        e.u64(deadline);
      }
      try {
        enqueue(
            *states[p], std::move(e.data),
            [=](bool ok, const Location&) {
              if (ok)
                for (size_t i = 0; i < live.size(); ++i)
                  if (live[i]->epoch == epochs[i] && live[i]->online)
                    live[i]->deadline = deadlines[i];
            },
            false);
      } catch (const Deferred&) {
        return;
      } catch (const journal::AdmissionFull&) {
        break;
      }
    }
  }
}
void DurableStore::Impl::maintain()
{
  auto last_heartbeat = std::chrono::steady_clock::now(), last_checkpoint = last_heartbeat;
  auto retry_after = last_heartbeat;
  unsigned retry_ms = 1000;
  for (;;) {
    {
      std::unique_lock<std::mutex> guard(mutex);
      if (stopped.wait_for(guard, std::chrono::milliseconds(100), [&] { return stopping; }))
        return;
    }
    try {
      auto now = std::chrono::steady_clock::now();
      if (now - last_heartbeat >= std::chrono::seconds(1)) {
        heartbeat();
        last_heartbeat = now;
      }
      bool pressure = false;
      for (const auto& log : messages)
        pressure = pressure || log->needs_checkpoint();
      for (const auto& log : states)
        pressure = pressure || log->needs_checkpoint();
      if (now >= retry_after &&
          (now - last_checkpoint >= std::chrono::milliseconds(config.checkpoint_interval_ms) ||
           (pressure && now - last_checkpoint >= std::chrono::seconds(1)))) {
        checkpoint();
        last_checkpoint = now;
        retry_ms = 1000;
      }
    } catch (const std::exception& e) {
      LOG_ERROR("Persistent journal maintenance will retry: {}", e.what());
      retry_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(retry_ms);
      retry_ms = std::min(60000U, retry_ms * 2);
    }
  }
}
void DurableStore::import_legacy(const std::string& exported)
{
  auto& v = *impl_;
  std::unique_lock<std::mutex> exclusive(v.checkpoint_mutex);
  {
    std::lock_guard<std::mutex> guard(v.mutex);
    if (v.next_id || !v.sessions.empty() || v.requests)
      throw std::runtime_error("offline import requires a new empty store");
    v.frozen = true;
    v.indexing = false;
  }
  journal::atomic_file(v.config.path + "/IMPORTING", "offline migration in progress\n");
  try {
    auto body = unchecked(exported);
    Decoder d(body);
    if (d.text() != "MQTTS-SQLITE-EXPORT-1")
      throw std::runtime_error("unsupported offline export");
    v.next_id = d.u64();
    uint32_t count = d.u32();
    if (count > v.config.max_sessions)
      throw std::runtime_error("source exceeds import session limit");
    for (uint32_t i = 0; i < count; ++i) {
      auto s = std::make_shared<Impl::Session>();
      s->client = d.text();
      s->owner = d.text();
      s->generation = d.u64();
      s->epoch = s->generation;
      s->expiry = d.u32();
      s->deadline = d.u64();
      s->online = false;
      uint32_t next = d.u32();
      if (!next || next > 65535)
        throw std::runtime_error("invalid legacy packet cursor");
      s->next_packet = uint16_t(next);
      uint32_t subs = d.u32();
      if (subs > v.config.max_subscriptions_per_session)
        throw std::runtime_error("source exceeds import subscription limit");
      for (uint32_t j = 0; j < subs; ++j) {
        auto topic = d.text();
        auto qos = d.u8();
        s->subscriptions[topic] = qos;
      }
      if (!v.sessions.emplace(s->client, s).second)
        throw std::runtime_error("duplicate legacy session");
      v.next_id = std::max(v.next_id, s->generation);
    }
    count = d.u32();
    if (count > v.config.max_messages)
      throw std::runtime_error("source exceeds import message limit");
    std::vector<Ticket> jobs;
    size_t job_bytes = 0;
    for (uint32_t i = 0; i < count; ++i) {
      int64_t id = d.u64(), expires = d.u64();
      auto topic = d.text(), wire = d.text();
      uint32_t targets = d.u32();
      Encoder e;
      e.u8(MESSAGE);
      e.u64(id);
      e.u64(expires);
      e.text(wire);
      e.u32(targets);
      for (uint32_t j = 0; j < targets; ++j) {
        auto client = d.text();
        auto generation = d.u64();
        auto packet = d.u32();
        e.text(client);
        e.u64(generation);
        e.u32(packet);
      }
      if (e.data.size() + 256 > v.config.max_request_bytes)
        throw std::runtime_error("legacy message exceeds import request limit");
      if (jobs.size() >= std::min<size_t>(64, v.config.max_requests) ||
          job_bytes + e.data.size() + 256 > v.config.max_request_bytes) {
        for (const auto& job : jobs)
          if (!job->wait())
            throw std::runtime_error(job->error);
        jobs.clear();
        job_bytes = 0;
      }
      size_t partition = journal::topic_hash(topic) % v.messages.size();
      auto m = v.decode_message(e.data, partition, Location(), true);
      m->committed = false;
      if (v.pending > v.config.max_messages || v.bytes > v.config.max_bytes)
        throw std::runtime_error("legacy backlog exceeds import capacity");
      job_bytes += e.data.size() + 256;
      jobs.push_back(
          v.messages[partition]->append(std::move(e.data), [m](bool ok, const Location& location) {
            if (ok) {
              m->location = location;
              m->committed = true;
            }
          }));
    }
    d.end();
    for (const auto& job : jobs)
      if (!job->wait())
        throw std::runtime_error(job->error);
    v.expire(true);
    v.rebuild_indexes();
    v.validate_state();
    exclusive.unlock();
    v.checkpoint();
    if (unlink((v.config.path + "/IMPORTING").c_str()) != 0)
      throw std::runtime_error("cannot finish import marker");
    int directory = open(v.config.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
      throw std::runtime_error("cannot open completed import directory");
    int result = fsync(directory);
    close(directory);
    if (result != 0)
      throw std::runtime_error("cannot sync completed import directory");
  } catch (...) {
    std::lock_guard<std::mutex> guard(v.mutex);
    v.fail();
    throw;
  }
}
}  // namespace mqtt
