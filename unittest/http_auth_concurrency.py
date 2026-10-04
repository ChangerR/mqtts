#!/usr/bin/env python3
"""Bounded real-MQTT concurrency load; reports delivered traffic, not send calls."""
import argparse
import asyncio
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import time

from http_auth_cache_integration import Fixture
from http_auth_integration import packet, utf


def slow_delivery_isolation(binary):
    with Fixture(binary, http_workers=4, http_queue_capacity=64) as fixture:
        fixture.wildcards = True
        reader, writer = fixture.pair()
        cold = [fixture.client('cold-'+str(i)) for i in range(12)]
        for client in cold:
            assert client.subscribe('fixture/#') == 0
        fixture.delayed_users = {client.name for client in cold}
        fixture.delay = .8
        samples = fixture.transfer(reader, writer, 100)
        assert max(samples) < 200, ('cold recipients blocked cached recipient', max(samples))
        isolation = dict(scenario='12 slow wildcard recipients and one cached recipient',
                         delivered=len(samples), max_ms=round(max(samples), 3))
        print(json.dumps(isolation), flush=True)
    # A late arrival must not pass earlier messages while the queue rotates a
    # pending authorization head. Warm topics may not overtake a cold topic.
    with Fixture(binary, timeout_ms=1000, http_workers=4, http_queue_capacity=64) as fixture:
        fixture.wildcards = True
        writer = fixture.client('writer')
        for topic in ['fixture/cold', 'fixture/warm']:
            writer.publish(topic, fixture.payload(writer))
        time.sleep(.05)
        reader = fixture.client('reader')
        assert reader.subscribe('fixture/#') == 0
        writer.publish('fixture/warm', fixture.payload(writer))
        assert reader.message()[0] == 'fixture/warm'
        fixture.delayed_users, fixture.delay = {'reader'}, .2
        expected = []
        for i, topic in enumerate(['fixture/cold', 'fixture/warm', 'fixture/warm', 'fixture/cold']):
            data = fixture.payload(writer, i+1)
            expected.append((topic, data))
            writer.publish(topic, data)
            time.sleep(.01)
        assert [reader.message() for _ in expected] == expected, 'per-client order changed during deferral'
    for consent in [True, False]:
        with Fixture(binary, version_feed=True, timeout_ms=1000) as fixture:
            fixture.consent = consent
            fixture.wildcards = True
            reader, writer = fixture.pair()
            time.sleep(.15)
            fixture.transfer(reader, writer)
            revoked = fixture.client('revoked')
            assert revoked.subscribe('fixture/#') == 0
            fixture.delayed_users, fixture.delay = {'revoked'}, .3
            before = fixture.requests_by_user[('/authorization', 'revoked')]
            writer.publish('fixture/allowed', fixture.payload(writer))
            deadline = time.monotonic()+1
            while fixture.requests_by_user[('/authorization', 'revoked')] == before:
                assert time.monotonic() < deadline, 'pending delivery never reached policy service'
                time.sleep(.005)
            fixture.revoked.add('revoked')
            fixture.revision = 'delivery-revoked'
            revoked.no_message()
            time.sleep(.2)
            revoked.no_message()
    print('PASS asynchronous delivery isolation, late-arrival ordering, and pending delivery revocation', flush=True)
    return [isolation, dict(scenario='late-arrival order and pending revocation', passed=True)]


def invalidation_burst(binary):
    # A global revision change can turn formerly warm subscriptions into a
    # simultaneous cold burst. Overflow must deny, and already rewarmed traffic
    # must still progress while the policy workers are saturated.
    with Fixture(binary, version_feed=True, server_threads=2, timeout_ms=400,
                 http_workers=4, http_queue_capacity=8, cache_max_entries=1024) as fixture:
        reader, writer = fixture.pair()
        cold = [fixture.client('burst-'+str(i)) for i in range(128)]
        for index, client in enumerate(cold):
            assert client.subscribe('fixture/burst-'+str(index)) == 0
        fixture.revision = 'burst-invalidation'
        time.sleep(.2)
        fixture.transfer(reader, writer)
        fixture.delayed_users, fixture.delay = {c.name for c in cold}, .8
        fixture.seen.clear()
        before = fixture.counts['/authorization']
        with ThreadPoolExecutor(max_workers=len(cold)) as pool:
            jobs = [pool.submit(c.subscribe, 'fixture/burst-'+str(i)) for i, c in enumerate(cold)]
            assert fixture.seen.wait(1)
            samples = fixture.transfer(reader, writer, 100)
            assert max(samples) < 200, ('invalidation burst blocked cached delivery', max(samples))
            assert all(job.result() >= 128 for job in jobs), 'timed-out/overflowed authorization failed open'
        callbacks = fixture.counts['/authorization']-before
        # Four initial workers plus at most two follow-ups before the third
        # failed response opens the circuit. Expired queue entries do no I/O.
        assert callbacks <= 4+2, ('cold burst exceeded worker/circuit bounds', callbacks)
        assert fixture.proc.poll() is None
        result = dict(scenario='128 simultaneous cold scopes after invalidation',
                      denied=128, authorization_requests=callbacks,
                      cached_delivered=len(samples), cached_max_ms=round(max(samples), 3))
        print(json.dumps(result), flush=True)
        return result


