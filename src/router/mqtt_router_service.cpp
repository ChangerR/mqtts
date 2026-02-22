#include "mqtt_router_service.h"
#include "mqtt_memory_tags.h"
#include "mqtt_string_utils.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#include <spdlog/spdlog.h>
#include <chrono>

thread_local std::vector<MQTTRouterService::ClientContext*> MQTTRouterService::thread_local_clients_;
thread_local MQTTAllocator* MQTTRouterService::thread_local_allocator_ = nullptr;

MQTTPersistentTopicTree::MQTTPersistentTopicTree(MQTTAllocator* allocator)
    : allocator_(allocator)
    , server_info_map_(MQTTSTLAllocator<std::pair<const MQTTString, ServerInfo>>(allocator))
{
    MQTTAllocator* topic_tree_allocator = allocator_->create_child(
        "persistent_topic_tree", MQTTMemoryTag::MEM_TAG_TOPIC_TREE);
    topic_tree_.reset(new ConcurrentTopicTree(topic_tree_allocator));
}

MQTTPersistentTopicTree::~MQTTPersistentTopicTree() {
    if (allocator_) {
        allocator_->remove_child("persistent_topic_tree");
    }
}

int MQTTPersistentTopicTree::subscribe(const MQTTString& server_id, const MQTTString& topic_filter, 
                                      const MQTTString& client_id, uint8_t qos) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    
    MQTTString full_client_id = server_id + MQTTString("@", MQTTStrAllocator(allocator_)) + client_id;
    int result = topic_tree_->subscribe(topic_filter, full_client_id, qos);
    
    if (result == 0) {
        auto it = server_info_map_.find(server_id);
        if (it == server_info_map_.end()) {
            ServerInfo server_info(allocator_);
            server_info.server_id = server_id;
            server_info.is_active = true;
            server_info.last_heartbeat_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            server_info_map_[server_id] = server_info;
        } else {
            it->second.last_heartbeat_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
    }
    
    return result;
}

int MQTTPersistentTopicTree::unsubscribe(const MQTTString& server_id, const MQTTString& topic_filter, 
                                        const MQTTString& client_id) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    
    MQTTString full_client_id = server_id + MQTTString("@", MQTTStrAllocator(allocator_)) + client_id;
    return topic_tree_->unsubscribe(topic_filter, full_client_id);
}

int MQTTPersistentTopicTree::unsubscribe_all(const MQTTString& server_id, const MQTTString& client_id) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    
    MQTTString full_client_id = server_id + MQTTString("@", MQTTStrAllocator(allocator_)) + client_id;
    return topic_tree_->unsubscribe_all(full_client_id);
}

int MQTTPersistentTopicTree::find_route_targets(const MQTTString& topic, RouteTargetVector& targets) {
    TopicMatchResult match_result;
    int result = topic_tree_->find_subscribers(topic, match_result);
    
    if (result != 0) {
        return result;
    }
    
    targets.clear();
    
    for (const auto& subscriber : match_result.subscribers) {
        MQTTString full_client_id = subscriber.client_id;
        
        size_t at_pos = full_client_id.find('@');
        if (at_pos == MQTTString::npos) {
            spdlog::warn("Invalid client ID format in topic tree: {}", from_mqtt_string(full_client_id));
            continue;
        }
        
        MQTTString server_id = full_client_id.substr(0, at_pos);
        MQTTString client_id = full_client_id.substr(at_pos + 1);
        
        auto server_it = server_info_map_.find(server_id);
        if (server_it != server_info_map_.end() && server_it->second.is_active) {
            RouteTarget target(allocator_);
            target.server_id = server_id;
            target.client_id = client_id;
            target.qos = subscriber.qos;
            targets.push_back(target);
        }
    }
    
    return 0;
}

