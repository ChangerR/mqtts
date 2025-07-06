cmake_minimum_required(VERSION 3.10)

# 设置C++14标准（Google Test 需要）
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Google Test 头文件路径
set(GTEST_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/3rd/gperftools/vendor/googletest/googletest/include)
set(GTEST_LIB_DIR ${CMAKE_BINARY_DIR}/3rd/gperftools)

# 添加测试可执行文件
add_executable(test_connect test_connect.cpp)
add_executable(test_mqtt_allocator test_mqtt_allocator.cpp)
add_executable(test_mqtt_parser test_mqtt_parser.cpp)
add_executable(test_mqtt_topic_tree test_mqtt_topic_tree.cpp)
add_executable(test_mqttv5_protocol test_mqttv5_protocol.cpp)

# 添加头文件搜索路径
target_include_directories(test_connect PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqtt_allocator PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqtt_parser PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqtt_topic_tree PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqttv5_protocol PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})

# 链接必要的库（包括Google Test）
target_link_libraries(test_connect PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqtt_allocator PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqtt_parser PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqtt_topic_tree PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqttv5_protocol PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)

# 启用测试
enable_testing()
add_test(NAME test_connect COMMAND test_connect)
add_test(NAME test_mqtt_allocator COMMAND test_mqtt_allocator)
add_test(NAME test_mqtt_parser COMMAND test_mqtt_parser)
add_test(NAME test_mqtt_topic_tree COMMAND test_mqtt_topic_tree)
add_test(NAME test_mqttv5_protocol COMMAND test_mqttv5_protocol) 