cmake_minimum_required(VERSION 3.22)

project(mqtts)

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

add_subdirectory(3rd/gperftools)
add_subdirectory(3rd EXCLUDE_FROM_ALL)
add_executable(mqtts 
	src/main.cpp 
	src/mqtt_socket.cpp 
	src/mqtt_server.cpp
	src/mqtt_allocator.cpp
	src/mqtt_protocol_handler.cpp
	src/mqtt_parser.cpp
)

target_link_libraries(mqtts 
	colib_static 
	pthread 
	dl 
	tcmalloc_minimal
)