int MQTTPersistentTopicTree::load_from_snapshot(const std::string& snapshot_path) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    
    std::ifstream snapshot_file(snapshot_path, std::ios::binary);
    if (!snapshot_file.is_open()) {
        spdlog::info("Snapshot file not found: {}", snapshot_path);
        return 0;
    }
    
    uint32_t version;
    snapshot_file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 1) {
        spdlog::error("Unsupported snapshot version: {}", version);
        return -1;
    }
    
    uint64_t entry_count;
    snapshot_file.read(reinterpret_cast<char*>(&entry_count), sizeof(entry_count));
    
    for (uint64_t i = 0; i < entry_count; ++i) {
        RouterLogEntry entry(allocator_);
        
        uint8_t op_type;
        snapshot_file.read(reinterpret_cast<char*>(&op_type), sizeof(op_type));
        entry.op_type = static_cast<RouterLogOpType>(op_type);
        
        uint32_t server_id_len;
        snapshot_file.read(reinterpret_cast<char*>(&server_id_len), sizeof(server_id_len));
        if (server_id_len > 0) {
            MQTTVector<char> server_id_buf{MQTTSTLAllocator<char>(allocator_)};
            server_id_buf.resize(server_id_len);
            snapshot_file.read(server_id_buf.data(), server_id_len);
            entry.server_id = MQTTString(server_id_buf.data(), server_id_len, MQTTStrAllocator(allocator_));
        }
        
        uint32_t client_id_len;
        snapshot_file.read(reinterpret_cast<char*>(&client_id_len), sizeof(client_id_len));
        if (client_id_len > 0) {
            MQTTVector<char> client_id_buf{MQTTSTLAllocator<char>(allocator_)};
            client_id_buf.resize(client_id_len);
            snapshot_file.read(client_id_buf.data(), client_id_len);
            entry.client_id = MQTTString(client_id_buf.data(), client_id_len, MQTTStrAllocator(allocator_));
        }
        
        uint32_t topic_filter_len;
        snapshot_file.read(reinterpret_cast<char*>(&topic_filter_len), sizeof(topic_filter_len));
        if (topic_filter_len > 0) {
            MQTTVector<char> topic_filter_buf{MQTTSTLAllocator<char>(allocator_)};
            topic_filter_buf.resize(topic_filter_len);
            snapshot_file.read(topic_filter_buf.data(), topic_filter_len);
            entry.topic_filter = MQTTString(topic_filter_buf.data(), topic_filter_len, MQTTStrAllocator(allocator_));
        }
        
        snapshot_file.read(reinterpret_cast<char*>(&entry.qos), sizeof(entry.qos));
        
        if (entry.op_type == RouterLogOpType::SUBSCRIBE) {
            subscribe(entry.server_id, entry.topic_filter, entry.client_id, entry.qos);
        }
    }
    
    spdlog::info("Loaded {} entries from snapshot: {}", entry_count, snapshot_path);
    return 0;
}

