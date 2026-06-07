# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Container-First Development

**The target environment is Linux.** All compilation, testing, and validation must run inside Docker containers, not natively on macOS. The primary entry point is:

```bash
./bin/container-validate.sh all   # build image + compile + run all tests
```

### Step-by-step Container Commands
```bash
./bin/container-validate.sh image      # build dev image only
./bin/container-validate.sh configure  # cmake configure only
./bin/container-validate.sh build      # compile only
./bin/container-validate.sh test       # run ctest only
./bin/container-validate.sh shell      # interactive shell in dev container
./bin/container-validate.sh run        # start mqtts server container (port 1883)
```

### Running a Specific Test
```bash
TEST_REGEX=test_mqtt_parser ./bin/container-validate.sh test
```

### Useful Environment Variables
| Variable | Default | Description |
|----------|---------|-------------|
| `BUILD_TYPE` | `RelWithDebInfo` | CMake build type (Debug, Release, etc.) |
| `JOBS` | CPU count | Parallel compile jobs |
| `BUILD_DIR` | `build-container` | Build output directory |
| `CTEST_TIMEOUT` | `120` | Test timeout in seconds |
| `TEST_REGEX` | (none) | ctest filter regex |

Example: `BUILD_TYPE=Debug JOBS=8 ./bin/container-validate.sh all`

### Available Test Binaries (inside container at `build-container/unittest/`)
```
test_mqtt_parser              # MQTT packet parsing
test_mqtt_allocator           # Memory allocation
test_mqtt_topic_tree          # Topic matching
test_websocket_frame          # WebSocket frame handling
test_mqtt_router_service      # Router service
test_mqtt_router_rpc_client   # Router RPC client
test_mqtt_auth_manager        # Auth manager
test_session_manager_stress   # Stress tests
```

### Running the Server
```bash
./bin/container-validate.sh run   # starts mqtts container, maps port 1883
docker logs -f mqtts              # follow logs
docker rm -f mqtts                # stop and remove
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

**Event Forwarding (`src/events/`)**
- `MQTTEventForwardingService` / `MQTTEventForwardingQueue`: Async forwarding of lifecycle events (login, logout, publish) to an external RPC endpoint
- Events are batched for efficiency; configured via `event_forwarding` section in YAML

**Forwarding Service (`src/forwarding/`)**
- `MQTTForwardingService`: Accepts inbound forwarded messages from remote MQTT broker instances
- `MQTTForwardingRpcClient`: Sends messages to remote brokers; used by cluster deployments alongside the Router Service

**HTTP Parser (`src/http/`)**
- `HttpParser` / `HttpMessage`: Lightweight HTTP/1.1 request-response handling, used internally for WebSocket upgrade handshakes

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

### Protocol Support
- Primary: MQTT v5.0
- MQTT 3.1 / 3.1.1 compatibility is **enabled by default** via `mqtt.allow_mqtt3x: true` in `mqtts.yaml`; set to `false` to restrict to v5.0 only

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
