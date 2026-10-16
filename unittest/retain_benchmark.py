#!/usr/bin/env python3
"""Retain-store benchmarks added by phase 0 section 5.

Two measurements, both on one in-memory broker (no persistence, no auth):

1. Publish-path overhead with retain = 0. The same QoS 1 pair is measured
   before and after the retain store holds 100,000 messages on other topics.
   retain = 0 must not take the retain lock, so the two distributions should
   match within the phase 0 latency gate (p99 no worse than 10%).

2. Wildcard subscribe delivery latency while those 100,000 messages are stored.
   Product defaults apply: max_messages = 100000 and max_deliver_per_subscribe
   = 1000, so a `#` subscribe walks the whole store and delivers the first
   1,000 topics in sorted order.

Every MQTT 5 subscription in this process sets No Local = 1.
"""
import argparse
import json
import os
import select
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path


def utf(value):
    raw = value.encode()
    return struct.pack('!H', len(raw)) + raw


def encode_packet(header, body):
    size, encoded = len(body), bytearray()
    while True:
        byte, size = size % 128, size // 128
        encoded.append(byte | (128 if size else 0))
        if not size:
            return bytes([header]) + encoded + body


def read_packet(sock, pending):
    def take(count):
        while len(pending[0]) < count:
            chunk = sock.recv(max(4096, count - len(pending[0])))
            if not chunk:
                raise EOFError('connection closed')
            pending[0] += chunk
        result, pending[0] = pending[0][:count], pending[0][count:]
        return result

    header = take(1)[0]
    length, shift = 0, 0
    while True:
        byte = take(1)[0]
        length += (byte & 127) << shift
        if not byte & 128:
            break
        shift += 7
        if shift > 28:
            raise AssertionError('remaining length too long')
    return header, take(length)


def connect(port, client_id):
    sock = socket.create_connection(('127.0.0.1', port), timeout=10)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
    # Clean Start, no credentials. Auth is off for this isolated broker.
    body = utf('MQTT') + bytes([5, 0x02, 0, 0]) + b'\x00' + utf(client_id)
    sock.sendall(encode_packet(0x10, body))
    pending = [b'']
    header, result = read_packet(sock, pending)
    if header != 0x20 or result[1] != 0:
        raise AssertionError(('connack', client_id, header, result[:8]))
    return sock, pending


def subscribe(sock, pending, topic, qos, no_local=True):
    options = qos | (0x04 if no_local else 0)
    packet_id = b'\x00\x01'
    sock.sendall(encode_packet(0x82, packet_id + b'\x00' + utf(topic) + bytes([options])))
    header, result = read_packet(sock, pending)
    if header != 0x90 or result[-1] != qos:
        raise AssertionError(('suback', topic, header, result))


def publish_qos0(sock, topic, payload, retain):
    sock.sendall(encode_packet(0x31 if retain else 0x30, utf(topic) + b'\x00' + payload))


def skip_properties(body, offset):
    length, shift = 0, 0
    while True:
        byte = body[offset]
        offset += 1
        length += (byte & 127) << shift
        if not byte & 128:
            return offset + length
        shift += 7
        if shift > 28:
            raise AssertionError('property length too long')


def distribution(values):
    ordered = sorted(values)
    if not ordered:
        return {}
    return {name: round(ordered[min(len(ordered) - 1, int(len(ordered) * fraction))], 3)
            for name, fraction in (('p50', 0.5), ('p95', 0.95), ('p99', 0.99), ('max', 1))}


def protocol_stats(path):
    try:
        return json.loads(Path(path).read_text()).get('protocol', {})
    except (OSError, json.JSONDecodeError):
        return {}


def wait_retained(path, count, timeout):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        stats = protocol_stats(path)
        last = stats.get('retained_messages')
        if last is not None and last >= count and stats.get('retained_not_stored', 0) == 0:
            return stats
        time.sleep(0.25)
    raise TimeoutError('retained_messages stayed at %s, want %s' % (last, count))


