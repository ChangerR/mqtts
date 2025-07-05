# MQTT Session Manager Allocator Integration

## Overview

The MQTT Session Manager has been completely integrated with the MQTT allocator system to provide comprehensive memory management, tracking, and limits for all session-related operations.

## Key Features

### 1. Memory Hierarchy
- **Global Allocator**: Root allocator for all MQTT operations
- **Thread-Local Allocators**: Per-thread allocators for session management
- **Client-Specific Allocators**: Dedicated allocators for each client session
- **Component-Specific Allocators**: Separate allocators for queues, workers, and message cache

### 2. Memory Tagging
Added new memory tags for better categorization:
- `MEM_TAG_SESSION`: Main session allocations
- `MEM_TAG_SESSION_QUEUE`: Message queue allocations
- `MEM_TAG_SESSION_WORKER`: Worker pool allocations
- `MEM_TAG_MESSAGE_CACHE`: Message cache allocations

### 3. Memory Limits and Monitoring
- Per-client memory limits
- Real-time memory usage tracking
- Memory limit violation detection
- Comprehensive memory statistics

## Architecture Changes

### SessionAllocatorManager
New utility class that manages session-specific allocators:
- Creates and manages allocator hierarchy
- Provides factory methods for allocator-aware containers
- Handles cleanup of expired allocators
- Tracks memory usage per client

### ThreadLocalSessionManager
Updated to use allocator-aware containers:
- `SessionUnorderedMap` for session storage
- `SessionQueue` for message queues
- Proper allocator initialization and cleanup
- Memory statistics and limit checking

### GlobalSessionManager
Enhanced with global memory management:
- Global allocator management
- Client memory limit enforcement
- Memory usage reporting
- Allocator hierarchy visualization

## Usage Examples

### Setting Client Memory Limits
```cpp
// Set memory limit for a specific client
MQTTString client_id = to_mqtt_string("client123", nullptr);
int ret = global_session_manager->set_client_memory_limit(client_id, 1024 * 1024); // 1MB limit
if (ret != MQ_SUCCESS) {
    LOG_ERROR("Failed to set memory limit: {}", mqtt_error_string(ret));
}
```

### Monitoring Memory Usage
```cpp
// Get global memory usage
size_t total_usage = 0;
int ret = global_session_manager->get_global_memory_usage(total_usage);
if (ret == MQ_SUCCESS) {
    LOG_INFO("Total memory usage: {} bytes", total_usage);
}

// Get per-client memory usage
std::unordered_map<std::string, size_t> client_usage;
ret = global_session_manager->get_client_memory_usage(client_usage);
if (ret == MQ_SUCCESS) {
    for (const auto& pair : client_usage) {
        LOG_INFO("Client {}: {} bytes", pair.first, pair.second);
    }
}
```

### Checking Memory Limits
```cpp
// Check if client exceeded memory limit
bool limit_exceeded = false;
int ret = global_session_manager->is_client_memory_limit_exceeded(client_id, limit_exceeded);
if (ret == MQ_SUCCESS && limit_exceeded) {
    LOG_WARN("Client {} exceeded memory limit", from_mqtt_string(client_id));
} else if (ret != MQ_SUCCESS) {
    LOG_ERROR("Failed to check memory limit: {}", mqtt_error_string(ret));
}
```

### Memory Statistics
```cpp
// Get detailed memory statistics
ThreadLocalSessionManager* thread_manager = global_session_manager->get_thread_manager();
ThreadLocalSessionManager::MemoryStats stats;
int ret = thread_manager->get_memory_statistics(stats);
if (ret == MQ_SUCCESS) {
    LOG_INFO("Session Usage: {} bytes", stats.session_usage);
    LOG_INFO("Queue Usage: {} bytes", stats.queue_usage);
    LOG_INFO("Worker Usage: {} bytes", stats.worker_usage);
} else {
    LOG_ERROR("Failed to get memory statistics: {}", mqtt_error_string(ret));
}
```

## Benefits

### 1. Memory Control
- Prevents memory leaks through proper allocator management
- Enforces memory limits to prevent OOM conditions
- Provides detailed memory usage tracking

### 2. Performance
- Reduces memory fragmentation through custom allocators
- Optimizes memory locality for session data
- Enables bulk memory operations