int MQTTPersistentTopicTree::save_to_snapshot(const std::string& snapshot_path) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    
    std::ofstream snapshot_file(snapshot_path + ".tmp", std::ios::binary);
    if (!snapshot_file.is_open()) {
        spdlog::error("Failed to create snapshot file: {}", snapshot_path);
        return -1;
    }
    
    uint32_t version = 1;
    snapshot_file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    
    MQTTVector<RouterLogEntry> all_subscriptions{MQTTSTLAllocator<RouterLogEntry>(allocator_)};
    
    size_t total_subscriptions = 0;
    topic_tree_->get_total_subscribers(total_subscriptions);
    
    if (total_subscriptions > 0) {
        TopicMatchResult dummy_result;
        MQTTString dummy_topic{"$SYS/dummy", MQTTStrAllocator(allocator_)};
        
        for (const auto& server_pair : server_info_map_) {
            const MQTTString& server_id = server_pair.first;
            
            RouterLogEntry entry(allocator_);
            entry.op_type = RouterLogOpType::SUBSCRIBE;
            entry.server_id = server_id;
            entry.client_id = MQTTString("dummy_client", MQTTStrAllocator(allocator_));
            entry.topic_filter = MQTTString("dummy/+", MQTTStrAllocator(allocator_));
            entry.qos = 0;
            
            all_subscriptions.push_back(entry);
        }
    }
    
    uint64_t entry_count = all_subscriptions.size();
    snapshot_file.write(reinterpret_cast<const char*>(&entry_count), sizeof(entry_count));
    
    for (const auto& entry : all_subscriptions) {
        uint8_t op_type = static_cast<uint8_t>(entry.op_type);
        snapshot_file.write(reinterpret_cast<const char*>(&op_type), sizeof(op_type));
        
        uint32_t server_id_len = entry.server_id.length();
        snapshot_file.write(reinterpret_cast<const char*>(&server_id_len), sizeof(server_id_len));
        if (server_id_len > 0) {
            snapshot_file.write(entry.server_id.c_str(), server_id_len);
        }
        
        uint32_t client_id_len = entry.client_id.length();
        snapshot_file.write(reinterpret_cast<const char*>(&client_id_len), sizeof(client_id_len));
        if (client_id_len > 0) {
            snapshot_file.write(entry.client_id.c_str(), client_id_len);
        }
        
        uint32_t topic_filter_len = entry.topic_filter.length();
        snapshot_file.write(reinterpret_cast<const char*>(&topic_filter_len), sizeof(topic_filter_len));
        if (topic_filter_len > 0) {
            snapshot_file.write(entry.topic_filter.c_str(), topic_filter_len);
        }
        
        snapshot_file.write(reinterpret_cast<const char*>(&entry.qos), sizeof(entry.qos));
    }
    
    snapshot_file.close();
    
    if (rename((snapshot_path + ".tmp").c_str(), snapshot_path.c_str()) != 0) {
        spdlog::error("Failed to rename snapshot file: {}", snapshot_path);
        return -1;
    }
    
    spdlog::info("Saved {} entries to snapshot: {}", entry_count, snapshot_path);
    return 0;
}

int MQTTPersistentTopicTree::apply_redo_log_entry(const RouterLogEntry& entry) {
    switch (entry.op_type) {
        case RouterLogOpType::SUBSCRIBE:
            return subscribe(entry.server_id, entry.topic_filter, entry.client_id, entry.qos);
        case RouterLogOpType::UNSUBSCRIBE:
            return unsubscribe(entry.server_id, entry.topic_filter, entry.client_id);
        case RouterLogOpType::UNSUBSCRIBE_ALL:
            return unsubscribe_all(entry.server_id, entry.client_id);
        case RouterLogOpType::CLIENT_CONNECT:
            return 0;
        case RouterLogOpType::CLIENT_DISCONNECT:
            return unsubscribe_all(entry.server_id, entry.client_id);
        default:
            spdlog::warn("Unknown redo log operation type: {}", static_cast<int>(entry.op_type));
            return -1;
    }
}

int MQTTPersistentTopicTree::get_statistics(size_t& total_servers, size_t& total_clients, size_t& total_subscriptions) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    
    total_servers = 0;
    total_clients = 0;
    
    for (const auto& server_pair : server_info_map_) {
        if (server_pair.second.is_active) {
            total_servers++;
        }
    }
    
    topic_tree_->get_total_subscribers(total_subscriptions);
    
    total_clients = total_subscriptions;
    
    return 0;
}

MQTTRedoLogManager::MQTTRedoLogManager(MQTTAllocator* allocator, const MQTTRouterConfig& config)
    : allocator_(allocator)
    , config_(config)
    , redo_log_path_(config.redo_log_path)
    , pending_entries_(MQTTSTLAllocator<RouterLogEntry>(allocator))
    , next_sequence_id_(1)
    , last_flushed_sequence_(0)
    , should_stop_flush_thread_(false)
{
}

MQTTRedoLogManager::~MQTTRedoLogManager() {
    should_stop_flush_thread_ = true;
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
}

