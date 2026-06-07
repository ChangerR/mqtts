#include "mqtt_router_service.h"
#include "logger.h"
#include "mqtt_memory_tags.h"
#include "mqtt_router.pb.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

thread_local std::vector<MQTTRouterService::ClientContext*> MQTTRouterService::thread_local_clients_;
thread_local MQTTAllocator* MQTTRouterService::thread_local_allocator_ = nullptr;

namespace
{

int send_all_bytes(int fd, const void* buf, size_t len)
{
    int ret = MQ_SUCCESS;
    size_t sent_len = 0;
    const char* data = static_cast<const char*>(buf);

    while (MQ_SUCCESS == ret && sent_len < len) {
        ssize_t write_len = ::send(fd, data + sent_len, len - sent_len, 0);
        if (write_len <= 0) {
            ret = MQ_ERR_SOCKET_SEND;
        } else {
            sent_len += static_cast<size_t>(write_len);
        }
    }

    return ret;
}

int recv_all_bytes(int fd, void* buf, size_t len)
{
    int ret = MQ_SUCCESS;
    size_t recv_len = 0;
    char* data = static_cast<char*>(buf);

    while (MQ_SUCCESS == ret && recv_len < len) {
        ssize_t read_len = ::recv(fd, data + recv_len, len - recv_len, 0);
        if (read_len <= 0) {
            ret = MQ_ERR_SOCKET_RECV;
        } else {
            recv_len += static_cast<size_t>(read_len);
        }
    }

    return ret;
}

uint64_t current_time_ms()
{
    uint64_t now_ms = 0;
    now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return now_ms;
}

MQTTString make_subscription_key(const MQTTString& server_id,
                                 const MQTTString& client_id,
                                 const MQTTString& topic_filter,
                                 MQTTAllocator* allocator)
{
    MQTTString key{MQTTStrAllocator(allocator)};
    key = server_id;
    key += MQTTString("@", MQTTStrAllocator(allocator));
    key += client_id;
    key += MQTTString("@", MQTTStrAllocator(allocator));
    key += topic_filter;
    return key;
}

MQTTString make_full_client_id(const MQTTString& server_id,
                               const MQTTString& client_id,
                               MQTTAllocator* allocator)
{
    MQTTString full_client_id{MQTTStrAllocator(allocator)};
    full_client_id = server_id;
    full_client_id += MQTTString("@", MQTTStrAllocator(allocator));
    full_client_id += client_id;
    return full_client_id;
}

int write_router_envelope(int client_fd, const ::mqtt::router::RouterEnvelope& envelope)
{
    int ret = MQ_SUCCESS;
    std::string data;
    uint32_t payload_len_n = 0;

    if (!envelope.SerializeToString(&data)) {
        ret = MQ_ERR_ROUTER_PROTOCOL;
    } else {
        payload_len_n = htonl(static_cast<uint32_t>(data.size()));
        if (MQ_FAIL(send_all_bytes(client_fd, &payload_len_n, sizeof(payload_len_n)))) {
        } else if (MQ_FAIL(send_all_bytes(client_fd, data.data(), data.size()))) {
        }
    }

    return ret;
}

int read_router_envelope(int client_fd, ::mqtt::router::RouterEnvelope& envelope)
{
    int ret = MQ_SUCCESS;
    uint32_t payload_len_n = 0;
    uint32_t payload_len = 0;
    std::string data;

    if (MQ_FAIL(recv_all_bytes(client_fd, &payload_len_n, sizeof(payload_len_n)))) {
    } else {
        payload_len = ntohl(payload_len_n);
        data.resize(payload_len);
        if (payload_len > 0 && MQ_FAIL(recv_all_bytes(client_fd, &data[0], payload_len))) {
        } else if (!envelope.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
            ret = MQ_ERR_ROUTER_PROTOCOL;
        }
    }

    return ret;
}

}  // namespace

MQTTPersistentTopicTree::MQTTPersistentTopicTree(MQTTAllocator* allocator)
    : allocator_(allocator)
    , topic_tree_()
    , subscription_entry_map_(MQTTSTLAllocator<std::pair<const MQTTString, RouterLogEntry> >(allocator))
    , snapshot_mutex_()
{
    MQTTAllocator* topic_tree_allocator = allocator_->create_child(
        "persistent_topic_tree", MQTTMemoryTag::MEM_TAG_TOPIC_TREE);
    topic_tree_.reset(new ConcurrentTopicTree(topic_tree_allocator));
}

MQTTPersistentTopicTree::~MQTTPersistentTopicTree()
{
    if (allocator_ != NULL) {
        allocator_->remove_child("persistent_topic_tree");
    }
}

int MQTTPersistentTopicTree::subscribe(const MQTTString& server_id,
                                       const MQTTString& topic_filter,
                                       const MQTTString& client_id,
                                       uint8_t qos)
{
    int ret = MQ_SUCCESS;
    MQTTString full_client_id{MQTTStrAllocator(allocator_)};
    MQTTString subscription_key{MQTTStrAllocator(allocator_)};
    RouterLogEntry entry(allocator_);
    std::lock_guard<std::mutex> guard(snapshot_mutex_);

    full_client_id = make_full_client_id(server_id, client_id, allocator_);
    subscription_key = make_subscription_key(server_id, client_id, topic_filter, allocator_);
    ret = topic_tree_->subscribe(topic_filter, full_client_id, qos);
    if (MQ_SUCCESS == ret) {
        entry.op_type = RouterLogOpType::SUBSCRIBE;
        entry.server_id = server_id;
        entry.client_id = client_id;
        entry.topic_filter = topic_filter;
        entry.qos = qos;
        subscription_entry_map_[subscription_key] = entry;
    }

    return ret;
}

int MQTTPersistentTopicTree::unsubscribe(const MQTTString& server_id,
                                         const MQTTString& topic_filter,
                                         const MQTTString& client_id)
{
    int ret = MQ_SUCCESS;
    MQTTString full_client_id{MQTTStrAllocator(allocator_)};
    MQTTString subscription_key{MQTTStrAllocator(allocator_)};
    std::lock_guard<std::mutex> guard(snapshot_mutex_);

    full_client_id = make_full_client_id(server_id, client_id, allocator_);
    subscription_key = make_subscription_key(server_id, client_id, topic_filter, allocator_);
    ret = topic_tree_->unsubscribe(topic_filter, full_client_id);
    if (MQ_SUCCESS == ret) {
        (void)subscription_entry_map_.erase(subscription_key);
    }

    return ret;
}

