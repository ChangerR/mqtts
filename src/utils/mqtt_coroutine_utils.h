#ifndef MQTT_COROUTINE_UTILS_H
#define MQTT_COROUTINE_UTILS_H

#include "mqtt_runtime.h"

namespace mqtt {

/**
 * @brief 协程友好的锁类型。具体运行时实现隐藏在 runtime 层。
 */
using CoroMutex = runtime::AsyncMutex;

/**
 * @brief 协程锁的RAII包装器。
 */
using CoroLockGuard = runtime::AsyncLockGuard;

using CoroCondition = runtime::AsyncCondition;

}  // namespace mqtt

#endif  // MQTT_COROUTINE_UTILS_H