### 3. Monitoring
- Real-time memory usage statistics
- Per-client memory tracking
- Memory limit violation alerts
- Allocator hierarchy visualization

### 4. Maintenance
- Automatic cleanup of expired allocators
- Memory leak detection through usage tracking
- Configurable memory limits per client

## Configuration

Memory limits can be configured via:
1. **Runtime API**: `set_client_memory_limit()`
2. **Configuration File**: Can be extended to read from `mqtts.yaml`
3. **Environment Variables**: Can be implemented for dynamic configuration

## Cleanup and Maintenance

### Automatic Cleanup
- Session allocators are automatically cleaned up when sessions are unregistered
- Thread allocators are cleaned up when threads terminate
- Expired allocators are periodically cleaned up

### Manual Cleanup
```cpp
// Clean up expired allocators
int cleaned_count = 0;
int ret = global_session_manager->cleanup_expired_allocators(cleaned_count);
if (ret == MQ_SUCCESS) {
    LOG_INFO("Cleaned up {} expired allocators", cleaned_count);
} else {
    LOG_ERROR("Failed to cleanup expired allocators: {}", mqtt_error_string(ret));
}
```

## Error Code Design

All allocator management functions follow a consistent error code pattern:
- **Return Value**: Always an `int` error code (`MQ_SUCCESS` for success)
- **Output Parameters**: Actual return values passed by reference
- **Error Codes**: Specific allocator error codes in the -700 to -799 range

### Allocator Error Codes
- `MQ_ERR_ALLOCATOR`: General allocator error
- `MQ_ERR_ALLOCATOR_CREATE`: Failed to create allocator
- `MQ_ERR_ALLOCATOR_NOT_FOUND`: Allocator not found
- `MQ_ERR_ALLOCATOR_CLEANUP`: Failed to cleanup allocator
- `MQ_ERR_ALLOCATOR_LIMIT_EXCEEDED`: Allocator limit exceeded
- `MQ_ERR_ALLOCATOR_INVALID_PARENT`: Invalid parent allocator
- `MQ_ERR_ALLOCATOR_INVALID_TAG`: Invalid memory tag
- `MQ_ERR_ALLOCATOR_HIERARCHY`: Allocator hierarchy error

### Error Handling Pattern
```cpp
// Standard pattern for all allocator functions
MQTTAllocator* allocator = nullptr;
int ret = SessionAllocatorManager::get_session_allocator(client_id, limit, allocator);
if (ret == MQ_SUCCESS) {
    // Use allocator
} else {
    LOG_ERROR("Allocator operation failed: {}", mqtt_error_string(ret));
    // Handle error
}
```

## Error Handling

The system provides comprehensive error handling:
- Graceful fallback to standard allocators if custom allocators fail
- Detailed logging of allocation failures with specific error codes
- Memory limit violation warnings
- Automatic cleanup on errors
- Consistent error reporting through `mqtt_error_string()`

## Integration Status

✅ **Complete Integration Points:**
- ThreadLocalSessionManager allocator integration
- GlobalSessionManager memory management
- Client-specific allocator creation
- Memory limit enforcement
- Statistics and monitoring
- CMakeLists.txt updates

✅ **Memory Tags Added:**
- Session-specific memory tags
- Component-specific categorization

✅ **API Extensions:**
- Memory management APIs with error code returns
- Statistics reporting with error handling
- Limit configuration with validation

✅ **Error Code System:**
- Dedicated allocator error codes (-700 to -799)
- Consistent error code return pattern
- Reference parameter pattern for return values
- Comprehensive error message translation

## Future Enhancements

1. **Configuration Integration**: Read memory limits from `mqtts.yaml`
2. **Memory Pool Optimization**: Implement memory pools for frequent allocations
3. **Metrics Export**: Export memory statistics to monitoring systems
4. **Dynamic Limits**: Support for runtime memory limit adjustments
5. **Memory Profiling**: Integration with profiling tools for optimization

## Testing

The implementation includes comprehensive testing through:
- Unit tests for allocator management
- Integration tests for session operations
- Memory leak detection tests
- Performance benchmarks

## Dependencies

- **GPerfTools**: For tcmalloc integration
- **MQTT Core**: Uses existing MQTT allocator infrastructure
- **Logging**: Comprehensive logging for debugging and monitoring