def measure_retain0(pub, pub_pending, sub, sub_pending, topic, payload, count):
    samples = []
    for index in range(1, count + 1):
        packet_id = ((index - 1) % 65535) + 1
        body = utf(topic) + packet_id.to_bytes(2, 'big') + b'\x00' + payload
        start = time.perf_counter()
        pub.sendall(encode_packet(0x32, body))
        saw_ack = False
        saw_pub = False
        deadline = time.monotonic() + 5
        while not (saw_ack and saw_pub):
            if time.monotonic() > deadline:
                raise TimeoutError('retain=0 sample %s ack=%s pub=%s' % (index, saw_ack, saw_pub))
            readable, _, _ = select.select([pub, sub], [], [], 0.5)
            if pub in readable:
                header, result = read_packet(pub, pub_pending)
                if header != 0x40 or int.from_bytes(result[:2], 'big') != packet_id:
                    raise AssertionError(('puback', header, result[:4], packet_id))
                saw_ack = True
            if sub in readable:
                header, result = read_packet(sub, sub_pending)
                if header >> 4 != 3:
                    raise AssertionError(('unexpected', header))
                qos = (header >> 1) & 3
                retain = header & 1
                length = int.from_bytes(result[:2], 'big')
                offset = 2 + length
                if qos:
                    ack_at = offset
                    offset += 2
                    sub.sendall(encode_packet(0x40, result[ack_at:offset]))
                payload_at = skip_properties(result, offset)
                delivered = result[payload_at:]
                if retain or delivered != payload or result[2:2 + length].decode() != topic:
                    raise AssertionError('retain=0 delivery mismatch')
                samples.append((time.perf_counter() - start) * 1000)
                saw_pub = True
    return samples


def fill_retained(sock, count, payload):
    started = time.perf_counter()
    # One write per message would spend the time in Python. Batch the bytes.
    blob = bytearray()
    for index in range(count):
        topic = 'store/%06d' % index
        blob += encode_packet(0x31, utf(topic) + b'\x00' + payload)
        if len(blob) >= 1024 * 1024:
            sock.sendall(blob)
            blob.clear()
    if blob:
        sock.sendall(blob)
    return round(time.perf_counter() - started, 3)


def wildcard_trial(port, expect, payload):
    sock, pending = connect(port, 'wild-' + str(time.monotonic_ns()))
    try:
        started = time.perf_counter()
        deadline = time.monotonic() + 30
        # QoS 0 + No Local. Retained messages are QoS 0, so nothing is acked.
        options = 0x04
        sock.sendall(encode_packet(0x82, b'\x00\x01' + b'\x00' + utf('store/#') + bytes([options])))
        header, result = read_packet(sock, pending)
        suback = time.perf_counter()
        if header != 0x90 or result[-1] != 0:
            raise AssertionError(('wildcard suback', header, result))
        topics = []
        first = None
        while len(topics) < expect:
            if time.monotonic() > deadline:
                raise TimeoutError('wildcard delivered %s/%s' % (len(topics), expect))
            header, result = read_packet(sock, pending)
            if header >> 4 != 3 or header & 1 != 1:
                raise AssertionError(('wildcard delivery header', header))
            length = int.from_bytes(result[:2], 'big')
            topic = result[2:2 + length].decode()
            offset = 2 + length
            if ((header >> 1) & 3) != 0:
                raise AssertionError('retained QoS 0 was upgraded')
            if result[skip_properties(result, offset):] != payload:
                raise AssertionError(('wildcard payload', topic))
            now = time.perf_counter()
            if first is None:
                first = now
            topics.append(topic)
        finished = time.perf_counter()
        # Drain nothing else for a short quiet period; the cap must hold.
        sock.settimeout(0.05)
        try:
            extra_header, _ = read_packet(sock, pending)
        except (socket.timeout, TimeoutError):
            extra_header = None
        finally:
            sock.settimeout(10)
        if extra_header is not None:
            raise AssertionError('delivery exceeded the cap')
        return dict(
            subscribe_to_suback_ms=round((suback - started) * 1000, 3),
            subscribe_to_first_ms=round((first - started) * 1000, 3),
            subscribe_to_last_ms=round((finished - started) * 1000, 3),
            suback_to_last_ms=round((finished - suback) * 1000, 3),
            delivered=len(topics),
            first_topic=topics[0],
            last_topic=topics[-1],
        )
    finally:
        sock.close()


