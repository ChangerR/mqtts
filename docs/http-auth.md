# HTTP authentication and live authorization

`auth.enabled: true` now initializes the configured providers before opening the
listener. Missing, unknown, disabled, or unhealthy providers stop startup.
Anonymous fallback is disallowed on authenticated listeners. With authentication
disabled the existing unauthenticated development behavior is unchanged.

Use [config-examples/openclaw.yaml](../config-examples/openclaw.yaml) for
[OpenClaw Bot Chat](https://github.com/WorkClawDev/openclaw-bot-chat). TCP MQTT
and MQTT over WebSocket share port 1883; browsers use `/mqtt`. Both MQTT 3.1.1
and MQTT 5 authenticate CONNECT and authorize SUBSCRIBE, PUBLISH, and **every
outbound delivery**, including an already established subscription. Legacy
JSON/text WebSocket commands are rejected when authentication is enabled.

Set `BROKER_SECURITY_CALLBACK_TOKEN` from a protected secret, at least 32
characters, matching the backend. Alternatively use `token_file` to load its
first line from a mounted secret. Do not commit that file. Configure callback
URLs on a private network. HTTPS verifies the server certificate and hostname;
`ca_file` adds a custom trust bundle. Redirects are not followed.

The callback contract uses POST JSON and `X-Broker-Token`:

| Endpoint | Request | Successful response |
| --- | --- | --- |
| Authentication | `username`, `password`, `clientid` | HTTP 200, `{"result":"allow","expire_at":<Unix seconds>}` |
| Authorization | `username`, `clientid`, `action` (`publish`/`subscribe`), `topic` | HTTP 200, `{"result":"allow"}` |

`expire_at` is optional for server identities. When present, expiration blocks
further authorization and delivery on the existing connection; it does not
promise an immediate TCP disconnect. Clients must renew their scoped session
through the application and reconnect. The provider never grants superuser
status and never caches authorization. Timeout, malformed/oversized response,
non-200 status, unavailable callback, and expired credentials deny access.
`timeout_ms` defaults to 2000 and permits 100–10000 milliseconds. Callback
responses and requests are bounded to 8192 bytes.

For OpenClaw set `message_identity_prefix: "chat/"`. For those topics the
payload must be a JSON object. The broker extracts only `from`, `sender_type`,
`sender_id`, `conversation_id`, and `topic` into the authorization request's
`message` field, allowing the backend to bind the claimed author to CONNECT
credentials. Content is not forwarded to the authorization endpoint. This
payload validation is opt-in; other MQTT applications can omit the prefix to
keep arbitrary binary payloads.

Public deployments need a TLS terminator in front of the native TCP/WS listener,
with a publicly trusted certificate for the advertised `mqtts://` / `wss://`
hostname. The native listener does not terminate TLS. Keep its plain port
private and preserve the WebSocket upgrade. Do not disable certificate checks.

Build prerequisites now include libcurl development headers and
`nlohmann-json3-dev`, in addition to protobuf, yaml-cpp, SQLite, hiredis,
OpenSSL, llhttp, CMake, Ninja, and a C++ compiler. Clone recursive submodules.
On a Linux cloud VM build natively (see AGENTS.md):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE='-O1 -g0 -UNDEBUG' \
  -DCMAKE_C_FLAGS_RELEASE='-O1 -g0 -UNDEBUG'
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure --timeout 120
python3 unittest/http_auth_integration.py --broker build/mqtts
```

Assertions remain enabled for the repository's assert-based tests. Integration
tests start a real broker with a local HTTP fixture and use actual TCP/WS
packets; they cover credential/client binding, wildcard isolation, sender
forgery, live revocation, expiry, and fail-closed startup. OpenClaw's companion
acceptance script additionally checks real PostgreSQL/Redis account and group
changes through the application API.
