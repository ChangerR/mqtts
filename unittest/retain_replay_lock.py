#!/usr/bin/env python3
"""A blocked SUBACK must not stall other threads' PING or retained publishes.

Regression for the global retain lock being held across the socket write.
"""
import argparse
import json
import socket
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path


def varint(n):
    out = b''
    while True:
        b = n % 128
        n //= 128
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out


def utf(s):
    s = s.encode() if isinstance(s, str) else s
    return struct.pack('>H', len(s)) + s


def packet(head, body):
    return bytes([head]) + varint(len(body)) + body


class Client:
    def __init__(self, port, client_id):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=8)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        body = utf('MQTT') + bytes([5, 0x02, 0, 60, 0]) + utf(client_id)
        self.sock.sendall(packet(0x10, body))
        head, payload = self.read()
        assert head == 0x20 and payload[1] == 0, (head, payload)

    def read(self):
        def exact(n):
            data = b''
            while len(data) < n:
                chunk = self.sock.recv(n - len(data))
                if not chunk:
                    raise EOFError
                data += chunk
            return data
        head = exact(1)[0]
        value, shift = 0, 1
        while True:
            byte = exact(1)[0]
            value += (byte & 127) * shift
            shift *= 128
            if not byte & 128:
                break
        return head, exact(value)

    def publish(self, topic, payload, qos=0, retain=False, packet_id=1):
        head = 0x30 | (qos << 1) | (1 if retain else 0)
        body = utf(topic) + (struct.pack('>H', packet_id) if qos else b'') + b'\x00' + payload
        self.sock.sendall(packet(head, body))
        if qos:
            while True:
                response, _ = self.read()
                if response == 0x40:
                    return

    def subscribe(self, topic, qos=0, packet_id=1):
        self.sock.sendall(packet(0x82, struct.pack('>H', packet_id) + b'\x00' + utf(topic) + bytes([qos])))
        head, _ = self.read()
        assert head == 0x90

    def ping(self):
        self.sock.sendall(b'\xc0\x00')
        started = time.monotonic()
        while True:
            head, _ = self.read()
            if head == 0xD0:
                return time.monotonic() - started

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def server_tx_queue(listen_port, client_port):
    """Unacknowledged bytes sitting in the broker's send buffer for this client."""
    with open('/proc/net/tcp') as handle:
        next(handle)
        for line in handle:
            parts = line.split()
            local_port = int(parts[1].split(':')[1], 16)
            remote_port = int(parts[2].split(':')[1], 16)
            if local_port == listen_port and remote_port == client_port:
                return int(parts[4].split(':')[0], 16)
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mqtts-retain-lock-') as directory:
        root = Path(directory)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        config = {
            'server': {'bind_address': '127.0.0.1', 'port': port, 'thread_count': 2, 'max_connections': 100},
            'mqtt': {'allow_mqtt3x': True, 'max_qos': 2},
            'auth': {'enabled': False},
            'log': {'level': 'error'},
            'monitoring': {'enabled': False},
            'persistence': {
                'enabled': True,
                'path': str(root / 'journal'),
                'checkpoint_interval_ms': 600000,
                'segment_bytes': 65536,
            },
        }
        (root / 'broker.json').write_text(json.dumps(config))
        log = (root / 'broker.log').open('w')
        proc = subprocess.Popen([args.broker, '-c', str(root / 'broker.json')], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 8
            while True:
                assert proc.poll() is None, (root / 'broker.log').read_text()[-4000:]
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                        break
                except OSError:
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
            slow = Client(port, 'slow_sub')
            slow.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            slow.subscribe('flood', qos=0)
            flood = Client(port, 'flood')
            payload = b'x' * 60000
            for _ in range(80):
                flood.publish('flood', payload, qos=0)
            time.sleep(0.3)
            queued = server_tx_queue(port, slow.sock.getsockname()[1])
            assert queued is not None and queued > 20000, queued
            pingers = [Client(port, 'ping_%d' % i) for i in range(4)]
            publishers = [Client(port, 'pub_%d' % i) for i in range(4)]
            # Do not read the SUBACK. The broker blocks in the socket write.
            slow.sock.sendall(packet(0x82, struct.pack('>H', 9) + b'\x00' + utf('other') + b'\x00'))
            time.sleep(0.05)
            latencies = {}

            def measure_ping(index, client):
                latencies['ping%d' % index] = client.ping()

            def measure_publish(index, client):
                started = time.monotonic()
                client.publish('ret/%d' % index, b'v', qos=1, retain=True, packet_id=3)
                latencies['pub%d' % index] = time.monotonic() - started

            threads = [threading.Thread(target=measure_ping, args=(i, c)) for i, c in enumerate(pingers)]
            threads += [threading.Thread(target=measure_publish, args=(i, c)) for i, c in enumerate(publishers)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=3)
            assert all(not thread.is_alive() for thread in threads), (root / 'broker.log').read_text()[-4000:]
            slowest_ping = max(latencies['ping%d' % i] for i in range(4))
            slowest_publish = max(latencies['pub%d' % i] for i in range(4))
            print('blocked-SUBACK window: slowest ping %.3fs, slowest retained publish %.3fs' % (
                slowest_ping, slowest_publish))
            # Holding the retain lock across the 5s send made both of these ~4.5s.
            assert slowest_ping < 1.0, slowest_ping
            assert slowest_publish < 1.0, slowest_publish
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
            log.close()


if __name__ == '__main__':
    main()