def start_broker(binary, root, retained):
    port_sock = socket.socket()
    port_sock.bind(('127.0.0.1', 0))
    port = port_sock.getsockname()[1]
    port_sock.close()
    stats_path = root / 'monitor.json'
    config = dict(
        server=dict(bind_address='127.0.0.1', port=port, thread_count=2, max_connections=64),
        mqtt=dict(implicit_no_local=False, retain_available=True, topic_alias_maximum=65535),
        log=dict(level='warn'),
        monitoring=dict(enabled=True, interval_seconds=1, json_output_file=str(stats_path)),
        retain=dict(max_messages=max(retained, 1), max_deliver_per_subscribe=1000,
                    max_total_bytes=256 * 1024 * 1024),
        auth=dict(enabled=False),
        persistence=dict(enabled=False),
    )
    config_path = root / 'broker.json'
    config_path.write_text(json.dumps(config))
    log = (root / 'broker.log').open('w')
    proc = subprocess.Popen([binary, '-c', str(config_path)], stdout=log, stderr=log)
    deadline = time.monotonic() + 8
    while True:
        if proc.poll() is not None:
            raise RuntimeError((root / 'broker.log').read_text()[-4000:])
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                break
        except OSError:
            if time.monotonic() > deadline:
                raise TimeoutError((root / 'broker.log').read_text()[-4000:])
            time.sleep(0.02)
    return proc, port, stats_path, log


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--broker', required=True)
    parser.add_argument('--retained', type=int, default=100000)
    parser.add_argument('--samples', type=int, default=2000)
    parser.add_argument('--trials', type=int, default=15)
    parser.add_argument('--report', required=True)
    args = parser.parse_args()
    assert args.retained == 100000, 'phase 0 section 5 measures a 100,000 message retain store'
    assert 100 <= args.samples <= 20000 and 5 <= args.trials <= 40
    retain_payload = b'retained-v'
    live_payload = b'x' * 4096
    with tempfile.TemporaryDirectory(prefix='mqtts-retain-bench-') as directory:
        root = Path(directory)
        proc, port, stats_path, log = start_broker(args.broker, root, args.retained)
        try:
            pub, pub_pending = connect(port, 'pub-live')
            sub, sub_pending = connect(port, 'sub-live')
            subscribe(sub, sub_pending, 'live/qos1', 1, no_local=True)
            # Warm the pair so the compared samples are not connection setup.
            measure_retain0(pub, pub_pending, sub, sub_pending, 'live/qos1', live_payload, 100)
            baseline = measure_retain0(pub, pub_pending, sub, sub_pending, 'live/qos1', live_payload, args.samples)
            filler, filler_pending = connect(port, 'pub-retain')
            fill_s = fill_retained(filler, args.retained, retain_payload)
            stored = wait_retained(stats_path, args.retained, timeout=120)
            loaded = measure_retain0(pub, pub_pending, sub, sub_pending, 'live/qos1', live_payload, args.samples)
            trials = [wildcard_trial(port, 1000, retain_payload) for _ in range(args.trials)]
            # The monitor rewrites its JSON about once a second. Wait until the
            # truncation counter includes every wildcard subscribe.
            after = {}
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                after = protocol_stats(stats_path)
                if after.get('retained_delivery_truncated', 0) >= args.trials and after.get('retained_messages') == args.retained:
                    break
                time.sleep(0.2)
            for sock in (pub, sub, filler):
                sock.close()
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            log.close()
            if proc.returncode not in (0, -15, None) and proc.returncode is not None and proc.returncode < 0:
                pass
    base_p99 = distribution(baseline)['p99']
    loaded_p99 = distribution(loaded)['p99']
    ratio = round(loaded_p99 / base_p99, 4) if base_p99 else None
    report = dict(
        subscriber_no_local=1,
        no_local_note='MQTT v5 benchmark subscribers set No Local = 1 so a publisher on the same filter is excluded, matching historical fan-out after phase 0 made self-delivery the default.',
        broker=os.path.realpath(args.broker),
        event_threads=2,
        persistence=False,
        auth=False,
        payload_bytes_retain0=len(live_payload),
        retained_messages=args.retained,
        retained_payload_bytes=len(retain_payload),
        retain_limits=dict(max_messages=args.retained, max_deliver_per_subscribe=1000, max_total_bytes=256 * 1024 * 1024),
        topic_alias_maximum=65535,
        fill_send_s=fill_s,
        stored_when_loaded=stored,
        retain0_empty_store=dict(samples=len(baseline), publish_to_receive_ms=distribution(baseline)),
        retain0_store_loaded=dict(samples=len(loaded), publish_to_receive_ms=distribution(loaded)),
        retain0_p99_loaded_over_empty=ratio,
        retain0_p99_within_10_percent=ratio is not None and ratio <= 1.10,
        wildcard=dict(
            filter='store/#',
            trials=len(trials),
            expected_deliveries=1000,
            subscribe_to_last_ms=distribution([row['subscribe_to_last_ms'] for row in trials]),
            suback_to_last_ms=distribution([row['suback_to_last_ms'] for row in trials]),
            subscribe_to_first_ms=distribution([row['subscribe_to_first_ms'] for row in trials]),
            first_topic=trials[0]['first_topic'],
            last_topic=trials[0]['last_topic'],
            truncated_counter_after_trials=after.get('retained_delivery_truncated'),
            retained_messages_after_trials=after.get('retained_messages'),
            trials_raw=trials,
        ),
    )
    Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(
        retain0_empty_p99=base_p99,
        retain0_loaded_p99=loaded_p99,
        ratio=ratio,
        within_10_percent=report['retain0_p99_within_10_percent'],
        wildcard_subscribe_to_last_ms=report['wildcard']['subscribe_to_last_ms'],
        stored=stored.get('retained_messages'),
        truncated=after.get('retained_delivery_truncated'),
    ), indent=2), flush=True)


if __name__ == '__main__':
    main()