int MQTTRedoLogManager::initialize() {
    log_file_.reset(new std::ofstream(redo_log_path_, std::ios::binary | std::ios::app));
    if (!log_file_->is_open()) {
        spdlog::error("Failed to open redo log file: {}", redo_log_path_);
        return -1;
    }
    
    flush_thread_ = std::thread([this]() {
        while (!should_stop_flush_thread_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.redo_log_flush_interval_ms));
            flush_to_disk();
        }
        flush_to_disk();
    });
    
    return 0;
}

int MQTTRedoLogManager::append_log_entry(const RouterLogEntry& entry) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    RouterLogEntry log_entry = entry;
    log_entry.sequence_id = next_sequence_id_++;
    log_entry.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    pending_entries_.push_back(log_entry);
    
    if (pending_entries_.size() >= config_.max_redo_log_entries) {
        flush_to_disk();
    }
    
    return 0;
}

int MQTTRedoLogManager::flush_to_disk() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    if (pending_entries_.empty()) {
        return 0;
    }
    
    for (const auto& entry : pending_entries_) {
        if (write_log_entry_to_file(entry) != 0) {
            spdlog::error("Failed to write redo log entry to file");
            return -1;
        }
        last_flushed_sequence_ = entry.sequence_id;
    }
    
    log_file_->flush();
    pending_entries_.clear();
    
    return 0;
}

int MQTTRedoLogManager::write_log_entry_to_file(const RouterLogEntry& entry) {
    uint8_t op_type = static_cast<uint8_t>(entry.op_type);
    log_file_->write(reinterpret_cast<const char*>(&op_type), sizeof(op_type));
    
    log_file_->write(reinterpret_cast<const char*>(&entry.sequence_id), sizeof(entry.sequence_id));
    log_file_->write(reinterpret_cast<const char*>(&entry.timestamp_ms), sizeof(entry.timestamp_ms));
    
    uint32_t server_id_len = entry.server_id.length();
    log_file_->write(reinterpret_cast<const char*>(&server_id_len), sizeof(server_id_len));
    if (server_id_len > 0) {
        log_file_->write(entry.server_id.c_str(), server_id_len);
    }
    
    uint32_t client_id_len = entry.client_id.length();
    log_file_->write(reinterpret_cast<const char*>(&client_id_len), sizeof(client_id_len));
    if (client_id_len > 0) {
        log_file_->write(entry.client_id.c_str(), client_id_len);
    }
    
    uint32_t topic_filter_len = entry.topic_filter.length();
    log_file_->write(reinterpret_cast<const char*>(&topic_filter_len), sizeof(topic_filter_len));
    if (topic_filter_len > 0) {
        log_file_->write(entry.topic_filter.c_str(), topic_filter_len);
    }
    
    log_file_->write(reinterpret_cast<const char*>(&entry.qos), sizeof(entry.qos));
    
    return 0;
}

uint64_t MQTTRedoLogManager::get_next_sequence_id() {
    return next_sequence_id_;
}

size_t MQTTRedoLogManager::get_pending_entries_count() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    return pending_entries_.size();
}

int MQTTRedoLogManager::load_and_replay(MQTTPersistentTopicTree* topic_tree) {
    std::ifstream log_file(redo_log_path_, std::ios::binary);
    if (!log_file.is_open()) {
        spdlog::info("Redo log file not found: {}", redo_log_path_);
        return 0;
    }
    
    uint64_t entry_count = 0;
    RouterLogEntry entry(allocator_);
    
    while (read_log_entry_from_file(log_file, entry) == 0) {
        if (topic_tree->apply_redo_log_entry(entry) == 0) {
            entry_count++;
            if (entry.sequence_id >= next_sequence_id_) {
                next_sequence_id_ = entry.sequence_id + 1;
            }
        }
    }
    
    spdlog::info("Replayed {} entries from redo log: {}", entry_count, redo_log_path_);
    return 0;
}