def packet_growth(binary):
    with Fixture(binary) as fixture:
        reader, writer = fixture.pair()
        for size in [4096, 65536]:
            data = json.dumps(dict(actor=writer.name, data='x'*size)).encode()
            writer.publish('fixture/allowed', data)
            assert reader.message() == ('fixture/allowed', data)
        oversized = fixture.client('oversized')
        # Advertise a one-MiB body: with the fixed header this exceeds the
        # broker cap. It must close before allocating or waiting for the body.
        oversized.send(packet(0x30, b'\0'*(1024*1024))[:4])
        assert oversized.sock.recv(1) == b'', 'oversized packet header was accepted'
        fixture.transfer(reader, writer)
    print('PASS receive-buffer growth and oversized packet rejection', flush=True)
    return dict(scenario='4/64 KiB packet growth and oversized header rejection', passed=True)


class AsyncClient:
    def __init__(self, reader, writer, name):
        self.reader, self.writer, self.name = reader, writer, name
        self.pending = {}
        self.sequence = 0
        self.on_message = None
        self.failure = None
        self.task = None

    @classmethod
    async def connect(cls, port, name):
        reader, writer = await asyncio.open_connection('127.0.0.1', port)
        item = cls(reader, writer, name)
        try:
            writer.write(packet(0x10, utf('MQTT')+bytes([5, 0xc2, 0, 60, 0])+utf(name)+utf(name)+utf('test-password')))
            header, body = await asyncio.wait_for(item.read_packet(), 5)
            assert header == 0x20 and body[1] == 0, ('connect', name, body)
        except BaseException:
            writer.close()
            await writer.wait_closed()
            raise
        item.task = asyncio.create_task(item.read_loop())
        return item

    async def read_packet(self):
        header = (await self.reader.readexactly(1))[0]
        length, shift = 0, 0
        while True:
            byte = (await self.reader.readexactly(1))[0]
            length += (byte & 127) << shift
            if not byte & 128:
                break
            shift += 7
            assert shift <= 21
        return header, await self.reader.readexactly(length)

    async def read_loop(self):
        try:
            while True:
                header, body = await self.read_packet()
                if header >> 4 == 3:
                    length = int.from_bytes(body[:2], 'big')
                    topic, offset = body[2:2+length].decode(), 2+length
                    if (header >> 1) & 3:
                        self.writer.write(packet(0x40, body[offset:offset+2]))
                        offset += 2
                    assert body[offset] == 0
                    if self.on_message:
                        self.on_message(topic, body[offset+1:])
                elif header >> 4 in (4, 9):
                    ident = int.from_bytes(body[:2], 'big')
                    future = self.pending.get(ident)
                    if future and not future.done():
                        future.set_result(body[-1] if header >> 4 == 9 else (body[2] if len(body) > 2 else 0))
                else:
                    raise AssertionError(('unexpected packet', header, body))
        except asyncio.CancelledError:
            pass
        except Exception as error:
            self.failure = error
            for future in self.pending.values():
                if not future.done():
                    future.set_exception(error)

    async def request(self, kind, body):
        self.sequence = self.sequence % 65535 + 1
        ident = self.sequence
        future = asyncio.get_running_loop().create_future()
        self.pending[ident] = future
        self.writer.write(packet(kind, body(ident.to_bytes(2, 'big'))))
        try:
            await self.writer.drain()
            return await asyncio.wait_for(future, 5)
        finally:
            self.pending.pop(ident, None)

    async def subscribe(self, topic):
        return await self.request(0x82, lambda ident: ident+b'\0'+utf(topic)+b'\x01')

    async def publish(self, topic, data):
        return await self.request(0x32, lambda ident: utf(topic)+ident+b'\0'+data)

    async def close(self):
        if self.task:
            self.task.cancel()
            await asyncio.gather(self.task, return_exceptions=True)
        self.writer.close()
        await self.writer.wait_closed()


