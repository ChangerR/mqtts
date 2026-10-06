# MQTT / authorization concurrency verification

Measured on 2026-10-04 in an isolated loopback fixture on a shared Linux VM:
4 CPU quota, 16 GiB memory limit, 2 MQTT event threads, 4 gRPC workers, 32-item /
1-ms batches, 500-ms authorization deadline, 16-MiB authorization queue byte
limit, 16,384 cache entries, 10-second freshness and maximum 300-second leases.
The generator, broker and authorization process share the same CPU quota.
Payload content is 4096 bytes, plus JSON metadata. MQTT uses QoS 1.

The earlier 1000-connection measurement ran for only about 3 seconds per phase,
using a 60-second fresh cache, a 256-item queue and incremental connection setup.
It did not establish sustained, cold-authorization or connection-burst capacity.

## Findings and corrections

The [initial expanded run](grpc-concurrency-before.json), against `34af4e1`, used
the application's previous 64-item authorization queue. Of 1000 simultaneous
CONNECT attempts, 297 succeeded and 703 were refused. Controlled retries with
concurrency 8 established all 1000; these retries are excluded from initial
success counts. Even at modest average throughput, reconnect bursts need a
different admission budget from steady cached traffic.

That run also crashed the broker during one-to-499 fanout. The old routing path
copied every recipient's payload against the publishing client's 1-MiB allocator.
It exhausted that limit and let `std::bad_alloc` escape the MQTT coroutine.
Routing now owns **one immutable content snapshot per publication**, using the
session-manager allocator. All recipients share that snapshot, including its
deep-copied properties. It remains valid after the publisher disconnects.
Content is not reused across different publications of the same size. Allocation
failure during content creation/queueing returns an error; failed queue admission
on this routing path is no longer counted as success.
WebSocket publishing translates the recipient count into its success status.

The [corrected run](grpc-concurrency-after.json) uses a 1024-item authorization
queue. This is the consuming chat project's selected configuration; the generic
provider default remains 64. The byte limit, deadline and worker count are
unchanged. A larger queue absorbs bursts; it does not raise downstream service
throughput or guarantee admission when the service is slow.

## Results after the correction

| Scenario | Offered / received | Duration | P95 / P99 latency |
| --- | --- | --- | --- |
| Simultaneous CONNECT | 1000 / 1000, no retry | 1.291 s total | 1263 / 1266 ms |
| 1000 online, 500 publishers, 4000 messages/s | 240000 / 240000 | 60 s | 8.36 / 27.99 ms |
| Authorization process paused, 4000 messages/s | 60000 / 60000 | 15 s | 5.15 / 13.69 ms |
| Every publish has a new authorization cache key, 1000/s | 10000 / 10000 | 10 s | 5.87 / 12.69 ms |
| One publisher, 499 subscribers | 100 publishes / 49900 deliveries | 2.189 s | 23.78 / 27.73 ms |

Paced-phase latency starts at the **intended send time**, including generator
lateness. Actual-send latency is also retained in JSON. Each publisher has at most
four outstanding messages; generator backpressure, publish denial, client errors
and missing delivery are counted separately, never silently retried. Successful
phases had none of these failures. Receivers check the complete payload, identity,
unique sequence and per-recipient ordering. Fanout is a separate closed-loop
measurement with one group publish outstanding, not a sustained open-loop SLO.

The sustained phase crossed five cache freshness intervals and performed 5206
authorization decisions in 2003 batches (maximum batch 32). It used approximately
0.57 broker CPU cores, 0.013 authorization CPU cores and 0.62 generator CPU cores;
sampled peak RSS was 63.9 / 22.4 / 56.2 MiB respectively. Resource samples are
every 250 ms and do not establish an absolute memory ceiling or leak-free soak.

The paused-service phase lasts longer than freshness, exercising stale positive
leases and failed refreshes. It remains inside the original five-minute session
lease. A new uncached probe confirms recovery before measuring healthy cold
traffic. The initial run began cold traffic without that probe and saw five
denials during recovery; these remain visible in the original report.

After a policy revision invalidated caches, an 800-ms authorization delay exceeded
the 500-ms deadline. All 498 uncached publications were denied; a rewarmed pair
delivered 200/200 messages meanwhile (P95 7.49 ms including scheduling). Only six
authorization batches reached the service. The broker stayed alive, recovered
after delay removal and rejected the revoked subscription. Fail-closed overload
is deliberate; it is not evidence of lossless cold traffic during an outage.

## Reproduce

Build the native broker as described by the repository, then:

```sh
go -C modules/authz build -o /tmp/mqtts-authz-fixture ./integration/fixture
python3 unittest/grpc_auth_stress.py --broker "$PWD/build-native/mqtts" \
  --authz-fixture /tmp/mqtts-authz-fixture --pairs 500 --seconds 60 \
  --outage-seconds 15 --rate 4000 --cold-seconds 10 --cold-rate 1000 \
  --payload-bytes 4096 --fanout-messages 100 --queue-capacity 1024 \
  --report /tmp/mqtt-grpc-stress.json
```

CI runs a smaller 256-connection, 16-KiB-payload version, including cache refresh,
outage, cold authorization, revision/delay isolation and 127-recipient fanout.
The 16-KiB fanout also exceeds the former 1-MiB aggregate-copy limit. A native
unit regression holds 500 references to a 64-KiB message, destroys the publisher
allocator, checks all nested property allocators and then releases the message.
The fixture exposes test-only loopback fault injection; it is not in the image.

## What is not established

These are workload-specific observations, not the maximum or a production SLA.
No 10000-connection claim follows from them. The transport is local TCP and
plaintext gRPC, without MQTTS/WSS handshake cost, cross-host latency, application
database persistence, slow WAN recipients or a multi-hour soak. The fixture's
connection ceiling is 2048 and per-client allocation limit 1 MiB; the chat
configuration uses 1000 and 4 MiB respectively. The actual measured population
is 1000 connections, including publisher and receiver connections, not 1000
independent users plus additional agents/backend connections.

Authorization still has one authoritative store, not replicated HA. Large
control-plane populations require separate measurement: application policy
projection currently reads business permissions sequentially; namespace paging
sorts matching records; expired records are reaped at most 128 per second. These
can constrain policy propagation/login churn even while cached MQTT delivery is
healthy. Five-minute expiry safety is covered by unit/functional tests; this
short load run does not measure a full five-minute outage at scale.
