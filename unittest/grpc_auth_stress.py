#!/usr/bin/env python3
"""Repeatable gRPC pressure/fault probes. No production capacity claim is implied.

Paced phases measure latency from both the intended schedule and the actual send;
they never silently retry publishes or omit generator/receiver failures. All
processes are private, loopback-only and cleaned up, including a paused service.
"""
import argparse
import asyncio
from collections import Counter
import json
import os
from pathlib import Path
import signal
import time
import urllib.request

from grpc_auth_concurrency import fixture
from http_auth_concurrency import AsyncClient


def distribution(values):
    values = sorted(values)
    if not values:
        return {}
    return {name: round(values[min(len(values)-1, int(len(values)*q))], 3)
            for name, q in [('p50_ms', .5), ('p95_ms', .95), ('p99_ms', .99), ('max_ms', 1)]}


def optional_limit(path):
    # Hosted runners need not mount cgroup v2. Missing quota metadata means
    # unknown, not unlimited, and must not prevent exercising the workload.
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def rpc_stats(url, control=None):
    request = urllib.request.Request(url + ('/control' if control is not None else ''),
                                     data=json.dumps(control).encode() if control is not None else None,
                                     headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=3) as response:
        return json.load(response)


class Resources:
    """Linux process CPU/RSS/fds, including the generator sharing this VM."""
    def __init__(self, processes):
        self.processes = processes
        self.first, self.last, self.peaks = {}, {}, {}
        self.started = time.perf_counter()
        self.sample()

    def sample(self):
        for name, pid in self.processes.items():
            root = Path('/proc') / str(pid)
            # comm can contain spaces; fields after ')' start at state (field 3).
            fields = (root/'stat').read_text().rsplit(')', 1)[1].split()
            cpu = (int(fields[11])+int(fields[12])) / os.sysconf('SC_CLK_TCK')
            row = dict(rss_mib=int(fields[21])*os.sysconf('SC_PAGE_SIZE')/1048576,
                       fds=len(list((root/'fd').iterdir())), threads=int(fields[17]))
            self.first.setdefault(name, cpu)
            self.last[name] = cpu
            peak = self.peaks.setdefault(name, row)
            for key in row:
                peak[key] = max(peak[key], row[key])

    async def watch(self):
        while True:
            self.sample()
            await asyncio.sleep(.25)

    def result(self):
        self.sample()
        elapsed = time.perf_counter()-self.started
        return {name: dict(peak_rss_mib=round(row['rss_mib'], 1), peak_fds=row['fds'],
                           peak_threads=row['threads'],
                           cpu_cores=round((self.last[name]-self.first[name])/elapsed, 3))
                for name, row in self.peaks.items()}


class Probe:
    def __init__(self, reader, writer, index, size):
        self.reader, self.writer = reader, writer
        self.topic, self.data = 'fixture/pair-'+str(index), 'x'*size
        self.sequence, self.last_received, self.received = 0, 0, 0
        self.waiters = {}
        reader.on_message = self.receive

    def receive(self, topic, data):
        value = json.loads(data)
        seq = value['nonce']
        assert topic == self.topic and value['actor'] == self.writer.name and value['data'] == self.data
        assert seq > self.last_received, ('duplicate or out-of-order delivery', seq, self.last_received)
        assert seq in self.waiters, ('unexpected delivery', seq)
        self.last_received = seq
        self.received += 1
        self.waiters[seq].set_result(time.perf_counter())

    async def transfer(self, due=None, cold=False):
        self.sequence += 1
        seq = self.sequence
        value = dict(actor=self.writer.name, nonce=seq, data=self.data)
        if cold:
            value['policy_probe'] = seq  # Not an ignored cache field: every publish is a miss.
        future = asyncio.get_running_loop().create_future()
        self.waiters[seq] = future
        start = time.perf_counter()
        try:
            reason = await self.writer.publish(self.topic, json.dumps(value, separators=(',', ':')).encode())
            if reason >= 128:
                return 'publish_denied', None, None
            try:
                received = await asyncio.wait_for(future, 5)
            except asyncio.TimeoutError:
                return 'delivery_timeout', None, None
            return 'delivered', (received-start)*1000, (received-(due or start))*1000
        finally:
            self.waiters.pop(seq, None)