int MQTTPersistentTopicTree::unsubscribe_all(const MQTTString& server_id, const MQTTString& client_id)
{
    int ret = MQ_SUCCESS;
    MQTTString full_client_id{MQTTStrAllocator(allocator_)};
    TopicTreeLevelVector subscriptions{TopicTreeAllocator<MQTTString>(allocator_)};
    std::lock_guard<std::mutex> guard(snapshot_mutex_);

    full_client_id = make_full_client_id(server_id, client_id, allocator_);
    if (MQ_FAIL(topic_tree_->get_client_subscriptions(full_client_id, subscriptions))) {
    } else {
        ret = topic_tree_->unsubscribe_all(full_client_id);
        if (MQ_SUCCESS == ret) {
            for (size_t i = 0; i < subscriptions.size(); ++i) {
                MQTTString subscription_key =
                    make_subscription_key(server_id, client_id, subscriptions[i], allocator_);
                (void)subscription_entry_map_.erase(subscription_key);
            }
        }
    }

    return ret;
}

int MQTTPersistentTopicTree::find_route_targets(const MQTTString& topic,
                                                RouteTargetVector& targets)
{
    int ret = MQ_SUCCESS;
    MQTTString empty_server_id{MQTTStrAllocator(allocator_)};
    ret = find_route_targets(topic, empty_server_id, targets);
    return ret;
}

int MQTTPersistentTopicTree::find_route_targets(const MQTTString& topic,
                                                const MQTTString& publisher_server_id,
                                                RouteTargetVector& targets)
{
    int ret = MQ_SUCCESS;
    TopicMatchResult match_result(allocator_);

    targets.clear();
    if (MQ_FAIL(topic_tree_->find_subscribers(topic, match_result))) {
    } else {
        for (size_t i = 0; MQ_SUCCESS == ret && i < match_result.subscribers.size(); ++i) {
            const SubscriberInfo& subscriber = match_result.subscribers[i];
            MQTTString full_client_id = subscriber.client_id;
            size_t at_pos = full_client_id.find('@');
            if (MQTTString::npos == at_pos) {
                ret = MQ_ERR_ROUTER_PROTOCOL;
            } else {
                MQTTString server_id = full_client_id.substr(0, at_pos);
                MQTTString client_id = full_client_id.substr(at_pos + 1);
                if (server_id != publisher_server_id) {
                    RouteTarget target(allocator_);
                    target.server_id = server_id;
                    target.client_id = client_id;
                    target.qos = subscriber.qos;
                    targets.push_back(target);
                }
            }
        }
    }

    return ret;
}

int MQTTPersistentTopicTree::load_from_snapshot(const std::string& snapshot_path)
{
    int ret = MQ_SUCCESS;
    std::ifstream snapshot_file(snapshot_path.c_str(), std::ios::binary);
    uint32_t version = 0;
    uint64_t entry_count = 0;

    std::lock_guard<std::mutex> guard(snapshot_mutex_);

    if (!snapshot_file.is_open()) {
        ret = MQ_SUCCESS;
    } else if (!snapshot_file.read(reinterpret_cast<char*>(&version), sizeof(version))) {
        ret = MQ_ERR_ROUTER_SNAPSHOT;
    } else if (version != 2) {
        ret = MQ_ERR_ROUTER_SNAPSHOT;
    } else if (!snapshot_file.read(reinterpret_cast<char*>(&entry_count), sizeof(entry_count))) {
        ret = MQ_ERR_ROUTER_SNAPSHOT;
    } else {
        for (uint64_t i = 0; MQ_SUCCESS == ret && i < entry_count; ++i) {
            RouterLogEntry entry(allocator_);
            uint8_t op_type = 0;
            uint32_t field_len = 0;
            MQTTVector<char> field_buf{MQTTSTLAllocator<char>(allocator_)};

            if (!snapshot_file.read(reinterpret_cast<char*>(&op_type), sizeof(op_type))) {
                ret = MQ_ERR_ROUTER_SNAPSHOT;
            } else {
                entry.op_type = static_cast<RouterLogOpType>(op_type);
            }

            if (MQ_SUCCESS == ret &&
                !snapshot_file.read(reinterpret_cast<char*>(&field_len), sizeof(field_len))) {
                ret = MQ_ERR_ROUTER_SNAPSHOT;
            } else if (MQ_SUCCESS == ret && field_len > 0) {
                field_buf.resize(field_len);
                if (!snapshot_file.read(field_buf.data(), field_len)) {
                    ret = MQ_ERR_ROUTER_SNAPSHOT;
                } else {
                    entry.server_id = MQTTString(field_buf.data(), field_len, MQTTStrAllocator(allocator_));
                }
            }

            if (MQ_SUCCESS == ret &&
                !snapshot_file.read(reinterpret_cast<char*>(&field_len), sizeof(field_len))) {
                ret = MQ_ERR_ROUTER_SNAPSHOT;
            } else if (MQ_SUCCESS == ret && field_len > 0) {
                field_buf.resize(field_len);
                if (!snapshot_file.read(field_buf.data(), field_len)) {
                    ret = MQ_ERR_ROUTER_SNAPSHOT;
                } else {
                    entry.client_id = MQTTString(field_buf.data(), field_len, MQTTStrAllocator(allocator_));
                }
            }

            if (MQ_SUCCESS == ret &&
                !snapshot_file.read(reinterpret_cast<char*>(&field_len), sizeof(field_len))) {
                ret = MQ_ERR_ROUTER_SNAPSHOT;
            } else if (MQ_SUCCESS == ret && field_len > 0) {
                field_buf.resize(field_len);
                if (!snapshot_file.read(field_buf.data(), field_len)) {
                    ret = MQ_ERR_ROUTER_SNAPSHOT;
                } else {
                    entry.topic_filter = MQTTString(field_buf.data(), field_len, MQTTStrAllocator(allocator_));
                }
            }

            if (MQ_SUCCESS == ret &&
                !snapshot_file.read(reinterpret_cast<char*>(&entry.qos), sizeof(entry.qos))) {
                ret = MQ_ERR_ROUTER_SNAPSHOT;
            }

            if (MQ_SUCCESS == ret) {
                ret = subscribe(entry.server_id, entry.topic_filter, entry.client_id, entry.qos);
            }
        }
    }

    return ret;
}

