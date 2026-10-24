#pragma once

#include "mqtt_protocol_handler.h"
#include "websocket_mqtt_bridge.h"
#include "websocket_protocol_handler.h"

namespace websocket {

// Adapter that makes WebSocket handlers compatible with MQTT session manager
class WebSocketHandlerAdapter : public mqtt::MQTTProtocolHandler {
public:
    WebSocketHandlerAdapter(MQTTAllocator* allocator,
                           WebSocketMQTTBridge* bridge,
                           WebSocketProtocolHandler* ws_handler,
                           const std::string& client_id);

    virtual ~WebSocketHandlerAdapter();

    // Override send_publish to forward messages to WebSocket clients
    virtual int send_publish(const mqtt::MQTTString& topic,
                            const mqtt::MQTTByteVector& payload,
                            uint8_t qos = 0,
                            bool retain = false,
                            bool dup = false,
                            const mqtt::Properties& properties = mqtt::Properties(),
                            std::shared_ptr<mqtt::auth::AuthorizationRequest>* pending = nullptr) override;

    // We don't need to override process() since this is only used for message forwarding
    // The actual processing is done by the WebSocketProtocolHandler
    void request_session_takeover(const std::shared_ptr<mqtt::SessionTakeoverGate>& gate) override;

protected:
    int transmit_serialized(const uint8_t* data, size_t size) override;
    int transmit_parts(const uint8_t* head, size_t head_size, const uint8_t* body,
                       size_t body_size) override;

private:
    MQTTAllocator* adapter_allocator_;
    WebSocketMQTTBridge* bridge_;
    WebSocketProtocolHandler* ws_handler_;
    std::string client_id_;
};

}  // namespace websocket
