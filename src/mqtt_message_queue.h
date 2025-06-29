#ifndef MQTT_MESSAGE_QUEUE_H
#define MQTT_MESSAGE_QUEUE_H

#include <chrono>
#include "mqtt_parser.h"

namespace mqtt {

/**
 * @brief 待发送的消息结构
 */
struct PendingMessage
{
  PublishPacket packet;                             // 要发送的PUBLISH包
  MQTTString target_client_id;                      // 目标客户端ID
  MQTTString sender_client_id;                      // 发送者客户端ID
  std::chrono::steady_clock::time_point timestamp;  // 消息时间戳

  PendingMessage(const PublishPacket& p, const MQTTString& target, const MQTTString& sender)
      : packet(p),
        target_client_id(target),
        sender_client_id(sender),
        timestamp(std::chrono::steady_clock::now())
  {
  }
};

}  // namespace mqtt

#endif  // MQTT_MESSAGE_QUEUE_H