int MQTTPersistentTopicTree::save_to_snapshot(const std::string& snapshot_path)
{
    int ret = MQ_SUCCESS;
    std::ofstream snapshot_file((snapshot_path + ".tmp").c_str(), std::ios::binary);
    uint32_t version = 2;
    uint64_t entry_count = 0;

    std::lock_guard<std::mutex> guard(snapshot_mutex_);

    if (!snapshot_file.is_open()) {
        ret = MQ_ERR_ROUTER_SNAPSHOT;
    } else {
        entry_count = static_cast<uint64_t>(subscription_entry_map_.size());
        snapshot_file.write(reinterpret_cast<const char*>(&version), sizeof(version));
        snapshot_file.write(reinterpret_cast<const char*>(&entry_count), sizeof(entry_count));
        for (SubscriptionEntryMap::const_iterator it = subscription_entry_map_.begin();
             MQ_SUCCESS == ret && it != subscription_entry_map_.end();
             ++it) {
            const RouterLogEntry& entry = it->second;
            uint8_t op_type = static_cast<uint8_t>(entry.op_type);
            uint32_t field_len = 0;

            snapshot_file.write(reinterpret_cast<const char*>(&op_type), sizeof(op_type));

            field_len = static_cast<uint32_t>(entry.server_id.length());
            snapshot_file.write(reinterpret_cast<const char*>(&field_len), sizeof(field_len));
            if (field_len > 0) {
                snapshot_file.write(entry.server_id.c_str(), field_len);
            }

            field_len = static_cast<uint32_t>(entry.client_id.length());
            snapshot_file.write(reinterpret_cast<const char*>(&field_len), sizeof(field_len));
            if (field_len > 0) {
                snapshot_file.write(entry.client_id.c_str(), field_len);
            }

            field_len = static_cast<uint32_t>(entry.topic_filter.length());
            snapshot_file.write(reinterpret_cast<const char*>(&field_len), sizeof(field_len));
            if (field_len > 0) {
                snapshot_file.write(entry.topic_filter.c_str(), field_len);
            }

            snapshot_file.write(reinterpret_cast<const char*>(&entry.qos), sizeof(entry.qos));
            if (!snapshot_file.good()) {
                ret = MQ_ERR_ROUTER_SNAPSHOT;
            }
        }
    }

    if (MQ_SUCCESS == ret) {
        snapshot_file.close();
        if (::rename((snapshot_path + ".tmp").c_str(), snapshot_path.c_str()) != 0) {
            ret = MQ_ERR_ROUTER_SNAPSHOT;
        }
    }

    return ret;
}

int MQTTPersistentTopicTree::apply_redo_log_entry(const RouterLogEntry& entry)
{
    int ret = MQ_SUCCESS;

    switch (entry.op_type) {
        case RouterLogOpType::SUBSCRIBE:
            ret = subscribe(entry.server_id, entry.topic_filter, entry.client_id, entry.qos);
            break;
        case RouterLogOpType::UNSUBSCRIBE:
            ret = unsubscribe(entry.server_id, entry.topic_filter, entry.client_id);
            break;
        case RouterLogOpType::UNSUBSCRIBE_ALL:
            ret = unsubscribe_all(entry.server_id, entry.client_id);
            break;
        case RouterLogOpType::CLIENT_CONNECT:
            ret = MQ_SUCCESS;
            break;
        case RouterLogOpType::CLIENT_DISCONNECT:
            ret = unsubscribe_all(entry.server_id, entry.client_id);
            break;
        default:
            ret = MQ_ERR_ROUTER_REDO;
            break;
    }

    return ret;
}

int MQTTPersistentTopicTree::get_statistics(size_t& total_servers,
                                            size_t& total_clients,
                                            size_t& total_subscriptions)
{
    int ret = MQ_SUCCESS;
    total_servers = 0;
    total_clients = 0;
    total_subscriptions = 0;

    std::lock_guard<std::mutex> guard(snapshot_mutex_);
    total_subscriptions = subscription_entry_map_.size();
    total_clients = total_subscriptions;

    return ret;
}

MQTTRedoLogManager::MQTTRedoLogManager(MQTTAllocator* allocator, const MQTTRouterConfig& config)
    : allocator_(allocator)
    , config_(config)
    , redo_log_path_(config.redo_log_path)
    , pending_entries_(MQTTSTLAllocator<RouterLogEntry>(allocator))
    , next_sequence_id_(1)
    , last_flushed_sequence_(0)
    , log_mutex_()
    , log_file_()
    , flush_thread_()
    , should_stop_flush_thread_(false)
{
}

MQTTRedoLogManager::~MQTTRedoLogManager()
{
    should_stop_flush_thread_.store(true);
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
}

int MQTTRedoLogManager::initialize()
{
    int ret = MQ_SUCCESS;
    log_file_.reset(new std::ofstream(redo_log_path_.c_str(), std::ios::binary | std::ios::app));
    if (!log_file_->is_open()) {
        ret = MQ_ERR_ROUTER_REDO;
    } else {
        flush_thread_ = std::thread([this]() {
            while (!should_stop_flush_thread_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(config_.redo_log_flush_interval_ms));
                (void)flush_to_disk();
            }
            (void)flush_to_disk();
        });
    }
    return ret;
}

int MQTTRedoLogManager::append_log_entry(const RouterLogEntry& entry)
{
    int ret = MQ_SUCCESS;
    std::lock_guard<std::mutex> guard(log_mutex_);
    RouterLogEntry log_entry = entry;
    log_entry.sequence_id = next_sequence_id_.fetch_add(1);
    log_entry.timestamp_ms = current_time_ms();
    pending_entries_.push_back(log_entry);
    return ret;
}

