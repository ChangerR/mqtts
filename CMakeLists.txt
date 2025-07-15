cmake_minimum_required(VERSION 3.22)

project(mqtts)

# 获取Git信息
find_package(Git REQUIRED)
execute_process(
	COMMAND ${GIT_EXECUTABLE} describe --tags --always
	OUTPUT_VARIABLE GIT_TAG
	OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
	COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
	OUTPUT_VARIABLE GIT_HASH
	OUTPUT_STRIP_TRAILING_WHITESPACE
)

# 获取构建时间
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")

# 配置版本头文件
configure_file(
	${CMAKE_SOURCE_DIR}/src/version.h.in
	${CMAKE_BINARY_DIR}/generated/version.h
	@ONLY
)

# 添加生成的头文件目录
include_directories(${CMAKE_BINARY_DIR}/generated)

# Enable compilation database for ccls
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 设置静态链接
set(BUILD_SHARED_LIBS OFF)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++11")
endif()

set(THIRD_PART_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rd")
include_directories(
	"${THIRD_PART_DIR}/spdlog/include"
	"${THIRD_PART_DIR}/libco"
	"${THIRD_PART_DIR}/gperftools/src"
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Build only tcmalloc
set(GPERFTOOLS_BUILD_HEAP_PROFILER OFF CACHE BOOL "Build heap profiler")
set(GPERFTOOLS_BUILD_HEAP_CHECKER OFF CACHE BOOL "Build heap checker")
set(GPERFTOOLS_BUILD_CPU_PROFILER OFF CACHE BOOL "Build CPU profiler")
set(GPERFTOOLS_BUILD_STATIC ON CACHE BOOL "Build static libraries")
set(GPERFTOOLS_BUILD_SHARED OFF CACHE BOOL "Build shared libraries")
set(GPERFTOOLS_BUILD_TCMALLOC ON CACHE BOOL "Build tcmalloc")
set(GPERFTOOLS_BUILD_TCMALLOC_MINIMAL ON CACHE BOOL "Build tcmalloc minimal")
set(gperftools_build_minimal ON CACHE BOOL "Build gperftools minimal")
set(GPERFTOOLS_BUILD_TESTING OFF CACHE BOOL "Build gperftools testing")
add_subdirectory(3rd/gperftools)
add_subdirectory(3rd EXCLUDE_FROM_ALL)

# 添加mqtt_parser库
add_library(mqtt_parser STATIC
    src/mqtt_parser.cpp
    src/mqtt_allocator.cpp
    src/mqtt_serialize_buffer.cpp
    src/mqtt_string_utils.cpp
    src/mqtt_config.cpp
    src/mqtt_session_manager_v2.cpp
	src/mqtt_session_info.cpp
    src/mqtt_topic_tree.cpp
	src/mqtt_send_worker_pool.cpp
	src/mqtt_process_monitor.cpp
	src/mqtt_event_types.cpp
	src/mqtt_event_forwarding_queue.cpp
	src/mqtt_event_rpc_client.cpp
	src/mqtt_event_forwarding_service.cpp
)

# 添加mqtt_router库
add_library(mqtt_router STATIC
    src/mqtt_router_service.cpp
    src/mqtt_router_rpc_client.cpp
    src/mqtt_allocator.cpp
    src/mqtt_string_utils.cpp
    src/mqtt_topic_tree.cpp
)

# mqtt_router 需要链接 colib_static，因为 mqtt_router_service.cpp 使用了 libco 功能
target_link_libraries(mqtt_router colib_static)

# 添加mqtt_auth库
add_library(mqtt_auth STATIC
    src/mqtt_auth_manager.cpp
    src/mqtt_auth_sqlite.cpp
    src/mqtt_auth_redis.cpp
    src/mqtt_allocator.cpp
    src/mqtt_string_utils.cpp
)

# 查找认证相关依赖
find_package(PkgConfig REQUIRED)

# 查找SQLite3
find_package(SQLite3 REQUIRED)

# 查找hiredis (Redis client)
pkg_check_modules(HIREDIS REQUIRED hiredis)

# 查找OpenSSL (用于密码哈希)
find_package(OpenSSL REQUIRED)

# mqtt_auth 需要链接相关依赖
target_link_libraries(mqtt_auth 
    SQLite::SQLite3
    ${HIREDIS_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_include_directories(mqtt_auth PUBLIC
    ${HIREDIS_INCLUDE_DIRS}
)

# 查找yaml-cpp库的静态版本
find_package(PkgConfig REQUIRED)
pkg_check_modules(YAML_CPP REQUIRED IMPORTED_TARGET yaml-cpp)

target_include_directories(mqtt_parser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${YAML_CPP_INCLUDE_DIRS}
)

# 使用yaml-cpp的静态库
target_link_libraries(mqtt_parser 
    -Wl,-Bstatic
    ${YAML_CPP_STATIC_LIBRARIES}
    -Wl,-Bdynamic
)

# 添加主程序
add_executable(mqtts 
	src/main.cpp 
	src/mqtt_socket.cpp 
	src/mqtt_server.cpp
	src/mqtt_protocol_handler.cpp
	src/logger.cpp
)

# 静态链接所有库
target_link_libraries(mqtts 
    colib_static 
    mqtt_parser
    mqtt_router
    mqtt_auth
    tcmalloc_minimal
    pthread 
    dl
)

# 添加路由器服务可执行文件
add_executable(mqtt_router_service src/mqtt_router_main.cpp)

# 为mqtt_router可执行文件添加依赖配置
target_include_directories(mqtt_router_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${YAML_CPP_INCLUDE_DIRS}
)

target_link_libraries(mqtt_router_service 
    colib_static
    mqtt_router
    tcmalloc_minimal
    pthread 
    dl
    -Wl,-Bstatic
    ${YAML_CPP_STATIC_LIBRARIES}
    -Wl,-Bdynamic
)

# 添加性能分析工具
add_executable(performance_analyzer tools/performance_analyzer.cpp)

target_include_directories(performance_analyzer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${YAML_CPP_INCLUDE_DIRS}
)

target_link_libraries(performance_analyzer 
    mqtt_router
    colib_static
    tcmalloc_minimal
    pthread 
    dl
    -Wl,-Bstatic
    ${YAML_CPP_STATIC_LIBRARIES}
    -Wl,-Bdynamic
)

# 添加认证管理工具
add_executable(auth_admin tools/auth_admin.cpp)

target_include_directories(auth_admin PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${YAML_CPP_INCLUDE_DIRS}
)

target_link_libraries(auth_admin 
    mqtt_auth
    colib_static
    tcmalloc_minimal
    pthread 
    dl
    -Wl,-Bstatic
    ${YAML_CPP_STATIC_LIBRARIES}
    -Wl,-Bdynamic
)

# 启用测试
enable_testing()

# 添加测试目录
add_subdirectory(unittest)
