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

int WebSocketHandlerAdapter::send_publish(const mqtt::MQTTString& topic,
                                          const mqtt::MQTTByteVector& payload,
                                          uint8_t qos,
                                          bool retain,
                                          bool dup,
                                          const mqtt::Properties& properties) {
    if (!bridge_) {
        LOG_ERROR("Bridge is null in WebSocketHandlerAdapter");
        return MQ_ERR_PARAM_V2;
    }

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
