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
add_executable(test_publish_serialization test_publish_serialization.cpp)
add_executable(test_debug_serialization test_debug_serialization.cpp)
add_executable(test_mqttv5_client test_mqttv5_client.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_server.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_protocol_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_socket.cpp
    ${CMAKE_SOURCE_DIR}/src/logger.cpp)
add_executable(test_send_publish test_send_publish.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_protocol_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_socket.cpp
    ${CMAKE_SOURCE_DIR}/src/logger.cpp)
add_executable(test_qos_reply test_qos_reply.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_protocol_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_socket.cpp
    ${CMAKE_SOURCE_DIR}/src/logger.cpp)
add_executable(test_debug_properties test_debug_properties.cpp)
add_executable(test_mqtt_protocol_comprehensive test_mqtt_protocol_comprehensive.cpp)
add_executable(test_mqtt_serialization_comprehensive test_mqtt_serialization_comprehensive.cpp)
add_executable(test_mqtt_edge_cases test_mqtt_edge_cases.cpp)
add_executable(test_mqtt_session_manager_optimization test_mqtt_session_manager_optimization.cpp)
add_executable(test_mqtt_session_manager_memory test_mqtt_session_manager_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_session_manager_v2.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_allocator.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_protocol_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_socket.cpp
    ${CMAKE_SOURCE_DIR}/src/logger.cpp)
add_executable(test_simple_memory test_simple_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_session_manager_v2.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_allocator.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_protocol_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_socket.cpp
    ${CMAKE_SOURCE_DIR}/src/logger.cpp)
add_executable(test_session_manager_allocator test_session_manager_allocator.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_session_manager_v2.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_allocator.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_protocol_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_socket.cpp
    ${CMAKE_SOURCE_DIR}/src/logger.cpp)
add_executable(test_session_manager_stress test_session_manager_stress.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_session_manager_v2.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_allocator.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_protocol_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/mqtt_socket.cpp
    ${CMAKE_SOURCE_DIR}/src/logger.cpp)

# 添加头文件搜索路径
target_include_directories(test_connect PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqtt_allocator PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqtt_parser PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqtt_topic_tree PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqttv5_protocol PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_publish_serialization PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_debug_serialization PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqttv5_client PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR} ${YAML_CPP_INCLUDE_DIRS})
target_include_directories(test_send_publish PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_qos_reply PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_debug_properties PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqtt_protocol_comprehensive PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqtt_serialization_comprehensive PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqtt_edge_cases PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqtt_session_manager_optimization PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR})
target_include_directories(test_mqtt_session_manager_memory PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR} ${YAML_CPP_INCLUDE_DIRS})
target_include_directories(test_simple_memory PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR} ${YAML_CPP_INCLUDE_DIRS})
target_include_directories(test_session_manager_allocator PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR} ${YAML_CPP_INCLUDE_DIRS})
target_include_directories(test_session_manager_stress PRIVATE ${CMAKE_SOURCE_DIR} ${GTEST_INCLUDE_DIR} ${YAML_CPP_INCLUDE_DIRS})

# 链接必要的库（包括Google Test）
target_link_libraries(test_connect PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqtt_allocator PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqtt_parser PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqtt_topic_tree PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqttv5_protocol PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_publish_serialization PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_debug_serialization PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqttv5_client PRIVATE colib_static mqtt_parser tcmalloc_minimal gtest pthread dl -Wl,-Bstatic ${YAML_CPP_STATIC_LIBRARIES} -Wl,-Bdynamic)
target_link_libraries(test_send_publish PRIVATE colib_static mqtt_parser tcmalloc_minimal gtest pthread dl)
target_link_libraries(test_qos_reply PRIVATE colib_static mqtt_parser tcmalloc_minimal gtest pthread dl)
target_link_libraries(test_debug_properties PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqtt_protocol_comprehensive PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqtt_serialization_comprehensive PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqtt_edge_cases PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqtt_session_manager_optimization PRIVATE mqtt_parser tcmalloc_minimal gtest pthread)
target_link_libraries(test_mqtt_session_manager_memory PRIVATE colib_static mqtt_parser tcmalloc_minimal gtest pthread dl -Wl,-Bstatic ${YAML_CPP_STATIC_LIBRARIES} -Wl,-Bdynamic)
target_link_libraries(test_simple_memory PRIVATE colib_static mqtt_parser tcmalloc_minimal gtest pthread dl -Wl,-Bstatic ${YAML_CPP_STATIC_LIBRARIES} -Wl,-Bdynamic)
target_link_libraries(test_session_manager_allocator PRIVATE colib_static mqtt_parser tcmalloc_minimal gtest pthread dl -Wl,-Bstatic ${YAML_CPP_STATIC_LIBRARIES} -Wl,-Bdynamic)
target_link_libraries(test_session_manager_stress PRIVATE colib_static mqtt_parser tcmalloc_minimal gtest pthread dl -Wl,-Bstatic ${YAML_CPP_STATIC_LIBRARIES} -Wl,-Bdynamic)

# 启用测试
enable_testing()
add_test(NAME test_connect COMMAND test_connect)
add_test(NAME test_mqtt_allocator COMMAND test_mqtt_allocator)
add_test(NAME test_mqtt_parser COMMAND test_mqtt_parser)
add_test(NAME test_mqtt_topic_tree COMMAND test_mqtt_topic_tree)
add_test(NAME test_mqttv5_protocol COMMAND test_mqttv5_protocol)
add_test(NAME test_publish_serialization COMMAND test_publish_serialization)
add_test(NAME test_mqttv5_client COMMAND test_mqttv5_client)
add_test(NAME test_send_publish COMMAND test_send_publish)
add_test(NAME test_qos_reply COMMAND test_qos_reply)
add_test(NAME test_mqtt_protocol_comprehensive COMMAND test_mqtt_protocol_comprehensive)
add_test(NAME test_mqtt_serialization_comprehensive COMMAND test_mqtt_serialization_comprehensive)
add_test(NAME test_mqtt_edge_cases COMMAND test_mqtt_edge_cases)
add_test(NAME test_mqtt_session_manager_optimization COMMAND test_mqtt_session_manager_optimization)
add_test(NAME test_mqtt_session_manager_memory COMMAND test_mqtt_session_manager_memory)
add_test(NAME test_session_manager_allocator COMMAND test_session_manager_allocator)
add_test(NAME test_session_manager_stress COMMAND test_session_manager_stress) 