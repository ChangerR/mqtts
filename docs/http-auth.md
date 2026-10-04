# Optional HTTP authentication and authorization (contract v1)

MQTTS is a standalone MQTT broker. It builds and runs without any application
repository, application database, or HTTP service. Its default development
configuration has authentication disabled; SQLite and Redis providers can enforce
local credentials and topic ACLs. HTTP is an optional policy provider, chosen by
the operator. Applications own their users, roles, resources and message formats.
The broker does not interpret application JSON or reserve application topic names.

## Configuration

See [config-examples/http-auth.yaml](../config-examples/http-auth.yaml). TCP MQTT
and MQTT over WebSocket share port 1883; browsers use `/mqtt`. Both MQTT 3.1.1
and MQTT 5 authenticate CONNECT and authorize SUBSCRIBE, PUBLISH, and **every
outbound delivery**, including an already established subscription. Legacy
JSON/text WebSocket commands are rejected when authentication is enabled.

`auth.enabled: true` initializes configured providers before opening the listener.
Missing, unknown or invalid providers stop startup. Anonymous fallback is disallowed
on authenticated listeners. The HTTP endpoint may start later; it must be reachable
before clients can authenticate or exchange authorized traffic. An application
outage affects clients using that policy provider, not the broker's ability to start.

Set the variable named by `token_env` (the example uses `MQTTS_HTTP_AUTH_TOKEN`)
to a protected secret of at least 32 characters, matching the policy service.
Alternatively use `token_file` to load its first line from a mounted secret. Do not
commit secrets. Configure callbacks on a private network. HTTPS verifies the
server certificate and hostname; `ca_file` adds a custom trust bundle. Redirects
are not followed. `timeout_ms` defaults to 2000 and permits 100–10000 milliseconds.
Unknown settings and invalid payload modes/limits fail startup.

## Contract

The callback uses POST JSON and the `X-Broker-Token` header. The URLs are entirely
configurable; the broker assumes no application routes. This document defines
version 1 of the contract. Additive optional fields do not change existing topic-only
clients; incompatible changes require a new contract version and consumer opt-in.

| Request | Fields | Successful response |
| --- | --- | --- |
| Authentication | `username`, `password`, `clientid` | HTTP 200, `{"result":"allow","expire_at":<Unix seconds>}` |
| Authorization | `username`, `clientid`, `action` (`publish`/`subscribe`), `topic` | HTTP 200, `{"result":"allow"}` |

Outbound delivery uses `action: subscribe` with the concrete destination topic.
`expire_at` is optional. When present, expiration blocks further authorization
and delivery on the existing connection; it does not promise an immediate TCP
disconnect. Clients renew credentials with their own identity service and reconnect.
The provider never grants superuser status and never caches authorization.
Timeout, malformed/oversized response, non-200 status, unavailable callback and
expired credentials deny access. Responses are bounded to 8192 bytes.

### Optional opaque publish payload

The default `publish_payload: none` sends only MQTT identity, action and topic.
Arbitrary binary payloads pass through unchanged, subject to the returned ACL.

For an application that needs content-based policy, set `publish_payload: base64`.
PUBLISH callbacks additionally carry `payload_encoding: "base64"` and `payload`,
the standard padded Base64 encoding of the **entire original byte sequence**.
Empty payloads encode to `""`. SUBSCRIBE/delivery/authentication callbacks never
contain payload bytes. MQTTS does not parse JSON, extract sender fields, or assume
any topic namespace. Only the external policy service decides what bytes mean.

`max_payload_bytes` defaults to 1048576 and accepts 1–16777216. Oversized publishes
are denied rather than truncated or authorized without payload. Request capacity
is 8192 bytes of metadata plus the configured Base64 capacity. In topic-only mode
the request limit stays 8192 and this payload limit does not apply to MQTT data.
Enable payload forwarding only when the selected policy service needs the message
content; protect that endpoint and avoid logging callback bodies or credentials.
Base64 adds roughly one third to payload size, so include that cost in capacity tests.

## Independent build and distribution

The broker's CI builds/tests this repository alone and exports a Docker-loadable
`mqtts-image-<source-sha>-linux-amd64` artifact. It contains `mqtts-image.tar.gz`,
`SHA256SUMS` and `metadata.json` with the source revision and image name. Consumers
can download a chosen artifact, verify the checksum and use `docker load`; no
broker source checkout or C++ toolchain is needed in their repository. CI artifacts
expire after 90 days. For durable distribution, a `v*` Git tag triggers a GitHub
release with the same files **after native tests and the container smoke test pass**.
An operator can also tag/push the built image to their own registry.

```sh
# In this repository only; no application checkout is needed.
docker build -t mqtts:my-version .
# In any deployment directory, after downloading the release or CI artifact:
sha256sum -c SHA256SUMS
docker load --input mqtts-image.tar.gz
# Use the image name in metadata.json or retag/push it to your registry.
```

Applications own their integration config, adapters, compatibility tests and
chosen broker version. Updating an application does not rebuild or release MQTTS;
updating MQTTS does not require changes to an application if contract v1 is preserved.

Public deployments need a TLS terminator in front of the native TCP/WS listener,
with a publicly trusted certificate for the advertised `mqtts://` / `wss://`
hostname. The native listener does not terminate TLS. Keep its plain port private
and preserve the WebSocket upgrade. Do not disable certificate checks.

Build prerequisites include libcurl development headers and `nlohmann-json3-dev`,
in addition to protobuf, yaml-cpp, SQLite, hiredis, OpenSSL, llhttp, CMake, Ninja and
a C++ compiler. Clone recursive submodules. If the distribution does not package
llhttp, `bash bin/install-llhttp.sh /absolute/prefix` installs the pinned release;
add its `lib/pkgconfig` and `lib` to `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH`.
On a Linux cloud VM build natively (see AGENTS.md):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE='-O1 -g0 -UNDEBUG' \
  -DCMAKE_C_FLAGS_RELEASE='-O1 -g0 -UNDEBUG'
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure --timeout 120
```

Assertions remain enabled for assert-based tests. A local HTTP fixture exercises
MQTT 3.1.1/5 over TCP/WS: credential/client binding, wildcard isolation, byte-for-byte
binary forwarding, opt-in payload callbacks, payload limits, online revocation,
expiry and fail-closed startup/callback failures. No application code or database
is required by these tests.
