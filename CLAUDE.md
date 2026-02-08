# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

### Primary Build Process
```bash
# Build the project
mkdir -p build && cd build
cmake .. && make -j$(nproc)

# Build and run all tests
cd build && make test
# Or run with ctest:
ctest --output-on-failure
```

### Running Individual Tests
```bash
./build/unittest/test_mqtt_parser              # MQTT packet parsing
./build/unittest/test_mqtt_allocator           # Memory allocation
./build/unittest/test_mqtt_topic_tree          # Topic matching
./build/unittest/test_websocket_frame          # WebSocket frame handling
./build/unittest/test_mqtt_router_service      # Router service
./build/unittest/test_mqtt_router_rpc_client   # Router RPC client
./build/unittest/test_mqtt_auth_manager        # Auth manager
./build/unittest/test_session_manager_stress   # Stress tests
```

### Quick Development
```bash
# Run the main MQTT server with default config
./build/bin/mqtts mqtts.yaml

# Run with custom parameters
./build/bin/mqtts -c config/custom.yaml -i 0.0.0.0 -p 1883

# Run the standalone router service
./build/bin/mqtt_router_service -c mqtt_router.yaml

# Run the auth admin tool
./build/bin/auth_admin -c mqtts_with_auth.yaml
```

## Architecture Overview

### Core Components

**MQTT Session Management (V2 Design)**
- `GlobalSessionManager`: Thread-safe singleton managing all session threads with lock-free read paths
- `ThreadLocalSessionManager`: Per-thread session management using coroutines (libco)
- `MQTTProtocolHandler`: Handles individual client connections and MQTT protocol
- Uses `SafeHandlerRef` for thread-safe handler references with reference counting
- Supports MQTT v5.0 protocol with optimized memory allocation

**Memory Management**
- Custom allocator (`MQTTAllocator`) with tcmalloc integration and memory tagging
- STL allocator wrapper (`mqtt_stl_allocator.h`) for container optimization
- `MessageContentCache` for shared message content deduplication
- Per-client memory limits via `MemoryConfig`

**Message Processing**
- `MQTTMessageQueue`: Thread-safe message queuing with shared content references
- `MQTTSendWorkerPool`: Asynchronous message sending with worker threads
- `ConcurrentTopicTree`: Lock-free topic matching with wildcard support and copy-on-write
- Coroutine-based I/O using libco for high concurrency

**WebSocket Support**
- `WebSocketFrame`: WebSocket frame parsing/serialization (RFC 6455)
- `WebSocketProtocolHandler`: WebSocket protocol upgrade handling
- `WebSocketMQTTBridge`: Bridges WebSocket connections to MQTT protocol
- Configurable message format (json, mqtt_packet, text_protocol)

**Router Service (Cluster Support)**
- Standalone service for cross-server message routing
- `MQTTPersistentTopicTree`: Persistent topic subscription storage
- `MQTTRedoLogManager`: WAL-based durability for subscriptions
- RPC handlers for subscribe, unsubscribe, publish routing
- Server heartbeat and client connect/disconnect tracking

**Authentication System**
- Pluggable auth providers: SQLite, Redis (configurable priority)
- Auth caching with TTL support
- `MQTTAuthManager` coordinates provider chain
- Password hashing via OpenSSL

### Dependencies
- **libco**: Coroutine library for async I/O (included in 3rd/)
- **spdlog**: High-performance logging (included in 3rd/)
- **tcmalloc**: Memory allocator from gperftools (included in 3rd/)
- **yaml-cpp**: Configuration file parsing
- **SQLite3**: Optional, for SQLite auth provider
- **hiredis**: Optional, for Redis auth provider
- **OpenSSL**: For password hashing
- **CMake 3.22+**: Build system
- **C++11**: Minimum language standard (tests use C++14)

### Threading Model
The server uses a hybrid threading + coroutine model:
1. Main thread initializes `GlobalSessionManager`
2. Worker threads each register with `ThreadLocalSessionManager`
3. Each thread uses coroutines (libco) for handling multiple client connections
4. Cross-thread message passing via thread-safe queues
5. Router service uses dedicated worker threads + coroutines per connection

### Key Design Patterns
- **Singleton**: `GlobalSessionManager` for global state coordination
- **Thread-Local Storage**: Per-thread session managers to minimize locking
- **RAII**: Custom allocators and smart pointers for memory safety
- **Producer-Consumer**: Async message queues between threads
- **Read-Write Lock**: RWMutex for client index (read-heavy workload)
- **Copy-on-Write**: `ConcurrentTopicTree` for lock-free reads

### Configuration
- Default config: `mqtts.yaml`
- Router config: `mqtts_with_router.yaml` or `mqtt_router.yaml`
- Auth config: `mqtts_with_auth.yaml`
- Sections: server, mqtt, memory, log, websocket, event_forwarding, monitoring, auth

### Executables
| Binary | Purpose |
|--------|---------|
| `bin/mqtts` | Main MQTT broker server |
| `bin/mqtt_router_service` | Standalone router for cluster deployments |
| `bin/auth_admin` | Authentication management tool |
| `bin/performance_analyzer` | Router performance analysis |
| `bin/simple_websocket_mqtt_test` | WebSocket-MQTT integration test |

### Data Flow
```
Client TCP Connection
       ↓
MQTTSocket → MQTTProtocolHandler → ThreadLocalSessionManager
       ↓                              ↓
    GlobalSessionManager ←──── Message Routing ────→ ConcurrentTopicTree
       ↓                              ↓
   MQTTSendWorkerPool          Subscribers Lookup
       ↓
   Client ACK
```
