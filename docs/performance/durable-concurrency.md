# Durable delivery concurrency

Native Linux development VM: 4 vCPU quota, 16 GiB memory, two broker MQTT event
threads and one SQLite disk worker. SQLite uses WAL and `synchronous=FULL` on the
VM's local filesystem. These are finite saturated-load measurements on shared
hardware, not a production SLA or PostgreSQL ingestion throughput.

Command:

```sh
python3 unittest/durable_concurrency.py --broker "$PWD/build-native/mqtts" \
  --pairs 500 --messages 128 --payload-bytes 4096 --report /tmp/durable.json
```

The test opens 500 publishers and 500 TCP persistent consumers, keeps up to four
publications outstanding per publisher, and verifies 64,000 actual deliveries in
each phase (including payload, identity, topic, sequence and absence of duplicates).
Every positive publisher receipt follows a disk commit. Reported latency is from
publish start to reception, and includes queueing under the saturated load.

| Phase | Connections | Delivered | Messages/s | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Authorization available, warm cache | 1,000 | 64,000 | 5,793 | 199 ms | 647 ms | 989 ms |
| Authorization unavailable, valid cache | 1,000 | 64,000 | 5,910 | 212 ms | 621 ms | 919 ms |

Both phases made zero remote authorization calls after warmup. This establishes
availability inside the existing cache lease; it does not extend an expired lease.
Queue admission is bounded to 1,024 operations / 16 MiB of reservations. Reads leave
room for writes and ACKs; a write can wait cooperatively up to 500 ms for admission.
The test asserts every attempted transfer completes, rather than counting socket
writes as successful delivery. Raw data: [durable-concurrency.json](durable-concurrency.json).

A separate `--fan-in` run uses 128 publishers and one persistent consumer through a
wildcard subscription (129 connections, 8,192 delivered messages per phase):

| Phase | Messages/s | p95 | p99 |
| --- | ---: | ---: | ---: |
| Warm authorization | 3,844 | 166 ms | 177 ms |
| Authorization unavailable, valid cache | 4,135 | 136 ms | 144 ms |

CI runs this same fan-in shape. It guards against single-client
ACK serialization and simultaneous read/write wakeup loss. It is a separate shape
of load: the many-consumer throughput above must not be presented as the throughput
of one message-ingest process or of PostgreSQL.

```sh
python3 unittest/durable_concurrency.py --broker "$PWD/build-native/mqtts" \
  --pairs 128 --messages 64 --fan-in --report /tmp/durable-fanin.json
```

Recovery correctness is checked independently with real process crashes, unacknowledged
replay, changed receive windows, ACL revocation, capacity overflow and real SQLite
write-lock failures. See [persistence.md](../persistence.md) for limits and semantics.
