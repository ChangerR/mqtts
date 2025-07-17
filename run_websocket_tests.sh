#!/bin/bash

# WebSocket服务测试脚本

echo "🚀 开始运行WebSocket服务测试..."
echo "======================================"

# 运行基础WebSocket测试
echo "1. 运行基础WebSocket测试..."
if g++ -o test_websocket_basic test_websocket_basic.cpp && ./test_websocket_basic; then
    echo "✅ 基础WebSocket测试通过"
else
    echo "❌ 基础WebSocket测试失败"
    exit 1
fi

echo ""

# 运行WebSocket帧测试
echo "2. 运行WebSocket帧解析测试..."
if g++ -o simple_websocket_test simple_websocket_test.cpp && ./simple_websocket_test; then
    echo "✅ WebSocket帧解析测试通过"
else
    echo "❌ WebSocket帧解析测试失败"
    exit 1
fi

echo ""

# 运行MQTT桥接测试
echo "3. 运行MQTT桥接测试..."
if g++ -o simple_mqtt_bridge_test simple_mqtt_bridge_test.cpp && ./simple_mqtt_bridge_test; then
    echo "✅ MQTT桥接测试通过"
else
    echo "❌ MQTT桥接测试失败"
    exit 1
fi

echo ""

# 运行集成测试
echo "4. 运行WebSocket-MQTT集成测试..."
if g++ -o websocket_integration_test websocket_integration_test.cpp && ./websocket_integration_test; then
    echo "✅ WebSocket-MQTT集成测试通过"
else
    echo "❌ WebSocket-MQTT集成测试失败"
    exit 1
fi

echo ""
echo "🎉 所有WebSocket测试成功完成！"
echo "======================================"
echo ""
echo "📋 WebSocket服务功能总结："
echo "- ✅ WebSocket RFC 6455协议实现"
echo "- ✅ 帧解析和序列化"
echo "- ✅ 文本和二进制消息支持"
echo "- ✅ Ping/Pong心跳机制"
echo "- ✅ 握手验证"
echo "- ✅ MQTT协议桥接"
echo "- ✅ JSON消息格式支持"
echo "- ✅ 文本协议支持"
echo "- ✅ Base64编码/解码"
echo "- ✅ 主题订阅/发布"
echo "- ✅ 通配符主题匹配"
echo "- ✅ 与现有MQTT系统兼容"
echo ""
echo "🏗️ 项目结构："
echo "- src/websocket_server.h/cpp         - WebSocket服务器"
echo "- src/websocket_protocol_handler.h/cpp - WebSocket协议处理"
echo "- src/websocket_frame.h/cpp          - WebSocket帧处理"
echo "- src/websocket_mqtt_bridge.h/cpp    - MQTT桥接器"
echo "- src/websocket_main.cpp             - WebSocket服务主程序"
echo "- unittest/test_websocket_*.cpp      - 单元测试"
echo "- websocket.yaml                     - 配置文件"
echo ""
echo "🚀 WebSocket服务已准备就绪！"