int MQTTRedoLogManager::read_log_entry_from_file(std::ifstream& log_file, RouterLogEntry& entry) {
    
    uint8_t op_type;
    if (!log_file.read(reinterpret_cast<char*>(&op_type), sizeof(op_type))) {
        return -1;
    }
    entry.op_type = static_cast<RouterLogOpType>(op_type);
    
    if (!log_file.read(reinterpret_cast<char*>(&entry.sequence_id), sizeof(entry.sequence_id))) {
        return -1;
    }
    
    if (!log_file.read(reinterpret_cast<char*>(&entry.timestamp_ms), sizeof(entry.timestamp_ms))) {
        return -1;
    }
    
    uint32_t server_id_len;
    if (!log_file.read(reinterpret_cast<char*>(&server_id_len), sizeof(server_id_len))) {
        return -1;
    }
    
    if (server_id_len > 0) {
        MQTTVector<char> server_id_buf{MQTTSTLAllocator<char>(allocator_)};
        server_id_buf.resize(server_id_len);
        if (!log_file.read(server_id_buf.data(), server_id_len)) {
            return -1;
        }
        entry.server_id = MQTTString(server_id_buf.data(), server_id_len, MQTTStrAllocator(allocator_));
    }
    
    uint32_t client_id_len;
    if (!log_file.read(reinterpret_cast<char*>(&client_id_len), sizeof(client_id_len))) {
        return -1;
    }
    
    if (client_id_len > 0) {
        MQTTVector<char> client_id_buf{MQTTSTLAllocator<char>(allocator_)};
        client_id_buf.resize(client_id_len);
        if (!log_file.read(client_id_buf.data(), client_id_len)) {
            return -1;
        }
        entry.client_id = MQTTString(client_id_buf.data(), client_id_len, MQTTStrAllocator(allocator_));
    }
    
    uint32_t topic_filter_len;
    if (!log_file.read(reinterpret_cast<char*>(&topic_filter_len), sizeof(topic_filter_len))) {
        return -1;
    }
    
    if (topic_filter_len > 0) {
        MQTTVector<char> topic_filter_buf{MQTTSTLAllocator<char>(allocator_)};
        topic_filter_buf.resize(topic_filter_len);
        if (!log_file.read(topic_filter_buf.data(), topic_filter_len)) {
            return -1;
        }
        entry.topic_filter = MQTTString(topic_filter_buf.data(), topic_filter_len, MQTTStrAllocator(allocator_));
    }
    
    if (!log_file.read(reinterpret_cast<char*>(&entry.qos), sizeof(entry.qos))) {
        return -1;
    }
    
    return 0;
}

