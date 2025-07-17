#include "websocket_server.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include "co_routine.h"
#include "logger.h"
#include "mqtt_session_manager_v2.h"

namespace websocket {

WebSocketServer::WebSocketServer(const WebSocketConfig& config, const mqtt::MemoryConfig& memory_config)
    : config_(config)
    , memory_config_(memory_config)
    , accept_co_(nullptr)
    , server_socket_(nullptr)
    , running_(false)
    , current_connections_(0)
    , session_manager_(nullptr)
    , server_allocator_(nullptr) {
}

WebSocketServer::~WebSocketServer() {
    stop();
    cleanup_resources();
}

int WebSocketServer::start() {
    if (running_) {
        return 0;
    }
    
    // Initialize server allocator
    server_allocator_ = new MQTTAllocator("websocket_server", MQTTMemoryTag::MEM_TAG_SERVER, 0);
    if (!server_allocator_) {
        LOG_ERROR("Failed to create server allocator");
        return -1;
    }
    
    // Initialize server socket
    int ret = initialize_server_socket();
    if (ret != 0) {
        LOG_ERROR("Failed to initialize server socket: {}", ret);
        return ret;
    }
    
    // Create accept coroutine
    ret = co_create(&accept_co_, nullptr, accept_routine, this);
    if (ret != 0) {
        LOG_ERROR("Failed to create accept coroutine: {}", ret);
        return -1;
    }
    
    running_ = true;
    
    LOG_INFO("WebSocket server started on {}:{}", config_.bind_address, config_.port);
    return 0;
}

void WebSocketServer::run() {
    if (!running_ || !accept_co_) {
        return;
    }
    
    // Start the event loop
    co_eventloop(co_get_epoll_ct(), eventloop_callback, this);
}

void WebSocketServer::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    LOG_INFO("Stopping WebSocket server...");
    
    if (accept_co_) {
        co_release(accept_co_);
        accept_co_ = nullptr;
    }
    
    if (server_socket_) {
        delete server_socket_;
        server_socket_ = nullptr;
    }
    
    LOG_INFO("WebSocket server stopped");
}

bool WebSocketServer::can_accept_connection() const {
    return current_connections_.load() < config_.max_connections;
}

int WebSocketServer::add_connection() {
    current_connections_++;
    stats_.total_connections++;
    stats_.active_connections = current_connections_.load();
    return 0;
}

int WebSocketServer::remove_connection() {
    if (current_connections_ > 0) {
        current_connections_--;
        stats_.active_connections = current_connections_.load();
    }
    return 0;
}

int WebSocketServer::initialize_server_socket() {
    // Create socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        LOG_ERROR("Failed to create socket: {}", strerror(errno));
        return -1;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_WARN("Failed to set SO_REUSEADDR: {}", strerror(errno));
    }
    
    // Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config_.port);
    
    if (inet_pton(AF_INET, config_.bind_address.c_str(), &server_addr.sin_addr) <= 0) {
        LOG_ERROR("Invalid bind address: {}", config_.bind_address);
        close(sockfd);
        return -1;
    }
    
    if (bind(sockfd, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        LOG_ERROR("Failed to bind socket: {}", strerror(errno));
        close(sockfd);
        return -1;
    }
    
    // Listen
    if (listen(sockfd, config_.backlog) < 0) {
        LOG_ERROR("Failed to listen on socket: {}", strerror(errno));
        close(sockfd);
        return -1;
    }
    
    // Create MQTTSocket wrapper
    server_socket_ = new MQTTSocket(sockfd);
    if (!server_socket_) {
        LOG_ERROR("Failed to create MQTTSocket wrapper");
        close(sockfd);
        return -1;
    }
    
    return 0;
}

void WebSocketServer::cleanup_resources() {
    if (server_allocator_) {
        delete server_allocator_;
        server_allocator_ = nullptr;
    }
}

void* WebSocketServer::accept_routine(void* arg) {
    WebSocketServer* server = static_cast<WebSocketServer*>(arg);
    
    while (server->running_) {
        if (!server->can_accept_connection()) {
            usleep(100000); // Wait 100ms before checking again
            continue;
        }
        
        // Accept new connection
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server->server_socket_->get_fd(), 
                              reinterpret_cast<struct sockaddr*>(&client_addr), 
                              &client_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            LOG_ERROR("Failed to accept connection: {}", strerror(errno));
            continue;
        }
        
        // Get client information
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        int client_port = ntohs(client_addr.sin_port);
        
        LOG_DEBUG("Accepted WebSocket connection from {}:{}", client_ip, client_port);
        
        // Create client context
        WebSocketClientContext* ctx = new WebSocketClientContext();
        ctx->server = server;
        ctx->client = new MQTTSocket(client_fd);
        ctx->client_ip = client_ip;
        ctx->client_port = client_port;
        ctx->allocator = new MQTTAllocator("websocket_client", MQTTMemoryTag::MEM_TAG_CLIENT, 
                                          server->memory_config_.client_max_size);
        ctx->connect_time = std::chrono::steady_clock::now();
        
        // Create client coroutine
        stCoRoutine_t* client_co = nullptr;
        int ret = co_create(&client_co, nullptr, client_routine, ctx);
        if (ret != 0) {
            LOG_ERROR("Failed to create client coroutine: {}", ret);
            delete ctx->client;
            delete ctx->allocator;
            delete ctx;
            close(client_fd);
            continue;
        }
        
        server->add_connection();
        co_resume(client_co);
    }
    
    return nullptr;
}

void* WebSocketServer::client_routine(void* arg) {
    WebSocketClientContext* ctx = static_cast<WebSocketClientContext*>(arg);
    
    // Handle client connection
    handle_client(ctx);
    
    // Cleanup
    ctx->server->remove_connection();
    delete ctx->client;
    delete ctx->allocator;
    delete ctx;
    
    return nullptr;
}

void WebSocketServer::handle_client(WebSocketClientContext* ctx) {
    LOG_DEBUG("Handling WebSocket client {}:{}", ctx->client_ip, ctx->client_port);
    
    // Create protocol handler
    WebSocketProtocolHandler handler(ctx->allocator);
    
    int ret = handler.init(ctx->client, ctx->client_ip, ctx->client_port);
    if (ret != 0) {
        LOG_ERROR("Failed to initialize WebSocket protocol handler: {}", ret);
        ctx->server->stats_.handshake_errors++;
        return;
    }
    
    // Set server reference
    handler.set_server(ctx->server);
    handler.set_session_manager(ctx->server->session_manager_);
    
    // Generate unique client ID
    std::ostringstream oss;
    oss << "ws_" << ctx->client_ip << "_" << ctx->client_port << "_" 
        << std::chrono::duration_cast<std::chrono::milliseconds>(
            ctx->connect_time.time_since_epoch()).count();
    handler.set_client_id(oss.str());
    
    // Process WebSocket connection
    try {
        ret = handler.process();
        if (ret != 0) {
            LOG_ERROR("WebSocket handler process failed: {}", ret);
            ctx->server->stats_.protocol_errors++;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception in WebSocket handler: {}", e.what());
        ctx->server->stats_.protocol_errors++;
    }
    
    LOG_DEBUG("WebSocket client {}:{} disconnected", ctx->client_ip, ctx->client_port);
}

int WebSocketServer::eventloop_callback(void* arg) {
    WebSocketServer* server = static_cast<WebSocketServer*>(arg);
    
    // Process pending messages if session manager is available
    if (server->session_manager_) {
        // This would process any pending messages
        // For now, just return to continue the event loop
    }
    
    return 0;
}

} // namespace websocket