int MQTTRedoLogManager::flush_to_disk()
{
    int ret = MQ_SUCCESS;
    std::lock_guard<std::mutex> guard(log_mutex_);

    if (log_file_.get() == NULL || !log_file_->is_open()) {
        ret = MQ_ERR_ROUTER_REDO;
    } else {
        for (size_t i = 0; MQ_SUCCESS == ret && i < pending_entries_.size(); ++i) {
            ret = write_log_entry_to_file(pending_entries_[i]);
            if (MQ_SUCCESS == ret) {
                last_flushed_sequence_.store(pending_entries_[i].sequence_id);
            }
        }
        if (MQ_SUCCESS == ret) {
            log_file_->flush();
            pending_entries_.clear();
        }
    }

    return ret;
}

int MQTTRedoLogManager::load_and_replay(MQTTPersistentTopicTree* topic_tree)
{
    int ret = MQ_SUCCESS;
    std::ifstream redo_file(redo_log_path_.c_str(), std::ios::binary);
    RouterLogEntry entry(allocator_);

    if (!redo_file.is_open()) {
        ret = MQ_SUCCESS;
    } else {
        while (MQ_SUCCESS == ret && MQ_SUCCESS == read_log_entry_from_file(redo_file, entry)) {
            ret = topic_tree->apply_redo_log_entry(entry);
            if (MQ_SUCCESS == ret && entry.sequence_id >= next_sequence_id_.load()) {
                next_sequence_id_.store(entry.sequence_id + 1);
            }
        }
        if (MQ_ERR_ROUTER_REDO == ret) {
            ret = MQ_SUCCESS;
        }
    }

    return ret;
}

int MQTTRedoLogManager::truncate_after_snapshot(uint64_t last_applied_sequence)
{
    int ret = MQ_SUCCESS;
    (void)last_applied_sequence;
    return ret;
}

uint64_t MQTTRedoLogManager::get_next_sequence_id()
{
    return next_sequence_id_.load();
}

size_t MQTTRedoLogManager::get_pending_entries_count()
{
    size_t pending_count = 0;
    std::lock_guard<std::mutex> guard(log_mutex_);
    pending_count = pending_entries_.size();
    return pending_count;
}

int MQTTRedoLogManager::write_log_entry_to_file(const RouterLogEntry& entry)
{
    int ret = MQ_SUCCESS;
    uint8_t op_type = static_cast<uint8_t>(entry.op_type);
    uint32_t field_len = 0;

    if (log_file_.get() == NULL || !log_file_->is_open()) {
        ret = MQ_ERR_ROUTER_REDO;
    } else {
        log_file_->write(reinterpret_cast<const char*>(&op_type), sizeof(op_type));
        log_file_->write(reinterpret_cast<const char*>(&entry.sequence_id), sizeof(entry.sequence_id));
        log_file_->write(reinterpret_cast<const char*>(&entry.timestamp_ms), sizeof(entry.timestamp_ms));

        field_len = static_cast<uint32_t>(entry.server_id.length());
        log_file_->write(reinterpret_cast<const char*>(&field_len), sizeof(field_len));
        if (field_len > 0) {
            log_file_->write(entry.server_id.c_str(), field_len);
        }

        field_len = static_cast<uint32_t>(entry.client_id.length());
        log_file_->write(reinterpret_cast<const char*>(&field_len), sizeof(field_len));
        if (field_len > 0) {
            log_file_->write(entry.client_id.c_str(), field_len);
        }

        field_len = static_cast<uint32_t>(entry.topic_filter.length());
        log_file_->write(reinterpret_cast<const char*>(&field_len), sizeof(field_len));
        if (field_len > 0) {
            log_file_->write(entry.topic_filter.c_str(), field_len);
        }

        log_file_->write(reinterpret_cast<const char*>(&entry.qos), sizeof(entry.qos));
        if (!log_file_->good()) {
            ret = MQ_ERR_ROUTER_REDO;
        }
    }

    return ret;
}

int MQTTRedoLogManager::read_log_entry_from_file(std::ifstream& redo_file, RouterLogEntry& entry)
{
    int ret = MQ_SUCCESS;
    uint8_t op_type = 0;
    uint32_t field_len = 0;
    MQTTVector<char> field_buf{MQTTSTLAllocator<char>(allocator_)};

    if (!redo_file.read(reinterpret_cast<char*>(&op_type), sizeof(op_type))) {
        ret = MQ_ERR_ROUTER_REDO;
    } else {
        entry.op_type = static_cast<RouterLogOpType>(op_type);
    }

    if (MQ_SUCCESS == ret &&
        !redo_file.read(reinterpret_cast<char*>(&entry.sequence_id), sizeof(entry.sequence_id))) {
        ret = MQ_ERR_ROUTER_REDO;
    }
    if (MQ_SUCCESS == ret &&
        !redo_file.read(reinterpret_cast<char*>(&entry.timestamp_ms), sizeof(entry.timestamp_ms))) {
        ret = MQ_ERR_ROUTER_REDO;
    }

    if (MQ_SUCCESS == ret &&
        !redo_file.read(reinterpret_cast<char*>(&field_len), sizeof(field_len))) {
        ret = MQ_ERR_ROUTER_REDO;
    } else if (MQ_SUCCESS == ret && field_len > 0) {
        field_buf.resize(field_len);
        if (!redo_file.read(field_buf.data(), field_len)) {
            ret = MQ_ERR_ROUTER_REDO;
        } else {
            entry.server_id = MQTTString(field_buf.data(), field_len, MQTTStrAllocator(allocator_));
        }
    } else if (MQ_SUCCESS == ret) {
        entry.server_id.clear();
    }

    if (MQ_SUCCESS == ret &&
        !redo_file.read(reinterpret_cast<char*>(&field_len), sizeof(field_len))) {
        ret = MQ_ERR_ROUTER_REDO;
    } else if (MQ_SUCCESS == ret && field_len > 0) {
        field_buf.resize(field_len);
        if (!redo_file.read(field_buf.data(), field_len)) {
            ret = MQ_ERR_ROUTER_REDO;
        } else {
            entry.client_id = MQTTString(field_buf.data(), field_len, MQTTStrAllocator(allocator_));
        }
    } else if (MQ_SUCCESS == ret) {
        entry.client_id.clear();
    }

    if (MQ_SUCCESS == ret &&
        !redo_file.read(reinterpret_cast<char*>(&field_len), sizeof(field_len))) {
        ret = MQ_ERR_ROUTER_REDO;
    } else if (MQ_SUCCESS == ret && field_len > 0) {
        field_buf.resize(field_len);
        if (!redo_file.read(field_buf.data(), field_len)) {
            ret = MQ_ERR_ROUTER_REDO;
        } else {
            entry.topic_filter = MQTTString(field_buf.data(), field_len, MQTTStrAllocator(allocator_));
        }
    } else if (MQ_SUCCESS == ret) {
        entry.topic_filter.clear();
    }

    if (MQ_SUCCESS == ret && !redo_file.read(reinterpret_cast<char*>(&entry.qos), sizeof(entry.qos))) {
        ret = MQ_ERR_ROUTER_REDO;
    }

    return ret;
}

