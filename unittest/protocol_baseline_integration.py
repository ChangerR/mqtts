#!/usr/bin/env python3
"""P0-2 连接生命周期，以及 P0-3 报文上限与流控。TCP 与 WebSocket 二进制都覆盖。"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time


def utf(value):
    raw = value.encode()
    return struct.pack('!H', len(raw)) + raw


def packet(header, body):
    size, encoded = len(body), bytearray()
    while True:
        byte, size = size % 128, size // 128
        encoded.append(byte | (128 if size else 0))
        if not size:
            return bytes([header]) + encoded + body


def remaining(data, index):
    value, shift = 0, 0
    while True:
        if index >= len(data):
            raise AssertionError('truncated remaining length')
        byte = data[index]
        index += 1
        value += (byte & 127) << shift
        if not byte & 128:
            return value, index
        shift += 7
        if shift > 21:
            raise AssertionError('remaining length too long')


class Broker:
    def __init__(self, binary, threads, keep_alive_max, connect_timeout,
                 max_packet_size=1048576, receive_maximum=65535, max_qos=2, monitor=False):
        self.root = Path(tempfile.mkdtemp(prefix='mqtts-p02-'))
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        self.monitor_path = self.root / 'monitor.json'
        config = {
            'server': {'bind_address': '127.0.0.1', 'port': self.port, 'thread_count': threads},
            'mqtt': {
                'max_packet_size': max_packet_size,
                'receive_maximum': receive_maximum,
                'max_qos': max_qos,
                'keep_alive_max': keep_alive_max,
                'connect_timeout_seconds': connect_timeout,
                'allow_mqtt3x': True,
            },
            'memory': {'client_max_size': max(1114112, max_packet_size + 65536)},
            'log': {'level': 'info', 'file_path': str(self.root / 'broker.log'), 'flush_immediately': True},
            'monitoring': {
                'enabled': monitor,
                'interval_seconds': 1,
                'json_output_file': str(self.monitor_path),
            },
        }
        (self.root / 'config.json').write_text(json.dumps(config))
        self.log = (self.root / 'broker.log').open('a+')
        self.proc = subprocess.Popen(
            [binary, '-c', str(self.root / 'config.json')], stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 8
        while True:
            if self.proc.poll() is not None or time.monotonic() >= deadline:
                raise AssertionError('broker did not listen:\n' + self.tail())
            try:
                with socket.create_connection(('127.0.0.1', self.port), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.02)

    def tail(self):
        path = self.root / 'broker.log'
        if not path.exists():
            return ''
        text = path.read_text(errors='replace')
        return text[-4000:]

    def counter(self, name):
        if not self.monitor_path.exists():
            return None
        try:
            return json.loads(self.monitor_path.read_text())['protocol'][name]
        except (OSError, json.JSONDecodeError, KeyError):
            return None

    def wait_counter(self, name, minimum, timeout=4):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.counter(name)
            if last is not None and last >= minimum:
                return last
            time.sleep(0.1)
        raise AssertionError('%s=%s, want >= %s\n%s' % (name, last, minimum, self.tail()))

    def thread_of(self, client_id):
        text = (self.root / 'broker.log').read_text(errors='replace')
        found = re.findall(
            r'Session registered globally: %s thread (\d+)' % re.escape(client_id), text)
        if not found:
            raise AssertionError('no session log for %s\n%s' % (client_id, self.tail()))
        return found[-1]

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=3)
        self.log.close()


class Client:
    def __init__(self, port, websocket=False):
        self.websocket = websocket
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.wire = b''
        self.mqtt = b''
        self.opened = time.monotonic()
        if websocket:
            key = base64.b64encode(os.urandom(16)).decode()
            self.sock.sendall((
                f'GET /mqtt HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n'
                'Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n'
                'Sec-WebSocket-Protocol: mqtt\r\n\r\n' % key).encode())
            response = b''
            while b'\r\n\r\n' not in response:
                chunk = self._raw(1, 3)
                if not chunk:
                    raise AssertionError('handshake closed: %r' % response)
                response += chunk
            if not response.startswith(b'HTTP/1.1 101'):
                raise AssertionError(response)
            accept = base64.b64encode(hashlib.sha1(
                (key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
            if accept not in response:
                raise AssertionError(response)

    def _raw(self, count, timeout):
        self.sock.settimeout(timeout)
        data = b''
        while len(data) < count:
            try:
                more = self.sock.recv(count - len(data))
            except TimeoutError:
                return data
            if not more:
                return data
            data += more
        return data

    def send(self, payload):
        if self.websocket:
            mask = os.urandom(4)
            size = len(payload)
            if size < 126:
                header = bytes([0x82, 0x80 | size])
            elif size < 65536:
                header = b'\x82\xfe' + struct.pack('!H', size)
            else:
                header = b'\x82\xff' + struct.pack('!Q', size)
            payload = header + mask + bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        self.sock.sendall(payload)

    def _fill(self, timeout):
        self.sock.settimeout(timeout)
        try:
            more = self.sock.recv(4096)
        except TimeoutError:
            return False
        if not more:
            raise EOFError('connection closed')
        self.wire += more
        return True

    def _take_frame(self):
        if len(self.wire) < 2:
            return None
        opcode = self.wire[0] & 15
        masked = self.wire[1] & 128
        size = self.wire[1] & 127
        header = 2
        if size == 126:
            if len(self.wire) < 4:
                return None
            size = struct.unpack('!H', self.wire[2:4])[0]
            header = 4
        elif size == 127:
            if len(self.wire) < 10:
                return None
            size = struct.unpack('!Q', self.wire[2:10])[0]
            header = 10
        mask = b''
        if masked:
            if len(self.wire) < header + 4:
                return None
            mask = self.wire[header:header + 4]
            header += 4
        if len(self.wire) < header + size:
            return None
        payload = self.wire[header:header + size]
        self.wire = self.wire[header + size:]
        if masked:
            payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        return opcode, payload

    def _complete(self):
        if len(self.mqtt) < 2:
            return None
        try:
            length, index = remaining(self.mqtt, 1)
        except AssertionError:
            return None
        if len(self.mqtt) < index + length:
            return None
        return index, length

    def _pull_mqtt(self):
        if not self.websocket:
            self.mqtt += self.wire
            self.wire = b''
            return
        while self._complete() is None:
            frame = self._take_frame()
            if frame is None:
                return
            opcode, payload = frame
            if opcode == 8:
                if self.mqtt:
                    return
                raise EOFError('websocket closed')
            if opcode == 2 and payload:
                self.mqtt += payload

    def read(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            self._pull_mqtt()
            complete = self._complete()
            if complete is not None:
                index, length = complete
                header = self.mqtt[0]
                body = self.mqtt[index:index + length]
                self.mqtt = self.mqtt[index + length:]
                return header, body
            if time.monotonic() >= deadline:
                raise TimeoutError('mqtt packet timeout')
            self._fill(max(0.05, deadline - time.monotonic()))

    def expect_closed(self, timeout):
        try:
            header, body = self.read(timeout)
        except TimeoutError:
            raise AssertionError('connection stayed open')
        except (EOFError, ConnectionError, OSError):
            return time.monotonic()
        raise AssertionError('unexpected packet 0x%02x %s' % (header, body.hex()))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def connect_packet(client_id, version, keep_alive, properties=b''):
    name = 'MQTT' if version >= 4 else 'MQIsdp'
    body = utf(name) + bytes([version, 0x02]) + struct.pack('!H', keep_alive)
    if version >= 5:
        if len(properties) > 127:
            raise AssertionError('test properties need a one-byte length')
        body += bytes([len(properties)]) + properties
    return packet(0x10, body + utf(client_id))


def property_length(body):
    length, index = remaining(body, 2)
    return body[index:index + length]


def server_keep_alive(body):
    blob = property_length(body)
    index, found = 0, None
    while index < len(blob):
        ident = blob[index]
        index += 1
        if ident in (0x13, 0x21, 0x22):
            value = int.from_bytes(blob[index:index + 2], 'big')
            if ident == 0x13:
                found = value
            index += 2
        elif ident in (0x11, 0x02, 0x18, 0x27):
            index += 4
        elif ident in (0x01, 0x17, 0x19, 0x24, 0x25, 0x28, 0x29, 0x2A):
            index += 1
        elif ident in (0x12, 0x15, 0x16, 0x1A, 0x1C, 0x1F):
            size = int.from_bytes(blob[index:index + 2], 'big')
            index += 2 + size
        elif ident == 0x26:
            for _ in range(2):
                size = int.from_bytes(blob[index:index + 2], 'big')
                index += 2 + size
        else:
            raise AssertionError('unknown property 0x%02x in %s' % (ident, blob.hex()))
    return found


def connect(client, client_id, version, keep_alive, properties=b''):
    client.send(connect_packet(client_id, version, keep_alive, properties))
    header, body = client.read(3)
    if header != 0x20 or body[1] != 0:
        raise AssertionError('CONNACK %s %s' % (header, body.hex()))
    return body


def subscribe(client, topic, version, qos=0):
    body = b'\x00\x01' + (b'\x00' if version >= 5 else b'') + utf(topic) + bytes([qos])
    client.send(packet(0x82, body))
    header, result = client.read(3)
    if header != 0x90 or result[-1] >= 0x80 or (qos == 0 and result[-1] != 0):
        raise AssertionError('SUBACK %s %s' % (header, result.hex()))
    return result[-1]


def publish(client, topic, payload, version):
    props = b'\x00' if version >= 5 else b''
    client.send(packet(0x30, utf(topic) + props + payload))


def publish_qos(client, topic, payload, version, qos, packet_id):
    body = utf(topic)
    if qos:
        body += struct.pack('!H', packet_id)
    if version >= 5:
        body += b'\x00'
    body += payload
    client.send(packet(0x30 | (qos << 1), body))


def puback(client, packet_id):
    client.send(packet(0x40, struct.pack('!H', packet_id)))


def pubrel(client, packet_id):
    client.send(packet(0x62, struct.pack('!H', packet_id)))


def publish_of(header, body, version):
    topic, _packet_id, payload = publish_meta(header, body, version)
    return topic, payload


def publish_meta(header, body, version):
    if header >> 4 != 3:
        raise AssertionError('not a publish: %s %s' % (header, body.hex()))
    qos = (header >> 1) & 3
    topic_len = int.from_bytes(body[:2], 'big')
    topic = body[2:2 + topic_len].decode()
    index = 2 + topic_len
    packet_id = 0
    if qos:
        packet_id = int.from_bytes(body[index:index + 2], 'big')
        index += 2
    if version >= 5:
        length, index = remaining(body, index)
        index += length
    return topic, packet_id, body[index:]


def disconnect_reason(header, body):
    if header != 0xE0:
        raise AssertionError('expected DISCONNECT, got 0x%02x %s' % (header, body.hex()))
    if not body:
        raise AssertionError('DISCONNECT without a reason code')
    return body[0]


def expect_disconnect_after(client, reason, low, high):
    started = time.monotonic()
    header, body = client.read(high + 1)
    elapsed = time.monotonic() - started
    got = disconnect_reason(header, body)
    if got != reason:
        raise AssertionError('reason 0x%02x, want 0x%02x' % (got, reason))
    if not low <= elapsed <= high:
        raise AssertionError('disconnect after %.2fs, want %.1f..%.1f' % (elapsed, low, high))
    client.expect_closed(1)


def silent_close(binary, websocket):
    broker = Broker(binary, 1, 30, 2)
    try:
        client = Client(broker.port, websocket)
        try:
            closed = client.expect_closed(4)
            elapsed = closed - client.opened
            if not 1.2 <= elapsed <= 4.0:
                raise AssertionError('silent close after %.2fs' % elapsed)
        finally:
            client.close()
    finally:
        broker.close()


def keepalive_idle(binary, websocket, version):
    broker = Broker(binary, 1, 30, 10)
    try:
        client = Client(broker.port, websocket)
        try:
            body = connect(client, 'ka-idle', version, 2)
            if version >= 5 and server_keep_alive(body) is not None:
                raise AssertionError('K=2 within max must not announce Server Keep Alive')
            if version >= 5:
                expect_disconnect_after(client, 0x8D, 2.0, 4.5)
            else:
                started = time.monotonic()
                closed = client.expect_closed(5)
                elapsed = closed - started
                if not 2.0 <= elapsed <= 4.5:
                    raise AssertionError('3.x close after %.2fs' % elapsed)
        finally:
            client.close()
    finally:
        broker.close()


def keepalive_pings(binary, websocket):
    broker = Broker(binary, 1, 30, 10)
    try:
        client = Client(broker.port, websocket)
        try:
            connect(client, 'ka-ping', 5, 2)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                client.send(b'\xc0\x00')
                header, _ = client.read(2)
                if header != 0xD0:
                    raise AssertionError('PINGRESP missing: 0x%02x' % header)
                time.sleep(0.8)
        finally:
            client.close()
    finally:
        broker.close()


def rewritten_keep_alive(binary, websocket):
    broker = Broker(binary, 1, 2, 10)
    try:
        v5 = Client(broker.port, websocket)
        try:
            body = connect(v5, 'ka-rewrite', 5, 0)
            if server_keep_alive(body) != 2:
                raise AssertionError('Server Keep Alive: %s' % server_keep_alive(body))
            expect_disconnect_after(v5, 0x8D, 2.0, 4.5)
        finally:
            v5.close()
        legacy = Client(broker.port, websocket)
        try:
            connect(legacy, 'ka-legacy-zero', 4, 0)
            legacy.send(b'\xc0\x00')
            time.sleep(4)
            legacy.send(b'\xc0\x00')
            header, _ = legacy.read(2)
            if header != 0xD0:
                raise AssertionError('3.x K=0 was disconnected: 0x%02x' % header)
        finally:
            legacy.close()
    finally:
        broker.close()


def malformed(binary, websocket):
    broker = Broker(binary, 1, 30, 10)
    bad = b'\x30\x80\x80\x80\x80'
    try:
        early = Client(broker.port, websocket)
        try:
            early.send(b'\xc0\x00')
            header, body = early.read(2)
            if disconnect_reason(header, body) != 0x82:
                raise AssertionError(body.hex())
            early.expect_closed(1)
        finally:
            early.close()

        client = Client(broker.port, websocket)
        try:
            connect(client, 'bad-packet', 5, 30)
            client.send(bad)
            header, body = client.read(2)
            if disconnect_reason(header, body) != 0x81:
                raise AssertionError(body.hex())
            client.expect_closed(1)
        finally:
            client.close()

        legacy = Client(broker.port, websocket)
        try:
            connect(legacy, 'bad-3x', 4, 30)
            legacy.send(bad)
            legacy.expect_closed(1)
        finally:
            legacy.close()
    finally:
        broker.close()


def collect(client, version, timeout):
    found = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            header, body = client.read(max(0.05, deadline - time.monotonic()))
        except (TimeoutError, EOFError):
            break
        found.append(publish_of(header, body, version))
    return found


def takeover_pair(broker, websocket, version, client_id):
    old = Client(broker.port, websocket)
    new = None
    publisher = None
    try:
        connect(old, client_id, version, 30)
        old_thread = broker.thread_of(client_id)
        subscribe(old, 'old/topic', version)
        new = Client(broker.port, websocket)
        connect(new, client_id, version, 30)
        new_thread = broker.thread_of(client_id)
        if version >= 5:
            header, body = old.read(2)
            if disconnect_reason(header, body) != 0x8E:
                raise AssertionError(body.hex())
        else:
            old.expect_closed(2)
        subscribe(new, 'new/topic', version)
        publisher = Client(broker.port, websocket)
        connect(publisher, client_id + '-pub', version, 30)
        publish(publisher, 'old/topic', b'nope', version)
        publish(publisher, 'new/topic', b'yes', version)
        got = collect(new, version, 1)
        if ('new/topic', b'yes') not in got or any(item[0] == 'old/topic' for item in got):
            raise AssertionError(got)
        return old_thread, new_thread
    finally:
        old.close()
        if new:
            new.close()
        if publisher:
            publisher.close()


def takeover(binary, websocket):
    broker = Broker(binary, 1, 30, 10)
    try:
        takeover_pair(broker, websocket, 5, 'same-thread')
        takeover_pair(broker, websocket, 4, 'same-thread-3x')
    finally:
        broker.close()

    broker = Broker(binary, 2, 30, 10)
    try:
        crossed = False
        for attempt in range(24):
            old_thread, new_thread = takeover_pair(
                broker, websocket, 5, 'cross-%s-%d' % ('ws' if websocket else 'tcp', attempt))
            if old_thread != new_thread:
                crossed = True
                break
        if not crossed:
            raise AssertionError('thread_count 2 never split a takeover\n' + broker.tail())
    finally:
        broker.close()


def quiet(client, timeout):
    try:
        header, body = client.read(timeout)
    except TimeoutError:
        return
    raise AssertionError('unexpected packet 0x%02x %s' % (header, body.hex()))


def flow_properties(receive_maximum=None, maximum_packet_size=None):
    blob = b''
    if receive_maximum is not None:
        blob += b'\x21' + struct.pack('!H', receive_maximum)
    if maximum_packet_size is not None:
        blob += b'\x27' + struct.pack('!I', maximum_packet_size)
    return blob


def oversized_publish(binary, websocket):
    broker = Broker(binary, 1, 30, 10, max_packet_size=256, monitor=True)
    try:
        client = Client(broker.port, websocket)
        try:
            connect(client, 'oversize', 5, 30)
            publish(client, 't', b'x' * 300, 5)
            header, body = client.read(2)
            if disconnect_reason(header, body) != 0x95:
                raise AssertionError('want 0x95, got 0x%02x %s' % (header, body.hex()))
            client.expect_closed(1)
        finally:
            client.close()
        legacy = Client(broker.port, websocket)
        try:
            connect(legacy, 'oversize3', 4, 30)
            publish(legacy, 't', b'x' * 300, 4)
            legacy.expect_closed(2)
        finally:
            legacy.close()
        # 监控线程每秒重写一次。流控断开不能记成畸形包。
        time.sleep(1.2)
        malformed_count = broker.counter('malformed_packet_disconnects')
        if malformed_count != 0:
            raise AssertionError('malformed disconnects %s' % malformed_count)
    finally:
        broker.close()


def outbound_packet_limit(binary, websocket):
    broker = Broker(binary, 1, 30, 10, monitor=True)
    try:
        subscriber = Client(broker.port, websocket)
        publisher = Client(broker.port, websocket)
        try:
            connect(subscriber, 'limited', 5, 30, flow_properties(maximum_packet_size=1024))
            subscribe(subscriber, 'size/limit', 5)
            connect(publisher, 'writer', 5, 30)
            publish(publisher, 'size/limit', b'A' * 2048, 5)
            publish(publisher, 'size/limit', b'B' * 512, 5)
            header, body = subscriber.read(3)
            topic, payload = publish_of(header, body, 5)
            if topic != 'size/limit' or payload != b'B' * 512:
                raise AssertionError((topic, len(payload)))
            quiet(subscriber, 0.4)
            drops = broker.wait_counter('outbound_packet_size_drops', 1)
            if drops != 1:
                raise AssertionError('drops %s' % drops)
        finally:
            subscriber.close()
            publisher.close()
    finally:
        broker.close()


def outbound_receive_window(binary, websocket):
    broker = Broker(binary, 1, 30, 10)
    try:
        subscriber = Client(broker.port, websocket)
        publisher = Client(broker.port, websocket)
        try:
            connect(subscriber, 'window', 5, 60, flow_properties(receive_maximum=2))
            subscribe(subscriber, 'flow/order', 5, qos=1)
            connect(publisher, 'window-writer', 5, 60)
            for index in range(5):
                publish_qos(publisher, 'flow/order', bytes([index]), 5, 1, index + 1)
                header, body = publisher.read(2)
                if header != 0x40 or int.from_bytes(body[:2], 'big') != index + 1:
                    raise AssertionError('publisher PUBACK %s %s' % (header, body.hex()))
            received = []
            for _ in range(2):
                header, body = subscriber.read(2)
                topic, packet_id, payload = publish_meta(header, body, 5)
                if topic != 'flow/order' or (header >> 1) & 3 != 1:
                    raise AssertionError((header, topic))
                received.append((packet_id, payload))
            quiet(subscriber, 0.4)
            if [item[1] for item in received] != [bytes([0]), bytes([1])]:
                raise AssertionError(received)
            if received[0][0] == received[1][0]:
                raise AssertionError(received)
            for packet_id, _payload in received:
                puback(subscriber, packet_id)
            for index in range(2, 5):
                header, body = subscriber.read(2)
                topic, packet_id, payload = publish_meta(header, body, 5)
                if payload != bytes([index]) or packet_id in (received[0][0], received[1][0]):
                    raise AssertionError((index, packet_id, payload))
                received.append((packet_id, payload))
                puback(subscriber, packet_id)
            if [item[1] for item in received] != [bytes([index]) for index in range(5)]:
                raise AssertionError(received)
        finally:
            subscriber.close()
            publisher.close()
    finally:
        broker.close()


def inbound_qos2(binary, websocket):
    broker = Broker(binary, 1, 30, 10, receive_maximum=1)
    try:
        subscriber = Client(broker.port, websocket)
        publisher = Client(broker.port, websocket)
        try:
            connect(subscriber, 'qos2-sub', 5, 30)
            subscribe(subscriber, 'flow/qos2', 5, qos=0)
            connect(publisher, 'qos2-pub', 5, 30)
            publish_qos(publisher, 'flow/qos2', b'once', 5, 2, 7)
            header, body = publisher.read(2)
            if header != 0x50 or int.from_bytes(body[:2], 'big') != 7:
                raise AssertionError('PUBREC %s %s' % (header, body.hex()))
            topic, payload = publish_of(*subscriber.read(2), 5)
            if topic != 'flow/qos2' or payload != b'once':
                raise AssertionError((topic, payload))
            publish_qos(publisher, 'flow/qos2', b'once', 5, 2, 7)
            header, body = publisher.read(2)
            if header != 0x50 or int.from_bytes(body[:2], 'big') != 7:
                raise AssertionError('duplicate PUBREC %s %s' % (header, body.hex()))
            quiet(subscriber, 0.4)
            pubrel(publisher, 7)
            header, body = publisher.read(2)
            if header != 0x70 or int.from_bytes(body[:2], 'big') != 7:
                raise AssertionError('PUBCOMP %s %s' % (header, body.hex()))
        finally:
            subscriber.close()
            publisher.close()
        limited = Client(broker.port, websocket)
        try:
            connect(limited, 'qos2-cap', 5, 30)
            publish_qos(limited, 'flow/qos2', b'a', 5, 2, 1)
            header, body = limited.read(2)
            if header != 0x50:
                raise AssertionError('first PUBREC %s %s' % (header, body.hex()))
            publish_qos(limited, 'flow/qos2', b'b', 5, 2, 2)
            header, body = limited.read(2)
            if disconnect_reason(header, body) != 0x93:
                raise AssertionError('want 0x93, got 0x%02x %s' % (header, body.hex()))
            limited.expect_closed(1)
        finally:
            limited.close()
    finally:
        broker.close()


def max_qos_cap(binary, websocket):
    broker = Broker(binary, 1, 30, 10, max_qos=1)
    try:
        publisher = Client(broker.port, websocket)
        try:
            connect(publisher, 'qos-cap', 5, 30)
            publish_qos(publisher, 'flow/qos', b'nope', 5, 2, 1)
            header, body = publisher.read(2)
            if disconnect_reason(header, body) != 0x9B:
                raise AssertionError('want 0x9B, got 0x%02x %s' % (header, body.hex()))
            publisher.expect_closed(1)
        finally:
            publisher.close()
        subscriber = Client(broker.port, websocket)
        sender = Client(broker.port, websocket)
        try:
            connect(subscriber, 'qos-sub', 5, 30)
            granted = subscribe(subscriber, 'flow/qos', 5, qos=2)
            if granted != 1:
                raise AssertionError('granted %s' % granted)
            connect(sender, 'qos-ok', 5, 30)
            publish_qos(sender, 'flow/qos', b'yes', 5, 1, 1)
            header, body = sender.read(2)
            if header != 0x40:
                raise AssertionError('PUBACK %s %s' % (header, body.hex()))
            topic, payload = publish_of(*subscriber.read(2), 5)
            if topic != 'flow/qos' or payload != b'yes':
                raise AssertionError((topic, payload))
        finally:
            subscriber.close()
            sender.close()
    finally:
        broker.close()


def packet_id_wrap(binary, websocket):
    count = 65536
    window = 32
    broker = Broker(binary, 1, 60, 10)
    try:
        subscriber = Client(broker.port, websocket)
        publisher = Client(broker.port, websocket)
        try:
            connect(subscriber, 'wrap-sub', 5, 60, flow_properties(receive_maximum=window))
            subscribe(subscriber, 'flow/ids', 5, qos=1)
            connect(publisher, 'wrap-pub', 5, 60)
            sent = acked = got = 0
            inflight = []
            while got < count:
                if len(inflight) >= window:
                    for _ in range(window // 2):
                        puback(subscriber, inflight.pop(0))
                while sent < min(count, got + window):
                    publish_qos(publisher, 'flow/ids', struct.pack('!I', sent), 5, 1,
                                (sent % 65535) + 1)
                    sent += 1
                while acked < sent:
                    header, body = publisher.read(3)
                    if header != 0x40:
                        raise AssertionError('publisher ack 0x%02x %s' % (header, body.hex()))
                    acked += 1
                while got < sent and len(inflight) < window:
                    header, body = subscriber.read(3)
                    topic, packet_id, payload = publish_meta(header, body, 5)
                    expected = (got % 65535) + 1
                    sequence = struct.unpack('!I', payload)[0]
                    if topic != 'flow/ids' or packet_id != expected or sequence != got:
                        raise AssertionError((got, packet_id, expected, sequence, topic))
                    if packet_id in inflight:
                        raise AssertionError('packet id %s still inflight' % packet_id)
                    inflight.append(packet_id)
                    got += 1
                    if got % 16384 == 0:
                        print('packet ids %s/%s websocket=%s' % (got, count, websocket), flush=True)
            for packet_id in inflight:
                puback(subscriber, packet_id)
        finally:
            subscriber.close()
            publisher.close()
    finally:
        broker.close()


def run(binary):
    for websocket in (False, True):
        silent_close(binary, websocket)
        keepalive_idle(binary, websocket, 5)
        keepalive_idle(binary, websocket, 4)
        keepalive_pings(binary, websocket)
        rewritten_keep_alive(binary, websocket)
        malformed(binary, websocket)
        takeover(binary, websocket)
        oversized_publish(binary, websocket)
        outbound_packet_limit(binary, websocket)
        outbound_receive_window(binary, websocket)
        inbound_qos2(binary, websocket)
        max_qos_cap(binary, websocket)
        packet_id_wrap(binary, websocket)
        print('PASS P0-3 websocket=%s' % websocket, flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    args = parser.parse_args()
    run(args.broker)


if __name__ == '__main__':
    main()
