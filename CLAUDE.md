# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

### Primary Build Process
```bash
# Build the project
mkdir -p build && cd build
cmake .. && make -j$(nproc)

# Build and run tests  
cd build && make test
# Or run individual tests:
./unittest/test_mqtt_parser
./unittest/test_mqtt_allocator
./unittest/test_connect
```

### Quick Development
```bash
# Run the server with default config
./bin/mqtts mqtts.yaml

# Run with custom parameters
./bin/mqtts -c config/custom.yaml -i 0.0.0.0 -p 1883
```

## Architecture Overview

### Core Components

**MQTT Session Management (V2 Design)**
- `GlobalSessionManager`: Thread-safe singleton managing all session threads
- `ThreadLocalSessionManager`: Per-thread session management using coroutines (libco)
- `MQTTProtocolHandler`: Handles individual client connections and MQTT protocol
- Supports MQTT v5.0 protocol with optimized memory allocation

**Memory Management**
- Custom allocator (`MQTTAllocator`) with tcmalloc integration
- STL allocator wrapper (`mqtt_stl_allocator.h`) for container optimization
- Memory tagging system for debugging and profiling

**Message Processing**
- `MQTTMessageQueue`: Thread-safe message queuing with shared content references
- `MQTTSendWorkerPool`: Asynchronous message sending with worker threads
- `ConcurrentTopicTree`: Lock-free topic matching with wildcard support and copy-on-write
- Coroutine-based I/O using libco for high concurrency

**Configuration & Logging**
- YAML-based configuration system (`mqtt_config.h`)
- spdlog integration for structured logging
- Runtime parameter override via command line

### Dependencies
- **libco**: Coroutine library for async I/O
- **spdlog**: High-performance logging
- **tcmalloc**: Memory allocator from gperftools
- **yaml-cpp**: Configuration file parsing
- **CMake 3.22+**: Build system
- **C++11**: Minimum language standard
- **pkg-config**: For finding yaml-cpp

### Threading Model
The server uses a hybrid threading + coroutine model:
1. Main thread initializes `GlobalSessionManager`
2. Worker threads each register with `ThreadLocalSessionManager`
3. Each thread uses coroutines (libco) for handling multiple client connections
4. Cross-thread message passing via thread-safe queues

### Key Design Patterns
- **Singleton**: `GlobalSessionManager` for global state coordination
- **Thread-Local Storage**: Per-thread session managers to minimize locking
- **RAII**: Custom allocators and smart pointers for memory safety
- **Producer-Consumer**: Async message queues between threads

### Testing Structure
- Unit tests in `unittest/` directory using custom test framework
- Tests cover: MQTT packet parsing, memory allocation, connection handling
- CMake test integration with `make test`

### Configuration
- Default config: `mqtts.yaml`
- Server settings: bind address, port, thread count, max connections
- MQTT settings: packet size limits, keep-alive defaults
- Memory settings: per-client memory limits
- Logging configuration: levels, file output, rotation

### Debugging Commands
```bash
# Debug individual test components
./build/unittest/test_mqtt_parser     # Test packet parsing
./build/unittest/test_mqtt_allocator  # Test memory allocation
./build/unittest/test_mqtt_topic_tree # Test topic matching

# Check server configuration
./bin/mqtts --help                    # Show available options
./bin/mqtts -c mqtts.yaml --dry-run   # Validate config (if supported)

# Monitor server logs
tail -f logs/mqtt.log                 # Follow log output
```
