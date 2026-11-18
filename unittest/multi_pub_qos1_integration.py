#!/usr/bin/env python3
"""Regression: unlimited-rate QoS1 from multiple publishers must not starve delivery.

The pre-fix process_loop only yielded on EAGAIN, so a handful of publishers
sending as fast as possible could keep every worker thread inside recv and
leave subscribers at recv==0. This test starts a real broker, one topic,
N publishers with no rate limit, and asserts each subscriber got messages.
"""
from __future__ import print_function

import argparse
import multiprocessing
import os
import socket
import struct
import time

from protocol_baseline_integration import (
    Broker, Client, connect, packet, puback, publish_meta, subscribe, utf,
)


TOPIC = 'starve/qos1'
PUBLISHERS = 4
SUBSCRIBERS = 2
THREADS = 2
FLOOD_SECONDS = 4
DRAIN_SECONDS = 1
# Floor is intentionally modest: the bug is total starvation (recv==0),
# not capacity loss under saturation. Fixed brokers deliver well above this
# in a few seconds; unfixed ones stay at or near zero.
MIN_RECV_EACH = 1
PAYLOAD = b'xxxxxxxx'


def remaining_encode(size):
    encoded = bytearray()
    while True:
        byte, size = size % 128, size // 128
        encoded.append(byte | (128 if size else 0))
        if not size:
            return bytes(encoded)


def qos1_batch(topic, start_id, count, payload):
    topic_hdr = utf(topic)
    chunks = []
    pid = start_id
    for _ in range(count):
        body = topic_hdr + struct.pack('!H', pid) + payload
        chunks.append(bytes([0x32]) + remaining_encode(len(body)) + body)
        pid = 1 if pid == 65535 else pid + 1
    return b''.join(chunks), pid


def connect_raw(port, client_id):
    sock = socket.socket()
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    sock.connect(('127.0.0.1', port))
    sock.settimeout(3)
    pkt = packet(0x10, utf('MQTT') + bytes([4, 0x02]) + struct.pack('!H', 60) + utf(client_id))
    sock.sendall(pkt)
    # CONNACK is 4 bytes (header + remaining length + 2-byte variable header)
    data = b''
    deadline = time.monotonic() + 3
    while len(data) < 4 and time.monotonic() < deadline:
        more = sock.recv(4 - len(data))
        if not more:
            raise AssertionError('publisher %s connection closed during CONNACK' % client_id)
        data += more
    if not data.startswith(b'\x20') or data[-1] != 0:
        raise AssertionError('publisher %s CONNACK %s' % (client_id, data.hex()))
    return sock


def publisher_proc(port, index, topic, duration, sent_counter, err_queue):
    try:
        sock = connect_raw(port, 'starve-pub-%d' % index)
        deadline = time.monotonic() + duration
        pid = 1
        sent = 0
        # Prebuild ~200 packets per write to keep the socket readable on the broker.
        while time.monotonic() < deadline:
            batch, pid = qos1_batch(topic, pid, 256, PAYLOAD)
            view = memoryview(batch)
            offset = 0
            while offset < len(batch):
                try:
                    sock.settimeout(max(0.05, deadline - time.monotonic()))
                    n = sock.send(view[offset:])
                    if n <= 0:
                        raise OSError('short send')
                    offset += n
                except socket.timeout:
                    if time.monotonic() >= deadline:
                        break
                except OSError:
                    err_queue.put('publisher %d send failed after %d' % (index, sent))
                    sock.close()
                    return
            sent += 256
            # Drain PUBACKs so the TCP window does not stall the send path.
            try:
                sock.settimeout(0)
                while True:
                    data = sock.recv(1 << 16)
                    if not data:
                        break
            except (BlockingIOError, socket.timeout):
                pass
        try:
            sent_counter.value += sent
        except Exception:
            pass
        sock.close()
    except Exception as exc:
        err_queue.put('publisher %d: %s' % (index, exc))



def multi_pub_qos1_no_starvation(binary):
    broker = Broker(binary, THREADS, 60, 10, max_qos=1)
    procs = []
    subscribers = []
    try:
        for index in range(SUBSCRIBERS):
            client = Client(broker.port)
            connect(client, 'starve-sub-%d' % index, 4, 60)
            subscribe(client, TOPIC, 4, qos=1)
            subscribers.append(client)

        sent_counter = multiprocessing.Value('L', 0)
        err_queue = multiprocessing.Queue()
        for index in range(PUBLISHERS):
            proc = multiprocessing.Process(
                target=publisher_proc,
                args=(broker.port, index, TOPIC, FLOOD_SECONDS, sent_counter, err_queue),
            )
            proc.daemon = True
            procs.append(proc)
        for proc in procs:
            proc.start()

        counts = [0] * SUBSCRIBERS
        deadline = time.monotonic() + FLOOD_SECONDS + DRAIN_SECONDS
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            # Poll each subscriber with a short slice so one does not starve the other.
            slice_timeout = min(0.2, remaining)
            for i, client in enumerate(subscribers):
                try:
                    header, body = client.read(slice_timeout)
                except TimeoutError:
                    continue
                except (EOFError, ConnectionError, OSError) as exc:
                    raise AssertionError('subscriber %d dropped: %s\n%s' % (i, exc, broker.tail()))
                if header >> 4 != 3:
                    continue
                _topic, packet_id, _payload = publish_meta(header, body, 4)
                counts[i] += 1
                if packet_id:
                    puback(client, packet_id)

        for proc in procs:
            proc.join(timeout=2)
            if proc.is_alive():
                proc.terminate()
                proc.join(timeout=1)

        errors = []
        while not err_queue.empty():
            errors.append(err_queue.get())
        sent = sent_counter.value
        print('multi_pub_qos1 sent_batches=%d (~%d msgs) recv=%s errors=%s'
              % (sent, sent * 256 if sent else 0, counts, errors), flush=True)
        if errors:
            raise AssertionError('publisher errors: %s\n%s' % (errors, broker.tail()))
        if sent < PUBLISHERS:
            raise AssertionError('publishers barely sent anything: %s\n%s' % (sent, broker.tail()))
        starved = [i for i, n in enumerate(counts) if n < MIN_RECV_EACH]
        if starved:
            raise AssertionError(
                'subscriber starvation: recv=%s (min %s each), sent_batches=%d, starved=%s\n%s'
                % (counts, MIN_RECV_EACH, sent, starved, broker.tail()))
    finally:
        for proc in procs:
            if proc.is_alive():
                proc.terminate()
        for client in subscribers:
            try:
                client.close()
            except OSError:
                pass
        broker.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    args = parser.parse_args()
    if not os.path.isfile(args.broker):
        raise SystemExit('broker binary not found: %s' % args.broker)
    multi_pub_qos1_no_starvation(args.broker)
    print('PASS mqtt_multi_pub_qos1_integration', flush=True)


if __name__ == '__main__':
    # Needed for some multiprocessing start methods to not re-run main.
    multiprocessing.set_start_method('fork')
    main()