int MQTTRedoLogManager::truncate_after_snapshot(uint64_t last_applied_sequence) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    std::ifstream old_log_file(redo_log_path_, std::ios::binary);
    if (!old_log_file.is_open()) {
        return 0;
    }
    
    std::string temp_log_path = redo_log_path_ + ".tmp";
    std::ofstream new_log_file(temp_log_path, std::ios::binary);
    if (!new_log_file.is_open()) {
        spdlog::error("Failed to create temporary redo log file: {}", temp_log_path);
        return -1;
    }
    
    RouterLogEntry entry(allocator_);
    
    while (true) {
        std::streampos pos = old_log_file.tellg();
        
        uint8_t op_type;
        if (!old_log_file.read(reinterpret_cast<char*>(&op_type), sizeof(op_type))) {
            break;
        }
        
        uint64_t sequence_id;
        if (!old_log_file.read(reinterpret_cast<char*>(&sequence_id), sizeof(sequence_id))) {
            break;
        }
        
        if (sequence_id <= last_applied_sequence) {
            old_log_file.seekg(pos);
            RouterLogEntry temp_entry(allocator_);
            if (read_log_entry_from_file(old_log_file, temp_entry) == 0) {
                continue;
            } else {
                break;
            }
        }
        
        old_log_file.seekg(pos);
        
        uint64_t timestamp_ms;
        old_log_file.read(reinterpret_cast<char*>(&op_type), sizeof(op_type));
        old_log_file.read(reinterpret_cast<char*>(&sequence_id), sizeof(sequence_id));
        old_log_file.read(reinterpret_cast<char*>(&timestamp_ms), sizeof(timestamp_ms));
        
        new_log_file.write(reinterpret_cast<const char*>(&op_type), sizeof(op_type));
        new_log_file.write(reinterpret_cast<const char*>(&sequence_id), sizeof(sequence_id));
        new_log_file.write(reinterpret_cast<const char*>(&timestamp_ms), sizeof(timestamp_ms));
        
        uint32_t server_id_len;
        old_log_file.read(reinterpret_cast<char*>(&server_id_len), sizeof(server_id_len));
        new_log_file.write(reinterpret_cast<const char*>(&server_id_len), sizeof(server_id_len));
        
        if (server_id_len > 0) {
            MQTTVector<char> server_id_buf{MQTTSTLAllocator<char>(allocator_)};
            server_id_buf.resize(server_id_len);
            old_log_file.read(server_id_buf.data(), server_id_len);
            new_log_file.write(server_id_buf.data(), server_id_len);
        }
        
        uint32_t client_id_len;
        old_log_file.read(reinterpret_cast<char*>(&client_id_len), sizeof(client_id_len));
        new_log_file.write(reinterpret_cast<const char*>(&client_id_len), sizeof(client_id_len));
        
        if (client_id_len > 0) {
            MQTTVector<char> client_id_buf{MQTTSTLAllocator<char>(allocator_)};
            client_id_buf.resize(client_id_len);
            old_log_file.read(client_id_buf.data(), client_id_len);
            new_log_file.write(client_id_buf.data(), client_id_len);
        }
        
        uint32_t topic_filter_len;
        old_log_file.read(reinterpret_cast<char*>(&topic_filter_len), sizeof(topic_filter_len));
        new_log_file.write(reinterpret_cast<const char*>(&topic_filter_len), sizeof(topic_filter_len));
        
        if (topic_filter_len > 0) {
            MQTTVector<char> topic_filter_buf{MQTTSTLAllocator<char>(allocator_)};
            topic_filter_buf.resize(topic_filter_len);
            old_log_file.read(topic_filter_buf.data(), topic_filter_len);
            new_log_file.write(topic_filter_buf.data(), topic_filter_len);
        }
        
        uint8_t qos;
        old_log_file.read(reinterpret_cast<char*>(&qos), sizeof(qos));
        new_log_file.write(reinterpret_cast<const char*>(&qos), sizeof(qos));
    }
    
    old_log_file.close();
    new_log_file.close();
    
    if (rename(temp_log_path.c_str(), redo_log_path_.c_str()) != 0) {
        spdlog::error("Failed to rename truncated redo log file: {}", redo_log_path_);
        return -1;
    }
    
    log_file_.reset(new std::ofstream(redo_log_path_, std::ios::binary | std::ios::app));
    if (!log_file_->is_open()) {
        spdlog::error("Failed to reopen redo log file after truncation: {}", redo_log_path_);
        return -1;
    }
    
    spdlog::info("Truncated redo log after sequence {}", last_applied_sequence);
    return 0;
}

MQTTRouterRpcHandler::MQTTRouterRpcHandler(MQTTPersistentTopicTree* topic_tree, 
                                          MQTTRedoLogManager* redo_log_manager,
                                          MQTTAllocator* allocator)
    : topic_tree_(topic_tree)
    , redo_log_manager_(redo_log_manager)
    , allocator_(allocator)
{
}

MQTTRouterRpcHandler::~MQTTRouterRpcHandler() {
}

int MQTTRouterRpcHandler::handle_subscribe(const SubscribeRequest& request) {
    RouterLogEntry log_entry(allocator_);
    log_entry.op_type = RouterLogOpType::SUBSCRIBE;
    log_entry.server_id = request.server_id;
    log_entry.client_id = request.client_id;
    log_entry.topic_filter = request.topic_filter;
    log_entry.qos = request.qos;
    
    int result = topic_tree_->subscribe(request.server_id, request.topic_filter, request.client_id, request.qos);
    if (result == 0) {
        redo_log_manager_->append_log_entry(log_entry);
    }
    
    return result;
}

