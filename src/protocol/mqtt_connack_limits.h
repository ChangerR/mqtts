#pragma once

#include <cstdint>
#include "mqtt_config.h"
#include "mqtt_packet.h"

namespace mqtt {

// 按冻结的协议限额填写 CONNACK。v5 写出真实能力；3.x 没有属性，调用方仍须把解析器设成 3.x。
// 保活改写同时决定 Server Keep Alive 通告和连接上的保活计时。
inline void apply_connack_capabilities(ConnAckPacket& packet, const ProtocolLimits& limits,
                                       uint8_t protocol_version, uint16_t client_keep_alive,
                                       bool client_requested_session_expiry, bool persistence_enabled)
{
  if (!persistence_enabled) {
    packet.session_present = false;
  }
  if (protocol_version < 5) {
    return;
  }

  uint8_t maximum_qos = limits.max_qos;
  if (persistence_enabled && maximum_qos > 1) {
    maximum_qos = 1;
  }
  if (maximum_qos < 2) {
    packet.properties.maximum_qos = maximum_qos;
    packet.properties.has_maximum_qos = true;
  }

  packet.properties.retain_available = limits.retain_available;
  packet.properties.wildcard_subscription_available = limits.wildcard_subscription_available;
  packet.properties.subscription_identifier_available = limits.subscription_identifier_available;
  packet.properties.shared_subscription_available = limits.shared_subscription_available;
  packet.properties.maximum_packet_size = limits.max_packet_size;
  packet.properties.receive_maximum = limits.receive_maximum;
  packet.properties.topic_alias_maximum = limits.topic_alias_maximum;

  if (limits.keep_alive_max > 0 &&
      (client_keep_alive == 0 || client_keep_alive > limits.keep_alive_max)) {
    packet.properties.server_keep_alive = limits.keep_alive_max;
  }

  if (!persistence_enabled && client_requested_session_expiry) {
    packet.properties.has_session_expiry_interval = true;
    packet.properties.session_expiry_interval = 0;
  }
}

}  // namespace mqtt
