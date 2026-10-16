#include "websocket_handler_adapter.h"
#include "logger.h"
#include "mqtt_string_utils.h"

namespace websocket {

WebSocketHandlerAdapter::WebSocketHandlerAdapter(MQTTAllocator* allocator,
                                                 WebSocketMQTTBridge* bridge,
                                                 WebSocketProtocolHandler* ws_handler,
                                                 const std::string& client_id)
    : mqtt::MQTTProtocolHandler(allocator),
      adapter_allocator_(allocator),
      bridge_(bridge),
      ws_handler_(ws_handler),
      client_id_(client_id) {
}

WebSocketHandlerAdapter::~WebSocketHandlerAdapter() {
    // Don't delete bridge_ or ws_handler_, they're managed elsewhere
}

void WebSocketHandlerAdapter::request_session_takeover(
    const std::shared_ptr<mqtt::SessionTakeoverGate>& gate) {
    if (ws_handler_) {
        ws_handler_->request_session_takeover(gate);
        return;
    }
    if (gate) {
        gate->done.store(true, std::memory_order_release);
    }
}

int WebSocketHandlerAdapter::transmit_serialized(const uint8_t* data, size_t size) {
    if (!ws_handler_ || !ws_handler_->is_connected()) {
        return MQ_ERR_SESSION_INVALID_HANDLER;
    }
    std::vector<uint8_t> payload(data, data + size);
    const int ret = ws_handler_->send_binary(payload);
    if (ret == MQ_SUCCESS && bridge_) {
        bridge_->get_statistics().ws_messages_sent.fetch_add(1, std::memory_order_relaxed);
        bridge_->get_statistics().mqtt_messages_sent.fetch_add(1, std::memory_order_relaxed);
        if (bridge_->get_session_manager()) {
            bridge_->get_session_manager()->note_bytes_sent(payload.size());
        }
    }
    return ret;
}

int WebSocketHandlerAdapter::send_publish(const mqtt::MQTTString& topic,
                                          const mqtt::MQTTByteVector& payload,
                                          uint8_t qos,
                                          bool retain,
                                          bool dup,
                                          const mqtt::Properties& properties,
                                          std::shared_ptr<mqtt::auth::AuthorizationRequest>* pending) {
    if (!bridge_) {
        LOG_ERROR("Bridge is null in WebSocketHandlerAdapter");
        return MQ_ERR_PARAM_V2;
    }

    // mqtt 子协议和 TCP 共用报文上限、接收窗口和报文标识符。
    // 授权仍走桥上原来的读权限复查，撤销后的订阅不能继续收到消息。
    if (bridge_->get_message_format() == MessageFormat::MQTT_PACKET) {
        if (!bridge_->outbound_read_allowed(client_id_, mqtt::from_mqtt_string(topic))) {
            return MQ_ERR_CONNECT_NOT_AUTHORIZED;
        }
        return mqtt::MQTTProtocolHandler::send_publish(topic, payload, qos, retain, dup, properties,
                                                       pending);
    }
    (void)pending;

    // Create a PublishPacket to forward to the bridge
    mqtt::PublishPacket packet(adapter_allocator_);
    packet.topic_name = topic;
    packet.payload = payload;
    packet.qos = qos;
    packet.retain = retain;
    packet.dup = dup;
    // Note: properties are not currently used in the bridge

    // Forward to the WebSocket bridge, which will send it to the WebSocket client
    int ret = bridge_->handle_mqtt_publish(client_id_, packet);

    if (ret != MQ_SUCCESS) {
        LOG_ERROR("Failed to forward message to WebSocket client {}: {}", client_id_, ret);
    } else {
        LOG_DEBUG("Forwarded PUBLISH to WebSocket client {} (topic: {}, qos: {})",
                  client_id_, mqtt::from_mqtt_string(topic), qos);
    }

    return ret;
}

}  // namespace websocket
