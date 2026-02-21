# MQTTS WebSocket 实现审计报告

## 结论

1. 当前项目**没有实现 MQTT over WebSocket**（没有 HTTP Upgrade / WebSocket 握手与帧编解码逻辑）。
2. 现有实现是纯 TCP MQTT（`AF_INET` + `SOCK_STREAM` + 直接 `recv/send` MQTT 字节流）。
3. 兼容性方面：可兼容传统 MQTT/TCP 客户端，但**不兼容**要求 `ws://` 或 `wss://` 的浏览器或 WebSocket 客户端。
4. 内存泄露风险方面：
   - WebSocket 相关逻辑不存在，因此不存在 WebSocket 专属帧缓冲泄露路径；
   - 但协程生命周期管理存在潜在风险（客户端协程创建后未见对应 `co_release`），建议专项压测验证。

## 关键证据

- 代码中未检索到 `websocket` / `Upgrade` / `Sec-WebSocket-*` / 帧掩码等关键词。
- 服务端 socket 使用 `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` 创建 TCP 套接字。
- 连接后直接使用 `recv/send` 读写，不包含 HTTP 握手或 WS 帧封装/拆包。

## 建议

1. 若目标支持浏览器接入，需新增 MQTT over WebSocket 适配层（握手、帧处理、ping/pong、关闭帧）。
2. 若短期不支持，请在 README 与配置中明确“仅支持 MQTT/TCP”。
3. 对协程连接风暴场景做长时压测，关注协程对象与栈内存曲线。