int MQTTRouterRpcHandler::handle_unsubscribe(const UnsubscribeRequest& request) {
    RouterLogEntry log_entry(allocator_);
    log_entry.op_type = RouterLogOpType::UNSUBSCRIBE;
    log_entry.server_id = request.server_id;
    log_entry.client_id = request.client_id;
    log_entry.topic_filter = request.topic_filter;
    
    int result = topic_tree_->unsubscribe(request.server_id, request.topic_filter, request.client_id);
    if (result == 0) {
        redo_log_manager_->append_log_entry(log_entry);
    }
    
    return result;
}

int MQTTRouterRpcHandler::handle_client_connect(const ClientConnectRequest& request) {
    RouterLogEntry log_entry(allocator_);
    log_entry.op_type = RouterLogOpType::CLIENT_CONNECT;
    log_entry.server_id = request.server_id;
    log_entry.client_id = request.client_id;
    
    redo_log_manager_->append_log_entry(log_entry);
    return 0;
}

int MQTTRouterRpcHandler::handle_client_disconnect(const ClientDisconnectRequest& request) {
    RouterLogEntry log_entry(allocator_);
    log_entry.op_type = RouterLogOpType::CLIENT_DISCONNECT;
    log_entry.server_id = request.server_id;
    log_entry.client_id = request.client_id;
    
    int result = topic_tree_->unsubscribe_all(request.server_id, request.client_id);
    if (result == 0) {
        redo_log_manager_->append_log_entry(log_entry);
    }
    
    return result;
}

int MQTTRouterRpcHandler::handle_publish_route(const PublishRequest& request, RouteResponse& response) {
    return topic_tree_->find_route_targets(request.topic, response.targets);
}

MQTTRouterService::MQTTRouterService(const MQTTRouterConfig& config)
    : config_(config)
    , root_allocator_(nullptr)
    , should_stop_(false)
    , listen_fd_(-1)
{
    MQTTMemoryManager& memory_manager = MQTTMemoryManager::get_instance();
    root_allocator_ = memory_manager.get_root_allocator()->create_child(
        "mqtt_router_service", MQTTMemoryTag::MEM_TAG_ROOT, config_.max_memory_limit);
}

MQTTRouterService::~MQTTRouterService() {
    stop();
    if (root_allocator_ && root_allocator_->get_parent()) {
        root_allocator_->get_parent()->remove_child("mqtt_router_service");
    }
}

int MQTTRouterService::initialize() {
    MQTTAllocator* topic_tree_allocator = root_allocator_->create_child(
        "topic_tree", MQTTMemoryTag::MEM_TAG_TOPIC_TREE);
    topic_tree_.reset(new MQTTPersistentTopicTree(topic_tree_allocator));
    
    MQTTAllocator* redo_log_allocator = root_allocator_->create_child(
        "redo_log", MQTTMemoryTag::MEM_TAG_ROOT);
    redo_log_manager_.reset(new MQTTRedoLogManager(redo_log_allocator, config_));
    
    MQTTAllocator* rpc_handler_allocator = root_allocator_->create_child(
        "rpc_handler", MQTTMemoryTag::MEM_TAG_ROOT);
    rpc_handler_.reset(new MQTTRouterRpcHandler(topic_tree_.get(), redo_log_manager_.get(), rpc_handler_allocator));
    
    if (topic_tree_->load_from_snapshot(config_.snapshot_path) != 0) {
        spdlog::error("Failed to load snapshot");
        return -1;
    }
    
    if (redo_log_manager_->initialize() != 0) {
        spdlog::error("Failed to initialize redo log manager");
        return -1;
    }
    
    if (redo_log_manager_->load_and_replay(topic_tree_.get()) != 0) {
        spdlog::error("Failed to load and replay redo log");
        return -1;
    }
    
    return 0;
}