MQTTRouterRpcHandler::MQTTRouterRpcHandler(MQTTPersistentTopicTree* topic_tree,
                                          RouterNodeRegistry* node_registry,
                                          RouterAuthStore* auth_store,
                                          MQTTRedoLogManager* redo_log_manager,
                                          MQTTAllocator* allocator)
    : topic_tree_(topic_tree)
    , node_registry_(node_registry)
    , auth_store_(auth_store)
    , redo_log_manager_(redo_log_manager)
    , allocator_(allocator)
{
}

MQTTRouterRpcHandler::~MQTTRouterRpcHandler()
{
}

int MQTTRouterRpcHandler::handle_subscribe(const SubscribeRequest& request)
{
    int ret = MQ_SUCCESS;
    RouterLogEntry log_entry(allocator_);

    log_entry.op_type = RouterLogOpType::SUBSCRIBE;
    log_entry.server_id = request.server_id;
    log_entry.client_id = request.client_id;
    log_entry.topic_filter = request.topic_filter;
    log_entry.qos = request.qos;

    if (MQ_FAIL(topic_tree_->subscribe(request.server_id, request.topic_filter, request.client_id, request.qos))) {
    } else {
        ret = redo_log_manager_->append_log_entry(log_entry);
    }

    return ret;
}

int MQTTRouterRpcHandler::handle_unsubscribe(const UnsubscribeRequest& request)
{
    int ret = MQ_SUCCESS;
    RouterLogEntry log_entry(allocator_);

    log_entry.op_type = RouterLogOpType::UNSUBSCRIBE;
    log_entry.server_id = request.server_id;
    log_entry.client_id = request.client_id;
    log_entry.topic_filter = request.topic_filter;

    if (MQ_FAIL(topic_tree_->unsubscribe(request.server_id, request.topic_filter, request.client_id))) {
    } else {
        ret = redo_log_manager_->append_log_entry(log_entry);
    }

    return ret;
}

int MQTTRouterRpcHandler::handle_client_connect(const ClientConnectRequest& request)
{
    int ret = MQ_SUCCESS;
    RouterLogEntry log_entry(allocator_);

    log_entry.op_type = RouterLogOpType::CLIENT_CONNECT;
    log_entry.server_id = request.server_id;
    log_entry.client_id = request.client_id;
    ret = redo_log_manager_->append_log_entry(log_entry);

    return ret;
}

int MQTTRouterRpcHandler::handle_client_disconnect(const ClientDisconnectRequest& request)
{
    int ret = MQ_SUCCESS;
    RouterLogEntry log_entry(allocator_);

    log_entry.op_type = RouterLogOpType::CLIENT_DISCONNECT;
    log_entry.server_id = request.server_id;
    log_entry.client_id = request.client_id;

    if (MQ_FAIL(topic_tree_->unsubscribe_all(request.server_id, request.client_id))) {
    } else {
        ret = redo_log_manager_->append_log_entry(log_entry);
    }

    return ret;
}

int MQTTRouterRpcHandler::handle_publish_route(const PublishRequest& request, RouteResponse& response)
{
    int ret = MQ_SUCCESS;
    RouteTargetVector available_targets{MQTTSTLAllocator<RouteTarget>(allocator_)};

    response.targets.clear();
    if (MQ_FAIL(topic_tree_->find_route_targets(request.topic, request.publisher_server_id, response.targets))) {
    } else {
        for (size_t i = 0; MQ_SUCCESS == ret && i < response.targets.size(); ++i) {
            ServerInfo server_info(allocator_);
            int node_ret = node_registry_->get_node(response.targets[i].server_id, server_info);
            if (MQ_SUCCESS == node_ret) {
                response.targets[i].host = server_info.host;
                response.targets[i].port = server_info.port;
                response.targets[i].forwarding_port = server_info.forwarding_port;
                available_targets.push_back(response.targets[i]);
            } else if (MQ_ERR_ROUTER_NODE_INACTIVE == node_ret || MQ_ERR_NOT_FOUND_V2 == node_ret) {
            } else {
                ret = node_ret;
            }
        }
        if (MQ_SUCCESS == ret) {
            response.targets.swap(available_targets);
        }
        if (MQ_SUCCESS == ret && response.targets.empty()) {
            ret = MQ_ERR_ROUTER_ROUTE_EMPTY;
        }
    }
    response.error_code = ret;

    return ret;
}

int MQTTRouterRpcHandler::handle_authenticate_node(const MQTTRouterRpcClient::NodeAuthRequest& request,
                                                   MQTTRouterRpcClient::NodeAuthResponse& response)
{
    int ret = MQ_SUCCESS;
    ServerInfo server_info(allocator_);
    uint64_t now_ms = 0;

    if (MQ_FAIL(auth_store_->validate_node_token(request, response.session_id))) {
        response.error_code = ret;
    } else {
        now_ms = current_time_ms();
        response.error_code = MQ_SUCCESS;
        ret = auth_store_->get_session_expires_at_ms(now_ms, response.expires_at_ms);
        server_info.server_id = request.server_id;
        server_info.host = request.host;
        server_info.port = request.port;
        server_info.forwarding_port = request.forwarding_port;
        server_info.is_active = true;
        server_info.last_heartbeat_ms = now_ms;
        server_info.session_id = response.session_id;
        if (MQ_SUCCESS == ret) {
            ret = node_registry_->upsert_node(server_info);
        }
        response.error_code = ret;
    }

    return ret;
}

