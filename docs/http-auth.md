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
before clients can authenticate or obtain new grants. Optional bounded local leases
can keep already authorized traffic flowing during a policy-service outage.

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
The provider never grants superuser status. Authorization caching defaults to off.
Timeout, malformed/oversized response, non-200 status, unavailable callback and
expired credentials deny new access. With explicit cache consent, unavailable
callbacks preserve existing leases only until their original deadline. Responses
are bounded to 8192 bytes. Return HTTP 200 `deny` for a real policy rejection and
HTTP 503 for unavailable policy dependencies; these have different cache semantics.

### Optional opaque publish payload

The default `publish_payload: none` sends only MQTT identity, action and topic.
Arbitrary binary payloads pass through unchanged, subject to the returned ACL.

For an application that needs content-based policy, set `publish_payload: base64`.
PUBLISH callbacks additionally carry `payload_encoding: "base64"` and `payload`,
the standard padded Base64 encoding of the **entire original byte sequence**.
Empty payloads encode to `""`. SUBSCRIBE/delivery/authentication callbacks never
contain payload bytes. MQTTS does not extract sender fields or assume any topic
namespace. Only the external policy service decides what bytes mean.

`max_payload_bytes` defaults to 1048576 and accepts 1–16777216. Oversized publishes
are denied rather than truncated or authorized without payload. Request capacity
is 8192 bytes of metadata plus the configured Base64 capacity. In topic-only mode
the request limit stays 8192 and this payload limit does not apply to MQTT data.
Enable payload forwarding only when the selected policy service needs the message
content; protect that endpoint and avoid logging callback bodies or credentials.
Base64 adds roughly one third to payload size, so include that cost in capacity tests.

## Optional local authorization leases

The HTTP provider has its own cache; the legacy `auth.cache_enabled` login cache
does not control it. CONNECT is always authenticated online, including reused
usernames/client IDs and wrong passwords. Each successful CONNECT gets a new
internal session identity. Cache keys bind that session, username, client ID,
action and topic. Payload mode additionally binds a SHA-256 digest of the bytes.

Both operator and policy service must consent. Set `cache_ttl_ms` above zero and
return these optional fields on an authorization decision:

```json
{"result":"allow","cache_ttl_ms":10000,"cache_max_age_ms":300000,"cache_revision":"opaque-version"}
```

Fresh hits use a sharded in-process LRU without HTTP/Redis. After the fresh interval,
an allowed hit returns locally and triggers one background refresh for that key.
Failures use a cooldown and never advance the existing deadline. Successful
refreshes can renew the lease, bounded by the original CONNECT expiration.
Explicit `deny` removes an allowed lease; negative caching lasts at most one second
and is never extended on outage. Cold misses and expired grants fail closed.

| Setting | Default | Bounds / meaning |
| --- | --- | --- |
| `cache_ttl_ms` | 0 (off) | 0–300000; upper bound on fresh interval |
| `cache_max_age_ms` | 300000 | 1–300000; total lease age, not extra stale time |
| `cache_max_entries` | 16384 | 16–1048576; split across 16 LRU shards |
| `http_workers` | 4 | 1–16 persistent libcurl workers |
| `http_queue_capacity` | 64 | 1–4096 queued jobs |
| `http_queue_bytes` | 16777216 | 8192–134217728 queued body bytes |
| `failure_cooldown_ms` | 1000 | 100–30000; retry/circuit cooldown |
| `cache_version_url` | unset | optional protected HTTP POST revision feed |
| `cache_version_interval_ms` | 250 | 100–60000; delay between revision polls |

The lesser of operator limits, response limits, response `expire_at` and original
CONNECT expiry applies. Omitting `cache_max_age_ms` in a response permits only the
fresh interval, with no extra stale window. Queue waiting counts against
`timeout_ms`; overflow is denied. A circuit opens after three unavailable/malformed
HTTP responses and admits one recovery probe after cooldown. Workers reuse HTTP
connections, and coroutine waiters yield through the runtime; slow cold requests
do not block unrelated MQTT traffic. The revision feed has a reserved worker.
`get_stats()` exposes hit/miss/stale/eviction, HTTP request/failure/rejection,
refresh and circuit counters.

The optional version endpoint receives POST `{}` and the same `X-Broker-Token`.
It returns `{"cache_revision":"opaque-version"}` (1–128 bytes). Change the version
after committed policy mutations; capture it **before** evaluating each policy
response. An observed change clears local grants; older in-flight responses cannot
restore them. Propagation takes the poll interval plus network/service time and is
not an atomic distributed revocation. Feed failures leave bounded leases intact.
Periodic refresh remains necessary for lost notifications. The broker knows no
application database, message schema, invalidation routes or revision storage.

### Ignoring non-policy payload fields

`publish_cache_ignored_fields` is an optional JSON-array **string**, used only with
payload forwarding and caching. For example `'["nonce","data"]'` removes those
top-level JSON fields before hashing the rest. It does not alter the forwarded
message or callback payload. Only configure fields that the policy service never
uses to authorize. Otherwise an operator can accidentally authorize changed content.
Identity, route fields, unknown keys and aliases must stay bound to the decision.
Without this option the entire payload is hashed. Invalid/non-object JSON,
duplicate keys or nesting deeper than 64 use the original byte digest. No field
names or application semantics are hardcoded into MQTTS.

Run the isolated cache/failure benchmark with:

```sh
python3 unittest/http_auth_cache_integration.py --broker build/mqtts \
  --messages 10000 --report /tmp/mqtt-cache-report.json
```

It checks actual delivered bytes, HTTP request counts, original expiry, revocation,
LRU eviction and a single MQTT event thread under slow cold requests. Reported
latencies are local sequential QoS 0 round trips, not maximum broker capacity.

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