async def paced(label, pairs, seconds, rate, processes, cold=False):
    expected = round(seconds*rate)
    pending, outcomes, samples, scheduled, errors = set(), Counter(), [], [], []
    inflight = Counter()
    resources = Resources(processes)
    monitor = asyncio.create_task(resources.watch())
    start = time.perf_counter()
    def done(task, index):
        pending.discard(task)
        inflight[index] -= 1
        try:
            result, latency, intended = task.result()
            outcomes[result] += 1
            if latency is not None:
                samples.append(latency); scheduled.append(intended)
        except Exception as error:
            outcomes['client_error'] += 1
            if len(errors) < 3: errors.append(repr(error))
    try:
        for i in range(expected):
            due = start+i/rate
            while due > time.perf_counter():
                await asyncio.sleep(min(.002, due-time.perf_counter()))
            index = i % len(pairs)
            if inflight[index] >= 4:
                outcomes['generator_backpressure'] += 1
                continue
            inflight[index] += 1
            task = asyncio.create_task(pairs[index].transfer(due, cold))
            pending.add(task)
            task.add_done_callback(lambda task, index=index: done(task, index))
        if pending:
            await asyncio.gather(*pending, return_exceptions=True)
        await asyncio.sleep(0)  # Run the final completion callbacks.
        elapsed = time.perf_counter()-start
        return dict(scenario=label, offered=expected, offered_per_second=rate,
                    elapsed_s=round(elapsed, 3), outcomes=dict(outcomes), errors=errors,
                    delivered_per_second=round(outcomes['delivered']/elapsed, 1),
                    send_latency=distribution(samples), schedule_latency=distribution(scheduled),
                    resources=resources.result())
    finally:
        monitor.cancel()
        await asyncio.gather(monitor, return_exceptions=True)