int MQTTRouterRpcHandler::handle_heartbeat(const ServerInfo& server_info)
{
    int ret = MQ_SUCCESS;
    ret = node_registry_->upsert_node(server_info);
    return ret;
}

MQTTRouterService::MQTTRouterService(const MQTTRouterConfig& config)
    : config_(config)
    , root_allocator_(NULL)
    , topic_tree_()
    , node_registry_()
    , auth_store_()
    , redo_log_manager_()
    , rpc_handler_()
    , worker_threads_()
    , snapshot_thread_()
    , should_stop_(false)
    , listen_fd_(-1)
{
    MQTTMemoryManager& memory_manager = MQTTMemoryManager::get_instance();
    root_allocator_ = memory_manager.get_root_allocator()->create_child(
        "mqtt_router_service", MQTTMemoryTag::MEM_TAG_ROOT, config_.max_memory_limit);
}

MQTTRouterService::~MQTTRouterService()
{
    (void)stop();
    if (root_allocator_ != NULL && root_allocator_->get_parent() != NULL) {
        root_allocator_->get_parent()->remove_child("mqtt_router_service");
    }
}

int MQTTRouterService::initialize()
{
    int ret = MQ_SUCCESS;
    MQTTAllocator* topic_tree_allocator = NULL;
    MQTTAllocator* node_registry_allocator = NULL;
    MQTTAllocator* auth_store_allocator = NULL;
    MQTTAllocator* redo_log_allocator = NULL;
    MQTTAllocator* rpc_handler_allocator = NULL;

    topic_tree_allocator = root_allocator_->create_child("topic_tree", MQTTMemoryTag::MEM_TAG_TOPIC_TREE);
    node_registry_allocator = root_allocator_->create_child("node_registry", MQTTMemoryTag::MEM_TAG_ROOT);
    auth_store_allocator = root_allocator_->create_child("auth_store", MQTTMemoryTag::MEM_TAG_ROOT);
    redo_log_allocator = root_allocator_->create_child("redo_log", MQTTMemoryTag::MEM_TAG_ROOT);
    rpc_handler_allocator = root_allocator_->create_child("rpc_handler", MQTTMemoryTag::MEM_TAG_ROOT);

    topic_tree_.reset(new MQTTPersistentTopicTree(topic_tree_allocator));
    node_registry_.reset(new RouterNodeRegistry(node_registry_allocator));
    auth_store_.reset(new RouterAuthStore(auth_store_allocator));
    redo_log_manager_.reset(new MQTTRedoLogManager(redo_log_allocator, config_));
    rpc_handler_.reset(new MQTTRouterRpcHandler(topic_tree_.get(),
                                                node_registry_.get(),
                                                auth_store_.get(),
                                                redo_log_manager_.get(),
                                                rpc_handler_allocator));

    if (MQ_FAIL(node_registry_->initialize(config_.node_session_ttl_ms))) {
    } else if (MQ_FAIL(auth_store_->initialize(config_))) {
    } else if (MQ_FAIL(topic_tree_->load_from_snapshot(config_.snapshot_path))) {
    } else if (MQ_FAIL(redo_log_manager_->initialize())) {
    } else if (MQ_FAIL(redo_log_manager_->load_and_replay(topic_tree_.get()))) {
    }

    return ret;
}

int MQTTRouterService::start()
{
    int ret = MQ_SUCCESS;
    int opt = 1;
    struct sockaddr_in addr;

    should_stop_.store(false);
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        ret = MQ_ERR_SOCKET_ALLOC;
    } else if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ret = MQ_ERR_SOCKET_BIND;
    } else {
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(config_.service_host.c_str());
        addr.sin_port = htons(static_cast<uint16_t>(config_.service_port));

        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ret = MQ_ERR_SOCKET_BIND;
        } else if (::listen(listen_fd_, 1024) < 0) {
            ret = MQ_ERR_SOCKET_LISTEN;
        } else {
            for (int i = 0; MQ_SUCCESS == ret && i < config_.worker_thread_count; ++i) {
                worker_threads_.push_back(std::thread(&MQTTRouterService::worker_thread_func, this, i));
            }
            if (MQ_SUCCESS == ret) {
                snapshot_thread_ = std::thread(&MQTTRouterService::snapshot_thread_func, this);
            }
        }
    }

    return ret;
}

int MQTTRouterService::stop()
{
    int ret = MQ_SUCCESS;

    should_stop_.store(true);
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    for (size_t i = 0; i < worker_threads_.size(); ++i) {
        if (worker_threads_[i].joinable()) {
            worker_threads_[i].join();
        }
    }
    worker_threads_.clear();

    if (snapshot_thread_.joinable()) {
        snapshot_thread_.join();
    }

    return ret;
}

void MQTTRouterService::worker_thread_func(int thread_id)
{
    MQTTAllocator* thread_allocator = root_allocator_->create_child(
        "worker_thread_" + std::to_string(thread_id), MQTTMemoryTag::MEM_TAG_ROOT);
    thread_local_allocator_ = thread_allocator;

    while (!should_stop_.load()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);

        if (client_fd < 0) {
            if (!should_stop_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } else {
            handle_client_connection(client_fd);
        }
    }
}

void MQTTRouterService::snapshot_thread_func()
{
    while (!should_stop_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(config_.snapshot_interval_seconds));
        if (!should_stop_.load()) {
            (void)topic_tree_->save_to_snapshot(config_.snapshot_path);
            (void)redo_log_manager_->flush_to_disk();
        }
    }
}

