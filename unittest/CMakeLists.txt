cmake_minimum_required(VERSION 3.10)

# 设置C++11标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加测试可执行文件
add_executable(test_connect test_connect.cpp)
add_executable(test_mqtt_allocator test_mqtt_allocator.cpp)
add_executable(test_mqtt_parser test_mqtt_parser.cpp)

# 添加头文件搜索路径
target_include_directories(test_connect PRIVATE ${CMAKE_SOURCE_DIR})
target_include_directories(test_mqtt_allocator PRIVATE ${CMAKE_SOURCE_DIR})
target_include_directories(test_mqtt_parser PRIVATE ${CMAKE_SOURCE_DIR})

# 链接必要的库
target_link_libraries(test_connect PRIVATE mqtt_parser tcmalloc_minimal)
target_link_libraries(test_mqtt_allocator PRIVATE mqtt_parser tcmalloc_minimal)
target_link_libraries(test_mqtt_parser PRIVATE mqtt_parser tcmalloc_minimal)

# 启用测试
enable_testing()
add_test(NAME test_connect COMMAND test_connect)
add_test(NAME test_mqtt_allocator COMMAND test_mqtt_allocator)
add_test(NAME test_mqtt_parser COMMAND test_mqtt_parser) 