async def run(port, authz, broker, url, args, emit):
    names = [kind+'-'+str(i) for i in range(args.pairs) for kind in ['reader', 'writer']]
    clients, failures, latencies = {}, {}, []
    processes = dict(broker=broker.pid, authz=authz.pid, generator=os.getpid())
    async def connect(name):
        start = time.perf_counter()
        try:
            clients[name] = await AsyncClient.connect(port, name)
            latencies.append((time.perf_counter()-start)*1000)
        except Exception as error:
            failures[name] = repr(error)
    try:
        start = time.perf_counter()
        await asyncio.gather(*(connect(name) for name in names))
        burst = dict(scenario='simultaneous CONNECT', attempted=len(names), accepted=len(clients),
                     rejected_or_failed=len(failures), elapsed_s=round(time.perf_counter()-start, 3),
                     accepted_latency=distribution(latencies), examples=list(failures.values())[:3],
                     authentication_rpcs=rpc_stats(url)['connects'])
        # Report the initial refusals. Then controlled recovery creates the
        # steady-state population; these retries never inflate the burst result.
        admission = asyncio.Semaphore(8)
        async def retry(name):
            async with admission:
                clients[name] = await AsyncClient.connect(port, name)
        await asyncio.sleep(1.1)
        await asyncio.gather(*(retry(name) for name in failures))
        burst['connections_after_controlled_retry'] = len(clients)
        emit(burst)
        probes = [Probe(clients['reader-'+str(i)], clients['writer-'+str(i)], i, args.payload_bytes)
                  for i in range(args.pairs)]
        async def warm(pair):
            async with admission:
                assert await pair.reader.subscribe(pair.topic) == 1
                assert (await pair.transfer())[0] == 'delivered'
        await asyncio.gather(*(warm(pair) for pair in probes))
        before = rpc_stats(url)
        row = await paced('sustained, including 10-second cache refresh', probes, args.seconds, args.rate, processes)
        after = rpc_stats(url)
        row['authorization'] = dict(batches=after['batches']-before['batches'], items=after['items']-before['items'], max_batch=after['max_batch'])
        emit(row)
        assert row['outcomes'] == {'delivered': row['offered']}, row
        assert after['items'] > before['items'], 'sustained phase did not cross freshness boundary'

        os.kill(authz.pid, signal.SIGSTOP)
        try:
            row = await paced('authz paused beyond cache freshness', probes, args.outage_seconds, args.rate, processes)
            emit(row)
            assert row['outcomes'] == {'delivered': row['offered']}, row
        finally:
            os.kill(authz.pid, signal.SIGCONT)
        await asyncio.sleep(1.2)

        # A cached hit cannot prove the breaker/transport has recovered. Probe
        # a new scope and report recovery refusals separately from healthy load.
        recovery_start = time.perf_counter()
        recovery_denied = 0
        while (await probes[0].transfer(cold=True))[0] != 'delivered':
            recovery_denied += 1
            assert time.perf_counter()-recovery_start < 5, 'authorization did not recover'
            await asyncio.sleep(.05)
        emit(dict(scenario='authorization recovery after resume',
                  probe_denied=recovery_denied,
                  elapsed_s=round(1.2+time.perf_counter()-recovery_start, 3)))

        before = rpc_stats(url)
        row = await paced('every publish requires gRPC authorization', probes, args.cold_seconds, args.cold_rate, processes, cold=True)
        after = rpc_stats(url)
        row['authorization'] = dict(batches=after['batches']-before['batches'], items=after['items']-before['items'], max_batch=after['max_batch'])
        emit(row)
        assert row['outcomes'] == {'delivered': row['offered']}, row
        assert after['items']-before['items'] >= row['outcomes'].get('delivered', 0), 'cold phase accidentally hit cache'

        # Revision change invalidates every cache. Rewarm only one unaffected
        # pair, then put all other scopes through a 800 ms RPC > 500 ms deadline.
        # The active cached pair must keep moving while cold work is denied.
        rpc_stats(url, dict(revoke='reader-'+str(args.pairs-1)))
        await asyncio.sleep(.6)
        assert (await probes[0].transfer())[0] == 'delivered'
        rpc_stats(url, dict(delay_ms=800))
        before = rpc_stats(url)
        start = time.perf_counter()
        cold = asyncio.gather(*(p.transfer(cold=True) for p in probes[1:-1]))
        warm = await paced('warm pair during slow cold authorization burst', probes[:1], 2, 100, processes)
        results = await cold
        after = rpc_stats(url)
        emit(dict(scenario='cold burst after revision change, 800 ms service delay',
                  cold_attempted=len(results), cold_outcomes=dict(Counter(r[0] for r in results)),
                  elapsed_s=round(time.perf_counter()-start, 3),
                  authorization_batches=after['batches']-before['batches'],
                  authorization_items=after['items']-before['items'], peak_service_rpcs=after['peak_active'], warm=warm))
        assert all(r[0] == 'publish_denied' for r in results), 'slow uncached authorization failed open'
        assert warm['outcomes'] == {'delivered': warm['offered']}, warm
        assert after['batches']-before['batches'] <= 6, 'deadline/circuit did not bound slow RPC traffic'
        rpc_stats(url, dict(delay_ms=0))
        await asyncio.sleep(1.2)
        assert (await probes[1].transfer(cold=True))[0] == 'delivered', 'circuit did not recover'
        emit(dict(scenario='recovery and revocation', recovered=True,
                  revoked_subscribe_denied=await probes[-1].reader.subscribe(probes[-1].topic) >= 128))
        assert await probes[-1].reader.subscribe(probes[-1].topic) >= 128

        # One publisher to many recipients: count individual deliveries and
        # compare every payload, duplicate, and per-recipient sequence.
        readers = probes[:-1]
        for pair in readers:
            assert await pair.reader.subscribe('fixture/fanout') == 1
        seq, received, waiting = 0, Counter(), {}
        def receive(name, topic, payload):
            value = json.loads(payload)
            assert topic == 'fixture/fanout' and value['data'] == 'x'*args.payload_bytes
            assert value['actor'] == 'writer-0' and value['nonce'] == received[name]+1
            received[name] += 1
            waiting[name].set_result(time.perf_counter())
        for pair in readers:
            pair.reader.on_message = lambda t, d, name=pair.reader.name: receive(name, t, d)
        samples = []
        start = time.perf_counter()
        for seq in range(1, args.fanout_messages+1):
            waiting = {p.reader.name: asyncio.get_running_loop().create_future() for p in readers}
            sent = time.perf_counter()
            data = json.dumps(dict(actor='writer-0', nonce=seq, data='x'*args.payload_bytes)).encode()
            assert await probes[0].writer.publish('fixture/fanout', data) < 128
            arrivals = await asyncio.wait_for(asyncio.gather(*waiting.values()), 5)
            samples.extend((arrival-sent)*1000 for arrival in arrivals)
        elapsed = time.perf_counter()-start
        emit(dict(scenario='group fanout', subscribers=len(readers), published=seq, delivered=len(samples),
                  elapsed_s=round(elapsed, 3), delivered_per_second=round(len(samples)/elapsed, 1),
                  latency=distribution(samples)))
        assert len(samples) == len(readers)*seq
        assert all(c.failure is None for c in clients.values()), [(c.name, str(c.failure)) for c in clients.values() if c.failure]
        assert broker.poll() is None and authz.poll() is None
    finally:
        os.kill(authz.pid, signal.SIGCONT)
        await asyncio.gather(*(client.close() for client in clients.values()), return_exceptions=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--broker', required=True)
    parser.add_argument('--authz-fixture', required=True)
    parser.add_argument('--pairs', type=int, default=500)
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--outage-seconds', type=float, default=15)
    parser.add_argument('--rate', type=int, default=4000)
    parser.add_argument('--cold-seconds', type=float, default=10)
    parser.add_argument('--cold-rate', type=int, default=1000)
    parser.add_argument('--payload-bytes', type=int, default=4096)
    parser.add_argument('--fanout-messages', type=int, default=100)
    parser.add_argument('--queue-capacity', type=int, default=64)
    parser.add_argument('--report', required=True)
    args = parser.parse_args()
    assert 3 <= args.pairs <= 512 and args.seconds > 10 and args.outage_seconds > 10
    assert 0 < args.cold_seconds and args.seconds+args.outage_seconds+args.cold_seconds < 180
    assert args.rate > 0 and args.cold_rate > 0 and 1 <= args.payload_bytes <= 65536 and args.fanout_messages > 0
    assert 1 <= args.queue_capacity <= 4096
    report = dict(settings=vars(args), transport='loopback TCP MQTT + plaintext gRPC',
                  mqtt_threads=2, rpc_workers=4, rpc_queue_capacity=args.queue_capacity, cache_ttl_ms=10000,
                  cpu_max=optional_limit('/sys/fs/cgroup/cpu.max'),
                  memory_max=optional_limit('/sys/fs/cgroup/memory.max'), phases=[])
    def emit(row):
        report['phases'].append(row)
        Path(args.report).write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(row), flush=True)
    with fixture(args.broker, args.authz_fixture, args.pairs, cache_ttl_ms=10000, queue_capacity=args.queue_capacity) as processes:
        asyncio.run(run(*processes, args, emit))
    # Rejections are expected under a bounded admission policy. Passing the
    # safety/steady-state checks does NOT mean the CONNECT burst was lossless.
    report['regression_checks_passed'] = True
    report['connect_burst_lossless'] = report['phases'][0]['rejected_or_failed'] == 0
    Path(args.report).write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