void MQTTRouterService::handle_client_connection(int client_fd)
{
    ClientContext* context = new ClientContext(this, client_fd, 0, thread_local_allocator_);
    co_create(&context->coroutine, NULL, client_coroutine_func, context);
    co_resume(context->coroutine);
}

void* MQTTRouterService::client_coroutine_func(void* arg)
{
    ClientContext* context = static_cast<ClientContext*>(arg);
    int ret = MQ_SUCCESS;

    while (MQ_SUCCESS == ret) {
        ::mqtt::router::RouterEnvelope request_envelope;
        ::mqtt::router::RouterEnvelope response_envelope;
        MQTTRouterRpcHandler::SubscribeRequest subscribe_request(context->allocator);
        MQTTRouterRpcHandler::UnsubscribeRequest unsubscribe_request(context->allocator);
        MQTTRouterRpcHandler::ClientConnectRequest client_connect_request(context->allocator);
        MQTTRouterRpcHandler::ClientDisconnectRequest client_disconnect_request(context->allocator);
        MQTTRouterRpcHandler::PublishRequest publish_request(context->allocator);
        MQTTRouterRpcHandler::RouteResponse route_response(context->allocator);
        MQTTRouterRpcClient::NodeAuthRequest auth_request(context->allocator);
        MQTTRouterRpcClient::NodeAuthResponse auth_response(context->allocator);
        ServerInfo heartbeat_server_info(context->allocator);
        int request_ret = MQ_SUCCESS;

        ret = read_router_envelope(context->client_fd, request_envelope);
        if (MQ_SUCCESS == ret) {
            response_envelope.set_request_id(request_envelope.request_id());
            switch (request_envelope.body_case()) {
                case ::mqtt::router::RouterEnvelope::kAuthenticateNodeRequest:
                    auth_request.server_id = to_mqtt_string(request_envelope.authenticate_node_request().server_id(),
                                                            context->allocator);
                    auth_request.token = to_mqtt_string(request_envelope.authenticate_node_request().token(),
                                                        context->allocator);
                    auth_request.timestamp_ms = request_envelope.authenticate_node_request().timestamp_ms();
                    auth_request.nonce = to_mqtt_string(request_envelope.authenticate_node_request().nonce(),
                                                        context->allocator);
                    auth_request.host = to_mqtt_string(request_envelope.authenticate_node_request().host(),
                                                       context->allocator);
                    auth_request.port = static_cast<int>(request_envelope.authenticate_node_request().port());
                    auth_request.forwarding_port =
                        static_cast<int>(request_envelope.authenticate_node_request().forwarding_port());
                    request_ret = context->service->rpc_handler_->handle_authenticate_node(auth_request, auth_response);
                    response_envelope.mutable_authenticate_node_response()->set_error_code(request_ret);
                    response_envelope.mutable_authenticate_node_response()->set_session_id(
                        from_mqtt_string(auth_response.session_id));
                    response_envelope.mutable_authenticate_node_response()->set_expires_at_ms(
                        auth_response.expires_at_ms);
                    if (MQ_SUCCESS == request_ret) {
                        context->connection_ctx.authenticated = true;
                        context->connection_ctx.authenticated_server_id = auth_request.server_id;
                        context->connection_ctx.session_id = auth_response.session_id;
                    }
                    break;
                case ::mqtt::router::RouterEnvelope::kSubscribeRequest:
                    subscribe_request.server_id =
                        to_mqtt_string(request_envelope.subscribe_request().server_id(), context->allocator);
                    subscribe_request.client_id =
                        to_mqtt_string(request_envelope.subscribe_request().client_id(), context->allocator);
                    subscribe_request.topic_filter =
                        to_mqtt_string(request_envelope.subscribe_request().topic_filter(), context->allocator);
                    subscribe_request.qos = static_cast<uint8_t>(request_envelope.subscribe_request().qos());
                    request_ret = context->service->auth_store_->validate_session_request(
                        context->connection_ctx.authenticated_server_id, subscribe_request.server_id);
                    if (MQ_SUCCESS == request_ret) {
                        request_ret = context->service->rpc_handler_->handle_subscribe(subscribe_request);
                    }
                    response_envelope.mutable_subscribe_response()->set_error_code(request_ret);
                    break;
                case ::mqtt::router::RouterEnvelope::kUnsubscribeRequest:
                    unsubscribe_request.server_id =
                        to_mqtt_string(request_envelope.unsubscribe_request().server_id(), context->allocator);
                    unsubscribe_request.client_id =
                        to_mqtt_string(request_envelope.unsubscribe_request().client_id(), context->allocator);
                    unsubscribe_request.topic_filter =
                        to_mqtt_string(request_envelope.unsubscribe_request().topic_filter(), context->allocator);
                    request_ret = context->service->auth_store_->validate_session_request(
                        context->connection_ctx.authenticated_server_id, unsubscribe_request.server_id);
                    if (MQ_SUCCESS == request_ret) {
                        request_ret = context->service->rpc_handler_->handle_unsubscribe(unsubscribe_request);
                    }
                    response_envelope.mutable_unsubscribe_response()->set_error_code(request_ret);
                    break;
                case ::mqtt::router::RouterEnvelope::kClientConnectRequest:
                    client_connect_request.server_id =
                        to_mqtt_string(request_envelope.client_connect_request().server_id(), context->allocator);
                    client_connect_request.client_id =
                        to_mqtt_string(request_envelope.client_connect_request().client_id(), context->allocator);
                    request_ret = context->service->auth_store_->validate_session_request(
                        context->connection_ctx.authenticated_server_id, client_connect_request.server_id);
                    if (MQ_SUCCESS == request_ret) {
                        request_ret = context->service->rpc_handler_->handle_client_connect(client_connect_request);
                    }
                    response_envelope.mutable_client_connect_response()->set_error_code(request_ret);
                    break;
                case ::mqtt::router::RouterEnvelope::kClientDisconnectRequest:
                    client_disconnect_request.server_id =
                        to_mqtt_string(request_envelope.client_disconnect_request().server_id(), context->allocator);
                    client_disconnect_request.client_id =
                        to_mqtt_string(request_envelope.client_disconnect_request().client_id(), context->allocator);
                    request_ret = context->service->auth_store_->validate_session_request(
                        context->connection_ctx.authenticated_server_id, client_disconnect_request.server_id);
                    if (MQ_SUCCESS == request_ret) {
                        request_ret =
                            context->service->rpc_handler_->handle_client_disconnect(client_disconnect_request);
                    }
                    response_envelope.mutable_client_disconnect_response()->set_error_code(request_ret);
                    break;
                case ::mqtt::router::RouterEnvelope::kRoutePublishRequest:
                    publish_request.topic =
                        to_mqtt_string(request_envelope.route_publish_request().topic(), context->allocator);
                    publish_request.qos =
                        static_cast<uint8_t>(request_envelope.route_publish_request().qos());
                    publish_request.retain = request_envelope.route_publish_request().retain();
                    publish_request.publisher_server_id = to_mqtt_string(
                        request_envelope.route_publish_request().publisher_server_id(), context->allocator);
                    publish_request.publisher_client_id = to_mqtt_string(
                        request_envelope.route_publish_request().publisher_client_id(), context->allocator);
                    publish_request.message_id = request_envelope.route_publish_request().message_id();
                    publish_request.trace_id = to_mqtt_string(
                        request_envelope.route_publish_request().trace_id(), context->allocator);
                    if (!request_envelope.route_publish_request().payload().empty()) {
                        const std::string& payload = request_envelope.route_publish_request().payload();
                        publish_request.payload.assign(payload.begin(), payload.end());
                    }
                    request_ret = context->service->auth_store_->validate_session_request(
                        context->connection_ctx.authenticated_server_id, publish_request.publisher_server_id);
                    if (MQ_SUCCESS == request_ret) {
                        request_ret = context->service->rpc_handler_->handle_publish_route(publish_request,
                                                                                           route_response);
                    }
                    response_envelope.mutable_route_publish_response()->set_error_code(request_ret);
                    for (size_t i = 0; i < route_response.targets.size(); ++i) {
                        ::mqtt::router::RouteTarget* target =
                            response_envelope.mutable_route_publish_response()->add_targets();
                        target->set_server_id(from_mqtt_string(route_response.targets[i].server_id));
                        target->set_client_id(from_mqtt_string(route_response.targets[i].client_id));
                        target->set_host(from_mqtt_string(route_response.targets[i].host));
                        target->set_port(static_cast<uint32_t>(route_response.targets[i].port));
                        target->set_forwarding_port(
                            static_cast<uint32_t>(route_response.targets[i].forwarding_port));
                        target->set_qos(route_response.targets[i].qos);
                    }
                    break;
                case ::mqtt::router::RouterEnvelope::kGetStatisticsRequest: {
                    size_t total_servers = 0;
                    size_t total_clients = 0;
                    size_t total_subscriptions = 0;
                    request_ret = context->service->get_statistics(total_servers, total_clients,
                                                                   total_subscriptions);
                    response_envelope.mutable_get_statistics_response()->set_error_code(request_ret);
                    response_envelope.mutable_get_statistics_response()->set_total_servers(total_servers);
                    response_envelope.mutable_get_statistics_response()->set_total_clients(total_clients);
                    response_envelope.mutable_get_statistics_response()->set_total_subscriptions(
                        total_subscriptions);
                    response_envelope.mutable_get_statistics_response()->set_total_routes(0);
                    response_envelope.mutable_get_statistics_response()->set_memory_usage_bytes(0);
                    response_envelope.mutable_get_statistics_response()->set_redo_log_entries(
                        context->service->redo_log_manager_->get_pending_entries_count());
                    response_envelope.mutable_get_statistics_response()->set_last_snapshot_time(current_time_ms());
                    break;
                }
                case ::mqtt::router::RouterEnvelope::kHeartbeatRequest:
                    heartbeat_server_info.server_id =
                        to_mqtt_string(request_envelope.heartbeat_request().server_id(), context->allocator);
                    heartbeat_server_info.session_id =
                        to_mqtt_string(request_envelope.heartbeat_request().session_id(), context->allocator);
                    heartbeat_server_info.host =
                        to_mqtt_string(request_envelope.heartbeat_request().host(), context->allocator);
                    heartbeat_server_info.port =
                        static_cast<int>(request_envelope.heartbeat_request().port());
                    heartbeat_server_info.forwarding_port =
                        static_cast<int>(request_envelope.heartbeat_request().forwarding_port());
                    heartbeat_server_info.client_count =
                        request_envelope.heartbeat_request().client_count();
                    heartbeat_server_info.subscription_count =
                        request_envelope.heartbeat_request().subscription_count();
                    heartbeat_server_info.last_heartbeat_ms = current_time_ms();
                    heartbeat_server_info.is_active = true;
                    request_ret = context->service->auth_store_->validate_session_request(
                        context->connection_ctx.authenticated_server_id, heartbeat_server_info.server_id);
                    if (MQ_SUCCESS == request_ret) {
                        request_ret = context->service->rpc_handler_->handle_heartbeat(heartbeat_server_info);
                    }
                    response_envelope.mutable_heartbeat_response()->set_error_code(request_ret);
                    response_envelope.mutable_heartbeat_response()->set_current_time(current_time_ms());
                    break;
                case ::mqtt::router::RouterEnvelope::BODY_NOT_SET:
                default:
                    request_ret = MQ_ERR_ROUTER_PROTOCOL;
                    response_envelope.mutable_get_statistics_response()->set_error_code(request_ret);
                    break;
            }
            if (MQ_FAIL(write_router_envelope(context->client_fd, response_envelope))) {
            }
        }
    }

    if (context->client_fd >= 0) {
        ::close(context->client_fd);
        context->client_fd = -1;
    }
    delete context;

    return NULL;
}

int MQTTRouterService::get_statistics(size_t& total_servers,
                                      size_t& total_clients,
                                      size_t& total_subscriptions)
{
    int ret = MQ_SUCCESS;
    size_t ignored_server_count = 0;

    if (MQ_FAIL(node_registry_->get_node_count(total_servers))) {
    } else if (MQ_FAIL(topic_tree_->get_statistics(ignored_server_count, total_clients, total_subscriptions))) {
    }

    return ret;
}
