# 阶段 0 性能基准

2026-10-16，在本机 Linux VM 上用 `build-container/mqtts`（RelWithDebInfo，gcc）重跑历史基准，并补上保留消息两项新基准。原始数字在 [phase0-benchmark.json](phase0-benchmark.json)。

机器是 4 vCPU、16 GiB。持久化基准的数据目录在工作区盘上：`findmnt` 显示 **ext4**（`/dev/vdb`），`stat -f` 报告 `ext2/ext3`。历史 [durable-concurrency.md](durable-concurrency.md) 的数字来自 **overlayfs**。两边都不是 tmpfs，但磁盘不同，绝对吞吐不能当成代码加速来读。下面只用它们检查阶段 0 的门槛。

所有 MQTT 5 订阅都设了 **No Local = 1**，避免阶段 0 把“发布者收得到自己的消息”做成默认之后，扇出比历史多 1。JSON 里的 `subscriber_no_local` 为 1。`topic_alias_maximum` 仍是 65535，没有改成 16。

门槛沿用规格第 5 节，没有放宽：

- 固定 1,000/s 和 3,000/s：收端 p99 恶化不超过 10%（测量值 / 历史值 ≤ 1.10）。
- 饱和吞吐下降不超过 5%（测量值 ≥ 历史值的 95%）。
- retain = 0 的发布路径与同一进程里的空保留库对比，要求无实质差异，并用同一条 10% p99 带来判定。

## 固定速率与饱和吞吐

历史对照是 [durable-concurrency.md](durable-concurrency.md) 里的表，脚本是 `unittest/durable_latency.py` 和 `unittest/durable_concurrency.py`。发布到收端的延迟含客户端调度。载荷 4 KiB，QoS 1，持久化，`fdatasync` 之后才可见。

| 负载 | 历史收端 p99 | 本次 p99 | 比值 | 门槛 |
| --- | ---: | ---: | ---: | --- |
| 1,000 连接，1,000/s | 5.894 ms | 3.500 ms | 0.59 | 通过 |
| 1,000 连接，3,000/s | 38.879 ms | 4.149 ms | 0.11 | 通过 |

| 饱和阶段 | 历史条/s | 本次条/s | 门槛 |
| --- | ---: | ---: | --- |
| 延迟脚本，窗口 4 | 7,201 | 16,967 | 通过 |
| 500 对，授权缓存热 | 7,800 | 17,457 | 通过 |
| 500 对，授权不可用、缓存仍有效 | 8,211 | 16,393 | 通过 |
| 128 发布者扇入 1 个通配消费者，缓存热 | 6,405 | 10,316 | 通过 |
| 同上，授权不可用 | 6,113 | 10,476 | 通过 |

1,000/s 与 3,000/s 都交满了（30,000 与 90,000 条，零失败）。p99 比历史低，饱和吞吐比历史高。ext4 上的 `fdatasync` 比当时的 overlayfs 便宜，所以幅度不能当成阶段 0 的优化成果。相对门槛，没有恶化。

## gRPC 授权

历史表在 [grpc-concurrency.md](grpc-concurrency.md)。该文档的复现命令是 `unittest/grpc_auth_stress.py`（500 对、4,000/s、60 秒、队列 1024），不是同目录下更短的 `grpc_auth_concurrency.py`。本次按文档命令重跑，订阅同样带 No Local = 1。表里的延迟是调度延迟（从预定发送时刻算起）。

| 场景 | 历史 P95 / P99 | 本次 P95 / P99 | 门槛 |
| --- | ---: | ---: | --- |
| 同时 CONNECT 1000/1000 | 1263 / 1266 ms | 1235 / 1261 ms | 通过 |
| 在线 4000/s，60 秒 | 8.36 / 27.99 ms | 2.42 / 6.46 ms | 通过 |
| 授权进程暂停，4000/s，15 秒 | 5.15 / 13.69 ms | 2.54 / 7.05 ms | 通过 |
| 每次发布都未命中缓存，1000/s | 5.87 / 12.69 ms | 4.26 / 8.18 ms | 通过 |
| 1 个发布者，499 个订阅者 | 23.78 / 27.73 ms | 12.42 / 13.11 ms | 通过 |

成功阶段都是足量送达：240,000、60,000、10,000，扇出 49,900。800 ms 慢调用阶段 498 次冷发布全部拒绝，授权批次 5（脚本上限 6）。回归检查通过。

## 新基准：保留库

脚本是 `unittest/retain_benchmark.py`。内存模式、无认证、2 个事件线程，保留上限用产品默认：10 万条、单次订阅最多下发 1,000 条。订阅 No Local = 1。

**retain = 0 的发布路径。** 同一对 QoS 1 连接、4 KiB 载荷、各 2,000 次。先在空保留库上测，再写入 10 万条别的主题，然后重测。retain = 0 不应拿保留库的锁。

| 保留库 | 收端 p50 | p99 |
| --- | ---: | ---: |
| 空 | 1.129 ms | 1.382 ms |
| 已有 10 万条 | 1.131 ms | 1.399 ms |

p99 比值 1.012。落在 10% 带内，和空库没有实质差别。

**10 万条时的通配符下发。** 主题 `store/000000` … `store/099999`，订阅 `store/#`。匹配会走完整棵树，再按主题名排序，默认上限只下发前 1,000 条（`store/000000` 到 `store/000999`），retain 位为 1。15 次订阅：

| 指标 | p50 | p99 |
| --- | ---: | ---: |
| SUBSCRIBE 到第一条 | 70.5 ms | 100.3 ms |
| SUBSCRIBE 到第 1000 条 | 94.8 ms | 126.6 ms |
| SUBACK 到第 1000 条 | 94.7 ms | 126.4 ms |

15 个样本的 p99 就是样本最大值。监控计数 `retained_messages = 100000`，`retained_delivery_truncated = 15`。

## 复现

```sh
cmake -S . -B build-container -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-container --target mqtts -j"$(nproc)"
mkdir -p "$PWD/run/journal-bench"
stat -f -c '%T' "$PWD/run/journal-bench"
TMPDIR="$PWD/run/journal-bench" python3 unittest/durable_latency.py \
  --broker "$PWD/build-container/mqtts" --pairs 500 --seconds 30 \
  --rates 1000,3000 --report /tmp/journal-latency.json
TMPDIR="$PWD/run/journal-bench" python3 unittest/durable_concurrency.py \
  --broker "$PWD/build-container/mqtts" --pairs 500 --messages 128 \
  --report /tmp/journal-concurrency.json
TMPDIR="$PWD/run/journal-bench" python3 unittest/durable_concurrency.py \
  --broker "$PWD/build-container/mqtts" --pairs 128 --messages 64 --fan-in \
  --report /tmp/journal-fanin.json
go -C modules/authz build -o /tmp/mqtts-authz-fixture ./integration/fixture
python3 unittest/grpc_auth_stress.py --broker "$PWD/build-container/mqtts" \
  --authz-fixture /tmp/mqtts-authz-fixture --pairs 500 --seconds 60 \
  --outage-seconds 15 --rate 4000 --cold-seconds 10 --cold-rate 1000 \
  --payload-bytes 4096 --fanout-messages 100 --queue-capacity 1024 \
  --report /tmp/mqtt-grpc-stress.json
python3 unittest/retain_benchmark.py --broker "$PWD/build-container/mqtts" \
  --retained 100000 --samples 2000 --trials 15 --report /tmp/retain-bench.json
```