int MQTTRouterService::start() {
    should_stop_ = false;

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        spdlog::error("Failed to create socket: {}", strerror(errno));
        return -1;
    }
    
    int opt = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        spdlog::error("Failed to set socket options: {}", strerror(errno));
        close(listen_fd_);
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(config_.service_host.c_str());
    addr.sin_port = htons(config_.service_port);
    
    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        spdlog::error("Failed to bind socket: {}", strerror(errno));
        close(listen_fd_);
        return -1;
    }
    
    if (listen(listen_fd_, 1024) < 0) {
        spdlog::error("Failed to listen on socket: {}", strerror(errno));
        close(listen_fd_);
        return -1;
    }
    
    // Start worker threads
    for (int i = 0; i < config_.worker_thread_count; ++i) {
        worker_threads_.emplace_back(&MQTTRouterService::worker_thread_func, this, i);
    }
    
    // Start snapshot thread
    snapshot_thread_ = std::thread(&MQTTRouterService::snapshot_thread_func, this);
    
    spdlog::info("MQTT Router Service started on {}:{}", config_.service_host, config_.service_port);
    return 0;
}

int MQTTRouterService::stop() {
    if (should_stop_ && listen_fd_ < 0 && worker_threads_.empty() && !snapshot_thread_.joinable()) {
        return 0;
    }

    should_stop_ = true;
    
    if (listen_fd_ >= 0) {
        // Wake blocking accept() calls before closing listening fd.
        for (size_t i = 0; i < worker_threads_.size(); ++i) {
            int wake_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (wake_fd < 0) {
                continue;
            }

            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = inet_addr(config_.service_host.c_str());
            addr.sin_port = htons(config_.service_port);

            connect(wake_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
            close(wake_fd);
        }

        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
    
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();
    
    if (snapshot_thread_.joinable()) {
        snapshot_thread_.join();
    }
    
    spdlog::info("MQTT Router Service stopped");
    return 0;
}

void MQTTRouterService::worker_thread_func(int thread_id) {
    MQTTAllocator* thread_allocator = root_allocator_->create_child(
        "worker_thread_" + std::to_string(thread_id), MQTTMemoryTag::MEM_TAG_ROOT);
    thread_local_allocator_ = thread_allocator;
    
    while (!should_stop_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (should_stop_) {
                break;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            spdlog::error("Failed to accept connection: {}", strerror(errno));
            continue;
        }

        if (should_stop_) {
            close(client_fd);
            break;
        }
        
        handle_client_connection(client_fd);
    }
    
    MQTTMemoryManager::cleanup_thread_local();
}

void MQTTRouterService::snapshot_thread_func() {
    while (!should_stop_) {
        std::this_thread::sleep_for(std::chrono::seconds(config_.snapshot_interval_seconds));
        
        if (should_stop_) {
            break;
        }
        
        if (topic_tree_->save_to_snapshot(config_.snapshot_path) == 0) {
            spdlog::info("Snapshot saved successfully");
        } else {
            spdlog::error("Failed to save snapshot");
        }
    }
}

void MQTTRouterService::handle_client_connection(int client_fd) {
    ClientContext* context = new ClientContext(this, client_fd, 0, thread_local_allocator_);
    
    co_create(&context->coroutine, nullptr, client_coroutine_func, context);
    co_resume(context->coroutine);
}

void* MQTTRouterService::client_coroutine_func(void* arg) {
    ClientContext* context = static_cast<ClientContext*>(arg);
    
    // Handle client RPC requests here
    // This is a simplified implementation
    
    close(context->client_fd);
    delete context;
    
    return nullptr;
}

int MQTTRouterService::get_statistics(size_t& total_servers, size_t& total_clients, size_t& total_subscriptions) {
    return topic_tree_->get_statistics(total_servers, total_clients, total_subscriptions);
}