class Pair:
    def __init__(self, reader, writer, index, size):
        self.reader, self.writer = reader, writer
        self.topic = 'fixture/pair-'+str(index)
        self.size = size
        self.sequence = 0
        self.waiters = {}
        self.received = 0
        self.reader.on_message = self.receive

    def receive(self, topic, data):
        value = json.loads(data)
        seq = value['nonce']
        assert topic == self.topic and value['actor'] == self.writer.name
        assert value['data'] == 'x'*self.size
        assert seq in self.waiters, ('duplicate or unexpected delivery', self.topic, seq)
        future = self.waiters[seq]
        assert not future.done(), ('duplicate', seq)
        future.set_result(time.perf_counter())
        self.received += 1

    async def transfer(self):
        self.sequence += 1
        seq = self.sequence
        data = json.dumps(dict(actor=self.writer.name, nonce=seq, data='x'*self.size), separators=(',', ':')).encode()
        future = asyncio.get_running_loop().create_future()
        self.waiters[seq] = future
        start = time.perf_counter()
        try:
            reason = await self.writer.publish(self.topic, data)
            assert reason < 128, ('publish refused', self.writer.name, reason)
            received = await asyncio.wait_for(future, 5)
            return (received-start)*1000
        finally:
            self.waiters.pop(seq, None)

    async def batch(self, count, window):
        samples = []
        for start in range(0, count, window):
            samples.extend(await asyncio.gather(*(self.transfer() for _ in range(min(window, count-start)))))
        return samples


def metrics(label, pairs, samples, elapsed, callbacks, threads, window, payload_size):
    values = sorted(samples)
    return dict(scenario=label, connections=pairs*2, active_publishers=pairs,
                mqtt_threads=threads, per_publisher_window=window, content_bytes=payload_size,
                delivered=len(values), authorization_requests=callbacks,
                delivered_per_second=round(len(values)/elapsed), elapsed_s=round(elapsed, 3),
                p50_ms=round(values[len(values)//2], 3), p95_ms=round(values[int(len(values)*.95)], 3),
                p99_ms=round(values[int(len(values)*.99)], 3), max_ms=round(max(values), 3))


async def load(fixture, count, messages, window, size, threads):
    clients, pairs, results = [], [], []
    admission = asyncio.Semaphore(8)
    async def prepare(index):
        async with admission:
            reader = await AsyncClient.connect(fixture.port, 'reader-'+str(index)); clients.append(reader)
            writer = await AsyncClient.connect(fixture.port, 'writer-'+str(index)); clients.append(writer)
            pair = Pair(reader, writer, index, size)
            assert await reader.subscribe(pair.topic) == 1
            await pair.transfer()
            return pair
    try:
        pairs = await asyncio.gather(*(prepare(i) for i in range(count)))
        for mode in ['cached healthy', 'cached policy offline']:
            if mode.endswith('offline'):
                fixture.mode = 'offline'
            before = fixture.counts['/authorization']
            start = time.perf_counter()
            all_samples = await asyncio.gather(*(p.batch(messages, window) for p in pairs))
            elapsed = time.perf_counter()-start
            samples = [sample for group in all_samples for sample in group]
            callbacks = fixture.counts['/authorization']-before
            assert callbacks == 0, ('fresh hits queried HTTP', callbacks)
            assert all(p.received == p.sequence for p in pairs)
            assert all(c.failure is None for c in clients), 'MQTT read loop failed'
            row = metrics(mode, count, samples, elapsed, callbacks, threads, window, size)
            results.append(row)
            print(json.dumps(row), flush=True)
        return results
    finally:
        await asyncio.gather(*(c.close() for c in clients), return_exceptions=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    parser.add_argument('--pairs', type=int, default=32)
    parser.add_argument('--messages', type=int, default=64)
    parser.add_argument('--window', type=int, default=4)
    parser.add_argument('--payload-bytes', type=int, default=256)
    parser.add_argument('--mqtt-threads', type=int, default=2)
    parser.add_argument('--report')
    args = parser.parse_args()
    assert 1 <= args.pairs <= 512 and 1 <= args.window <= 32
    assert args.messages > 0 and 1 <= args.payload_bytes <= 1048576 and args.mqtt_threads > 0
    result = slow_delivery_isolation(os.path.abspath(args.broker))
    result.append(invalidation_burst(os.path.abspath(args.broker)))
    result.append(packet_growth(os.path.abspath(args.broker)))
    with Fixture(os.path.abspath(args.broker), server_threads=args.mqtt_threads,
                 http_workers=4, http_queue_capacity=64, cache_max_entries=16384,
                 timeout_ms=1000, cache_ttl_ms=300000) as fixture:
        fixture.fresh = 300000
        result.extend(asyncio.run(load(fixture, args.pairs, args.messages, args.window, args.payload_bytes, args.mqtt_threads)))
    if args.report:
        Path(args.report).write_text(json.dumps(result, indent=2)+'\n')
