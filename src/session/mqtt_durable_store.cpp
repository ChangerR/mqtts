#include "mqtt_durable_store.h"
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include "logger.h"
#include "mqtt_runtime.h"

namespace mqtt {
namespace {
void sql(sqlite3* db, const char* text)
{
  if (sqlite3_exec(db, text, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
}
class Statement
{
 public:
  sqlite3_stmt* s = nullptr;
  explicit Statement(sqlite3* db, const char* text)
  {
    if (sqlite3_prepare_v2(db, text, -1, &s, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db));
  }
  ~Statement() { sqlite3_finalize(s); }
  Statement(const Statement&) = delete;
  void bind(int i, const std::string& value)
  {
    if (sqlite3_bind_text(s, i, value.data(), value.size(), SQLITE_TRANSIENT) != SQLITE_OK)
      throw std::runtime_error("bind text failed");
  }
  void bind(int i, int64_t value)
  {
    if (sqlite3_bind_int64(s, i, value) != SQLITE_OK)
      throw std::runtime_error("bind integer failed");
  }
  void blob(int i, const std::string& value)
  {
    if (sqlite3_bind_blob(s, i, value.data(), value.size(), SQLITE_TRANSIENT) != SQLITE_OK)
      throw std::runtime_error("bind blob failed");
  }
  bool step()
  {
    int rc = sqlite3_step(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
      throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(s)));
    return rc == SQLITE_ROW;
  }
  int64_t integer(int i) const { return sqlite3_column_int64(s, i); }
  std::string text(int i) const
  {
    const auto* p = sqlite3_column_blob(s, i);
    int n = sqlite3_column_bytes(s, i);
    return p ? std::string(static_cast<const char*>(p), n) : std::string();
  }
};
bool current(sqlite3* db, const std::string& client, uint64_t epoch)
{
  Statement s(db, "SELECT 1 FROM sessions WHERE client=? AND epoch=? AND online=1");
  s.bind(1, client);
  s.bind(2, int64_t(epoch));
  return s.step();
}
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
}  // namespace
struct DurableStore::Request
{
  int fd;
  std::string failure_client;
  uint64_t failure_epoch = 0;
  size_t bytes;
  bool bulk = false;
  std::function<Result(sqlite3*)> fn;
  Result result;
  std::atomic<bool> done{false};
  Request(size_t size, std::function<Result(sqlite3*)> call, bool wait)
      : fd(wait ? eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) : -1), bytes(size), fn(std::move(call))
  {
  }
  ~Request()
  {
    if (fd >= 0)
      close(fd);
  }
};
int64_t DurableStore::now_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

DurableStore::DurableStore(const PersistenceConfig& config) : config_(config)
{
  try {
    lock_fd_ = open((config_.path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd_ < 0 || flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
      throw std::runtime_error("persistent store path missing or already owned by another broker");
    int private_file = open(config_.path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (private_file < 0)
      throw std::runtime_error("cannot create persistent store");
    fchmod(private_file, 0600);
    close(private_file);
    if (sqlite3_open_v2(config_.path.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK)
      throw std::runtime_error("cannot open persistent store");
    sqlite3_busy_timeout(db_, 100);
    sql(db_,
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; PRAGMA "
        "cache_size=-8192;");
    {
      Statement s(db_, "PRAGMA user_version");
      s.step();
      if (s.integer(0) > 1)
        throw std::runtime_error("unsupported persistent store version");
    }
    sql(db_, R"SQL(
      CREATE TABLE IF NOT EXISTS sessions(client TEXT PRIMARY KEY, owner TEXT NOT NULL, epoch INTEGER NOT NULL,
        expiry INTEGER NOT NULL, deadline INTEGER NOT NULL, online INTEGER NOT NULL, next_packet INTEGER NOT NULL DEFAULT 1,
        pending INTEGER NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0);
      CREATE TABLE IF NOT EXISTS subscriptions(client TEXT NOT NULL REFERENCES sessions(client) ON DELETE CASCADE,
        filter TEXT NOT NULL,qos INTEGER NOT NULL,PRIMARY KEY(client,filter));
      CREATE TABLE IF NOT EXISTS messages(id INTEGER PRIMARY KEY AUTOINCREMENT,wire BLOB NOT NULL,expires INTEGER NOT NULL,refs INTEGER NOT NULL DEFAULT 0);
      CREATE TABLE IF NOT EXISTS deliveries(client TEXT NOT NULL REFERENCES sessions(client) ON DELETE CASCADE,
        message INTEGER NOT NULL REFERENCES messages(id),packet INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(client,message));
      CREATE UNIQUE INDEX IF NOT EXISTS delivery_packets ON deliveries(client,packet) WHERE packet>0;
      CREATE TABLE IF NOT EXISTS counters(id INTEGER PRIMARY KEY CHECK(id=1),epoch INTEGER NOT NULL,pending INTEGER NOT NULL,bytes INTEGER NOT NULL);
      INSERT OR IGNORE INTO counters VALUES(1,0,0,0);
      CREATE TRIGGER IF NOT EXISTS delivery_added AFTER INSERT ON deliveries BEGIN
        UPDATE messages SET refs=refs+1 WHERE id=new.message;
        UPDATE sessions SET pending=pending+1,bytes=bytes+(SELECT length(wire) FROM messages WHERE id=new.message) WHERE client=new.client;
        UPDATE counters SET pending=pending+1,bytes=bytes+(SELECT length(wire) FROM messages WHERE id=new.message) WHERE id=1;
      END;
      CREATE TRIGGER IF NOT EXISTS delivery_removed AFTER DELETE ON deliveries BEGIN
        UPDATE sessions SET pending=pending-1,bytes=bytes-(SELECT length(wire) FROM messages WHERE id=old.message) WHERE client=old.client;
        UPDATE counters SET pending=pending-1,bytes=bytes-(SELECT length(wire) FROM messages WHERE id=old.message) WHERE id=1;
        UPDATE messages SET refs=refs-1 WHERE id=old.message;
        DELETE FROM messages WHERE id=old.message AND refs=0;
      END;
      PRAGMA user_version=1;
    )SQL");
    sql(db_, "BEGIN IMMEDIATE");
    sweep(db_, true);
    sql(db_, "COMMIT");
    {
      Statement s(db_, "SELECT count(*) FROM subscriptions WHERE qos>0");
      s.step();
      subscription_count_.store(s.integer(0));
    }
    worker_ = std::thread(&DurableStore::work, this);
  } catch (...) {
    if (db_)
      sqlite3_close(db_);
    if (lock_fd_ >= 0)
      close(lock_fd_);
    throw;
  }
}
DurableStore::~DurableStore()
{
  {
    std::lock_guard<std::mutex> guard(mutex_);
    stopping_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable())
    worker_.join();
  if (db_)
    sqlite3_close(db_);
  if (lock_fd_ >= 0)
    close(lock_fd_);
}
DurableStore::Result DurableStore::execute(size_t bytes, std::function<Result(sqlite3*)> fn,
                                           bool bulk, bool wait_for_commit,
                                           const std::string& failure_client,
                                           uint64_t failure_epoch)
{
  auto r = std::make_shared<Request>(bytes, std::move(fn), wait_for_commit);
  r->bulk = bulk;
  r->failure_client = failure_client;
  r->failure_epoch = failure_epoch;
  if (wait_for_commit && r->fd < 0) {
    r->result.error = "persistent completion descriptor unavailable";
    return r->result;
  }
  const auto admission_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  for (;;) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      bool full = outstanding_ >= config_.max_requests || bytes > config_.max_request_bytes ||
                  bytes_ > config_.max_request_bytes - std::min(bytes, config_.max_request_bytes) ||
                  (bulk && (outstanding_ >= std::max<size_t>(1, config_.max_requests / 4) ||
                            bytes > config_.max_request_bytes / 2 ||
                            bulk_bytes_ > config_.max_request_bytes / 2 -
                                              std::min(bytes, config_.max_request_bytes / 2)));
      if (stopping_ || (full && (bulk || bytes > config_.max_request_bytes ||
                                 std::chrono::steady_clock::now() >= admission_deadline))) {
        r->result.error = "persistent IO admission full";
        return r->result;
      }
      if (!full) {
        ++outstanding_;
        bytes_ += bytes;
        if (bulk)
          bulk_bytes_ += bytes;
        queue_.push_back(r);
        break;
      }
    }
    // Briefly backpressure the calling connection during an ACK burst. The
    // admitted queue stays bounded and the network event thread keeps running.
    runtime::current_runtime().wait(-1, 0, 2);
  }
  changed_.notify_one();
  if (!wait_for_commit)
    return success();
  while (!r->done.load(std::memory_order_acquire))
    runtime::current_runtime().wait_readable(r->fd, 100);
  return std::move(r->result);
}
bool DurableStore::reserve_result(size_t bytes)
{
  std::lock_guard<std::mutex> guard(mutex_);
  if (!active_request_ || bytes > config_.max_request_bytes / 2 ||
      bulk_bytes_ > config_.max_request_bytes / 2 - bytes ||
      bytes_ > config_.max_request_bytes - bytes)
    return false;
  bytes_ += bytes;
  bulk_bytes_ += bytes;
  active_request_->bytes += bytes;
  return true;
}
void DurableStore::sweep(sqlite3* db, bool startup)
{
  if (startup)
    sql(db, "UPDATE sessions SET online=0");
  Statement expired(db, "DELETE FROM sessions WHERE online=0 AND deadline<=?");
  expired.bind(1, now_ms());
  expired.step();
  if (sqlite3_changes(db))
    index_dirty_ = true;
  Statement waking(db,
                   "SELECT DISTINCT d.client FROM deliveries d JOIN messages m ON m.id=d.message "
                   "WHERE m.expires>0 AND m.expires<=?");
  waking.bind(1, now_ms());
  while (waking.step()) {
    auto it = signals_.find(waking.text(0));
    if (it != signals_.end())
      it->second->fetch_add(1);
  }
  Statement messages(db,
                     "DELETE FROM deliveries WHERE message IN (SELECT id FROM messages WHERE "
                     "expires>0 AND expires<=?)");
  messages.bind(1, now_ms());
  messages.step();
  for (auto it = signals_.begin(); it != signals_.end();) {
    Statement exists(db, "SELECT 1 FROM sessions WHERE client=?");
    exists.bind(1, it->first);
    if (!exists.step()) {
      it->second->fetch_add(1);
      failed_epochs_.erase(it->first);
      it = signals_.erase(it);
    } else
      ++it;
  }
  // A crash expires online sessions from the last heartbeat, never from a new
  // lease granted on restart. This can expire up to one second conservatively.
  Statement heartbeat(db, "UPDATE sessions SET deadline=?+expiry*1000 WHERE online=1");
  heartbeat.bind(1, now_ms());
  heartbeat.step();
}
void DurableStore::work()
{
  int64_t swept = now_ms();
  for (;;) {
    std::vector<std::shared_ptr<Request>> batch;
    {
      std::unique_lock<std::mutex> guard(mutex_);
      changed_.wait_for(guard, std::chrono::milliseconds(100),
                        [&] { return stopping_ || !queue_.empty(); });
      if (stopping_ && queue_.empty())
        break;
      if (!queue_.empty())
        changed_.wait_for(guard, std::chrono::milliseconds(1),
                          [&] { return stopping_ || queue_.size() >= 32; });
      while (!queue_.empty() && batch.size() < 32) {
        batch.push_back(queue_.front());
        queue_.pop_front();
      }
    }
    if (batch.empty() && now_ms() - swept < 1000)
      continue;
    try {
      sql(db_, "BEGIN IMMEDIATE");
      if (now_ms() - swept >= 1000) {
        sweep(db_);
        swept = now_ms();
      }
      for (auto& r : batch) {
        sql(db_, "SAVEPOINT request");
        try {
          active_request_ = r.get();
          r->result = r->fn(db_);
          active_request_ = nullptr;
          sql(db_, "RELEASE request");
        } catch (const std::exception& e) {
          sql(db_, "ROLLBACK TO request; RELEASE request");
          index_dirty_ = true;
          r->result = Result();
          r->result.error = e.what();
        }
      }
      sql(db_, "COMMIT");
      for (auto& r : batch)
        if (r->result.wake)
          for (const auto& target : r->result.targets) {
            auto signal = signals_.find(target);
            if (signal != signals_.end())
              signal->second->fetch_add(1);
          }
      Statement count(db_, "SELECT count(*) FROM subscriptions WHERE qos>0");
      count.step();
      subscription_count_.store(count.integer(0));
    } catch (const std::exception& e) {
      sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
      index_dirty_ = true;
      LOG_ERROR("Persistent transaction failed: {}", e.what());
      for (auto& r : batch) {
        r->result = Result();
        r->result.error = e.what();
      }
    }
    active_request_ = nullptr;
    for (auto& r : batch) {
      // ACK deletion can be deferred safely (a crash may repeat delivery), but
      // an unsuccessful async deletion must reconnect instead of wedging the
      // receive window behind records the client has already acknowledged.
      if (!r->result.ok && !r->failure_client.empty()) {
        auto& failed = failed_epochs_[r->failure_client];
        failed = std::max(failed, r->failure_epoch);
        auto signal = signals_.find(r->failure_client);
        if (signal != signals_.end())
          signal->second->fetch_add(1);
      }
      {
        std::lock_guard<std::mutex> guard(mutex_);
        --outstanding_;
        bytes_ -= r->bytes;
        if (r->bulk)
          bulk_bytes_ -= r->bytes;
      }
      r->done.store(true, std::memory_order_release);
      uint64_t one = 1;
      if (r->fd >= 0)
        (void)write(r->fd, &one, sizeof(one));
    }
  }
}
DurableStore::Result DurableStore::connect(const std::string& client, const std::string& owner,
                                           bool clean, uint32_t expiry)
{
  expiry = std::min(expiry, config_.max_session_expiry_seconds);
  return execute(1024, [=](sqlite3* db) {
    index_dirty_ = true;
    Result r = success();
    {
      Statement expired(db, "DELETE FROM sessions WHERE client=? AND online=0 AND deadline<=?");
      expired.bind(1, client);
      expired.bind(2, now_ms());
      expired.step();
    }
    bool exists = false;
    {
      Statement row(db, "SELECT owner FROM sessions WHERE client=?");
      row.bind(1, client);
      exists = row.step();
      if (exists && row.text(0) != owner)
        throw std::runtime_error("persistent session belongs to another identity");
    }
    if (clean) {
      Statement drop(db, "DELETE FROM sessions WHERE client=?");
      drop.bind(1, client);
      drop.step();
      exists = false;
    }
    if (!exists && expiry == 0) {
      r.wake = true;
      r.targets.push_back(client);
      return r;
    }
    if (!exists) {
      Statement count(db, "SELECT count(*) FROM sessions");
      count.step();
      if (count.integer(0) >= int64_t(config_.max_sessions))
        throw std::runtime_error("persistent session capacity exhausted");
    }
    sql(db, "UPDATE counters SET epoch=epoch+1 WHERE id=1");
    {
      Statement generation(db, "SELECT epoch FROM counters WHERE id=1");
      generation.step();
      r.epoch = generation.integer(0);
    }
    Statement row(
        db,
        "INSERT INTO sessions(client,owner,epoch,expiry,deadline,online) VALUES(?,?,?,?,?,1) ON "
        "CONFLICT(client) DO UPDATE SET "
        "epoch=excluded.epoch,expiry=excluded.expiry,deadline=excluded.deadline,online=1");
    row.bind(1, client);
    row.bind(2, owner);
    row.bind(3, int64_t(r.epoch));
    row.bind(4, int64_t(expiry));
    row.bind(5, now_ms() + int64_t(expiry) * 1000);
    row.step();
    failed_epochs_.erase(client);
    r.present = exists;
    r.wake = true;
    r.targets.push_back(client);
    auto& signal = signals_[client];
    if (!signal)
      signal = std::make_shared<std::atomic<uint64_t>>(0);
    r.revision = signal;
    Statement subs(db, "SELECT filter,qos FROM subscriptions WHERE client=? ORDER BY filter");
    subs.bind(1, client);
    while (subs.step())
      r.subscriptions.emplace_back(subs.text(0), uint8_t(subs.integer(1)));
    return r;
  });
}
DurableStore::Result DurableStore::disconnect(const std::string& client, uint64_t epoch,
                                              int64_t expiry_override)
{
  return execute(512, [=](sqlite3* db) {
    if (!current(db, client, epoch))
      return stale();
    if (expiry_override >= 0) {
      Statement s(db, "UPDATE sessions SET expiry=? WHERE client=?");
      s.bind(1, std::min<int64_t>(expiry_override, config_.max_session_expiry_seconds));
      s.bind(2, client);
      s.step();
    }
    Statement end(db,
                  "UPDATE sessions SET online=0,deadline=?+expiry*1000 WHERE client=? AND epoch=?");
    end.bind(1, now_ms());
    end.bind(2, client);
    end.bind(3, int64_t(epoch));
    end.step();
    Statement drop(db, "DELETE FROM sessions WHERE client=? AND expiry=0");
    drop.bind(1, client);
    drop.step();
    if (sqlite3_changes(db))
      index_dirty_ = true;
    return success();
  });
}
DurableStore::Result DurableStore::subscribe(const std::string& client, uint64_t epoch,
                                             const std::string& filter, uint8_t qos)
{
  return execute(filter.size() + 512, [=](sqlite3* db) {
    if (!current(db, client, epoch))
      return stale();
    Statement count(db, "SELECT count(*) FROM subscriptions WHERE client=? AND filter<>?");
    count.bind(1, client);
    count.bind(2, filter);
    count.step();
    if (count.integer(0) >= int64_t(config_.max_subscriptions_per_session))
      throw std::runtime_error("persistent subscription capacity exhausted");
    Statement s(db,
                "INSERT INTO subscriptions VALUES(?,?,?) ON CONFLICT(client,filter) DO UPDATE SET "
                "qos=excluded.qos");
    s.bind(1, client);
    s.bind(2, filter);
    s.bind(3, int64_t(qos));
    s.step();
    index_dirty_ = true;
    return success();
  });
}
DurableStore::Result DurableStore::unsubscribe(const std::string& client, uint64_t epoch,
                                               const std::string& filter)
{
  return execute(filter.size() + 512, [=](sqlite3* db) {
    if (!current(db, client, epoch))
      return stale();
    Statement s(db, "DELETE FROM subscriptions WHERE client=? AND filter=?");
    s.bind(1, client);
    s.bind(2, filter);
    s.step();
    index_dirty_ = true;
    return success();
  });
}
// Worker-owned trie: publishing visits matching topic levels and recipients,
// never scans all persisted subscriptions. Rebuild only after subscription or
// session changes; flat nodes also bound stack use for deeply nested filters.
std::vector<std::string> DurableStore::match_targets(sqlite3* db, const std::string& topic)
{
  if (index_dirty_) {
    std::vector<TopicNode> rebuilt(1);
    Statement rows(db, "SELECT client,filter FROM subscriptions WHERE qos>0");
    while (rows.step()) {
      std::string client = rows.text(0), filter = rows.text(1);
      size_t node = 0, offset = 0;
      for (;;) {
        size_t end = filter.find('/', offset);
        std::string level = filter.substr(offset, end == std::string::npos ? end : end - offset);
        auto found = rebuilt[node].children.find(level);
        if (found == rebuilt[node].children.end()) {
          size_t next = rebuilt.size();
          rebuilt.emplace_back();
          rebuilt[node].children.emplace(level, next);
          node = next;
        } else
          node = found->second;
        if (end == std::string::npos)
          break;
        offset = end + 1;
      }
      rebuilt[node].clients.push_back(std::move(client));
    }
    topic_index_ = std::move(rebuilt);
    index_dirty_ = false;
  }
  std::set<std::string> candidates;
  std::vector<std::pair<size_t, size_t>> pending{{0, 0}};
  while (!pending.empty()) {
    auto item = pending.back();
    pending.pop_back();
    const auto& node = topic_index_[item.first];
    bool system = item.first == 0 && !topic.empty() && topic[0] == '$';
    auto hash = node.children.find("#");
    if (!system && hash != node.children.end())
      for (const auto& c : topic_index_[hash->second].clients)
        candidates.insert(c);
    if (item.second > topic.size()) {
      for (const auto& c : node.clients)
        candidates.insert(c);
      continue;
    }
    size_t end = topic.find('/', item.second),
           next = end == std::string::npos ? topic.size() + 1 : end + 1;
    std::string level =
        topic.substr(item.second, end == std::string::npos ? end : end - item.second);
    auto exact = node.children.find(level);
    if (exact != node.children.end())
      pending.emplace_back(exact->second, next);
    auto plus = node.children.find("+");
    if (!system && plus != node.children.end())
      pending.emplace_back(plus->second, next);
  }
  std::vector<std::string> targets;
  for (const auto& client : candidates) {
    Statement live(db, "SELECT 1 FROM sessions WHERE client=? AND (online=1 OR deadline>?)");
    live.bind(1, client);
    live.bind(2, now_ms());
    if (live.step())
      targets.push_back(client);
  }
  return targets;
}
DurableStore::Result DurableStore::publish(const std::string& topic, const std::string& wire,
                                           const std::string& sender, int64_t expires)
{
  return execute(wire.size() + topic.size() + 512, [=](sqlite3* db) {
    Result r = success();
    if (expires > 0 && expires <= now_ms())
      return r;
    r.targets = match_targets(db, topic);
    if (r.targets.empty())
      return r;
    {
      Statement budget(db, "SELECT pending,bytes FROM counters WHERE id=1");
      budget.step();
      if (r.targets.size() > config_.max_messages ||
          uint64_t(budget.integer(0)) > config_.max_messages - r.targets.size() ||
          wire.size() > config_.max_bytes / r.targets.size() ||
          uint64_t(budget.integer(1)) > config_.max_bytes - wire.size() * r.targets.size())
        throw std::runtime_error("persistent message capacity exhausted");
    }
    for (const auto& target : r.targets) {
      Statement budget(db, "SELECT pending FROM sessions WHERE client=?");
      budget.bind(1, target);
      budget.step();
      if (budget.integer(0) >= int64_t(config_.max_messages_per_session))
        throw std::runtime_error("persistent client backlog exhausted");
    }
    Statement m(db, "INSERT INTO messages(wire,expires) VALUES(?,?)");
    m.blob(1, wire);
    m.bind(2, expires);
    m.step();
    int64_t id = sqlite3_last_insert_rowid(db);
    for (const auto& target : r.targets) {
      Statement d(db, "INSERT INTO deliveries(client,message) VALUES(?,?)");
      d.bind(1, target);
      d.bind(2, id);
      d.step();
    }
    r.wake = true;
    return r;
  });
}
DurableStore::Result DurableStore::fetch(const std::string& client, uint64_t epoch, int64_t after,
                                         uint16_t receive_maximum)
{
  return execute(
      512,
      [=](sqlite3* db) {
        if (failed_epochs_[client] == epoch || !current(db, client, epoch))
          return stale();
        Result r = success();
        size_t inflight = 0;
        int64_t next = 1;
        {
          Statement n(db,
                      "SELECT count(*) FROM deliveries WHERE client=? AND packet>0 AND message<=?");
          n.bind(1, client);
          n.bind(2, after);
          n.step();
          inflight = n.integer(0);
        }
        {
          Statement n(db, "SELECT next_packet FROM sessions WHERE client=?");
          n.bind(1, client);
          n.step();
          next = n.integer(0);
        }
        size_t limit = std::min<size_t>(config_.max_inflight,
                                        receive_maximum ? receive_maximum : 65535),
               bytes = 0;
        Statement rows(
            db,
            "SELECT d.message,d.packet,m.wire,m.expires FROM deliveries d JOIN messages m ON "
            "m.id=d.message WHERE d.client=? AND d.message>? ORDER BY d.message LIMIT 32");
        rows.bind(1, client);
        rows.bind(2, after);
        while (rows.step()) {
          Delivery d;
          d.sequence = rows.integer(0);
          d.packet_id = rows.integer(1);
          d.dup = d.packet_id != 0;
          d.expires = rows.integer(3);
          if (d.expires > 0 && d.expires <= now_ms())
            continue;
          if (inflight >= limit)
            break;
          // Reserve actual result bytes rather than a maximum-sized packet for
          // every idle reader. This keeps admission bounded without starving
          // publishers during a burst of hundreds of small-message consumers.
          if (!reserve_result(size_t(sqlite3_column_bytes(rows.s, 2)))) {
            if (r.deliveries.empty()) {
              r.ok = false;
              r.error = "persistent read admission full";
            }
            break;
          }
          if (d.packet_id == 0) {
            for (int tries = 0; tries < 65535; ++tries) {
              Statement used(db, "SELECT 1 FROM deliveries WHERE client=? AND packet=?");
              used.bind(1, client);
              used.bind(2, next);
              if (!used.step())
                break;
              next = next % 65535 + 1;
            }
            d.packet_id = uint16_t(next);
            next = next % 65535 + 1;
            Statement claim(db, "UPDATE deliveries SET packet=? WHERE client=? AND message=?");
            claim.bind(1, int64_t(d.packet_id));
            claim.bind(2, client);
            claim.bind(3, d.sequence);
            claim.step();
          }
          ++inflight;
          d.wire = rows.text(2);
          bytes += d.wire.size();
          r.deliveries.push_back(std::move(d));
          if (bytes >= 256 * 1024)
            break;
        }
        Statement n(db, "UPDATE sessions SET next_packet=? WHERE client=?");
        n.bind(1, next);
        n.bind(2, client);
        n.step();
        return r;
      },
      true);
}
DurableStore::Result DurableStore::acknowledge(const std::string& client, uint64_t epoch,
                                               uint16_t packet_id, bool wait_for_commit)
{
  return execute(
      512,
      [=](sqlite3* db) {
        if (!current(db, client, epoch))
          return stale();
        Statement s(db, "DELETE FROM deliveries WHERE client=? AND packet=? AND packet>0");
        s.bind(1, client);
        s.bind(2, int64_t(packet_id));
        s.step();
        auto r = success();
        r.wake = true;
        r.targets.push_back(client);
        return r;
      },
      false, wait_for_commit, client, epoch);
}
}  // namespace mqtt
