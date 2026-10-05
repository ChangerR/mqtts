# Native journal concurrency and latency

Measured on the shared Linux development VM with a 4 vCPU quota, 16 GiB memory,
two MQTT event threads, four message-log workers and four session-log workers.
Transport is loopback TCP without TLS. Payload content is 4 KiB; QoS 1 delivery becomes visible only after `fdatasync`.
All measured publications must receive success and actually arrive with correct
payload, identity, topic, monotonic per-topic sequence and no duplicate delivery.

**Filesystem correction:** earlier SQLite figures used Python's default temporary
directory, which this VM mounts as **tmpfs**. Those numbers measure a synthetic
in-memory filesystem workload, not physical disk durability throughput. The corrected
historical data is retained in [sqlite-durable-baseline.json](sqlite-durable-baseline.json).
It must not be compared directly to the disk-backed figures below as a hardware I/O
speedup. These new measurements explicitly use the workspace **overlayfs**, not tmpfs.
The VM filesystem/host still does not establish production storage latency or an SLA.

## Fixed offered load, including checkpoints

500 publishers and 500 persistent consumers remain connected. Each rate runs for
30 seconds; the same broker continues between rates, crossing the normal 30-second
checkpoint interval. The load generator caps outstanding tasks at four per publisher,
records scheduling lag separately, and waits for every attempted transfer to arrive.
Latency starts when the client begins a publication and ends when its subscriber
callback observes the full message. It includes client scheduling and broker queueing.

| Connections / workload | Delivered | Measured messages/s | Receive p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Two connections, sequential baseline | 256 | 389 | 2.75 ms | 3.49 ms | 4.06 ms |
| 1,000 connections, offered 1,000/s | 30,000 | 1,000 | 1.99 ms | 3.29 ms | 5.89 ms |
| 1,000 connections, offered 3,000/s | 90,000 | 2,999 | 2.34 ms | 7.08 ms | 38.88 ms |
| Same clients, saturated window 4 | 64,000 | 7,201 | 96.46 ms | 257.33 ms | 530.75 ms |

The instrumented saturation phase had 221 ms p99 **client event-loop lag**, so its
530.75 ms tail is not a measurement of server internals alone. Fixed-rate results
also include checkpoint pauses and shared-host scheduling. In the 3,000/s phase,
publication-receipt p99 was 41.05 ms and client scheduling-lag p99 was 15.61 ms.
Receipt observation can follow delivery observation even though both occur after
the same committed append, because the two sockets are serviced independently.
Raw data, including all maxima and CPU samples: [durable-latency.json](durable-latency.json).

```sh
mkdir -p "$PWD/run/journal-bench"
# Verify this directory is not tmpfs before interpreting storage throughput.
stat -f -c '%T' "$PWD/run/journal-bench"
TMPDIR="$PWD/run/journal-bench" python3 unittest/durable_latency.py \
  --broker "$PWD/build-native/mqtts" --pairs 500 --seconds 30 \
  --rates 1000,3000 --report /tmp/journal-latency.json
```

## Saturated delivery and authorization isolation

The lighter concurrency harness performs 64,000 deliveries per phase, with 500
publishers, 500 persistent TCP consumers and four in-flight publications per publisher.
The second phase makes the authorization fixture unavailable after caches are warm.

| Phase | Delivered/s | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: |
| Warm authorization | 7,800 | 96.32 ms | 206.55 ms | 275.67 ms |
| Authorization unavailable, valid cache | 8,211 | 89.71 ms | 219.55 ms | 354.61 ms |

Both phases made zero remote authorization requests after warmup. This checks operation
inside the existing cache lease, not renewal or an extension beyond the original expiry.
The lighter and instrumented saturation harnesses show 7,201–8,211 delivered messages/s
on this shared host; the different tails should not be cherry-picked as a latency SLA.

A separate fan-in run uses 128 publishers and one wildcard persistent consumer
(129 connections, 8,192 messages per phase):

| Phase | Delivered/s | p95 | p99 |
| --- | ---: | ---: | ---: |
| Warm authorization | 6,405 | 120.44 ms | 138.84 ms |
| Authorization unavailable, valid cache | 6,113 | 108.23 ms | 128.27 ms |

Topic partitions append independently even for that single subscriber. Its ordered
receive window and ACK stream remain finite capacity constraints. These results do
not measure `message-ingest`'s bbolt queue or PostgreSQL transactions.
Raw data: [durable-concurrency.json](durable-concurrency.json).

```sh
TMPDIR="$PWD/run/journal-bench" python3 unittest/durable_concurrency.py \
  --broker "$PWD/build-native/mqtts" --pairs 500 --messages 128 \
  --report /tmp/journal-concurrency.json
TMPDIR="$PWD/run/journal-bench" python3 unittest/durable_concurrency.py \
  --broker "$PWD/build-native/mqtts" --pairs 128 --messages 64 --fan-in \
  --report /tmp/journal-fanin.json
```

The durability suite independently tests a stalled partition while another partition
keeps delivering, real write/sync errors, SIGKILL replay, checkpoint/reclamation,
ACL revocation, ownership, torn tails/checksum failures, more than 65,535 pending
messages, exact ACK gaps, bounded quotas and read-only SQLite migration.
See [persistence.md](../persistence.md) for the storage contract and limitations.
