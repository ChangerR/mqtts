#!/usr/bin/env python3
"""P0-2 到 P0-7 的内存行为。TCP 与 WebSocket 二进制都覆盖。遗嘱延迟只在 TCP 持久化会话上跑。保留消息落盘不在这里。"""
import argparse
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import threading
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
                 max_packet_size=1048576, receive_maximum=65535, max_qos=2, monitor=False,
                 implicit_no_local=False, wildcard_subscription_available=True,
                 topic_alias_maximum=None, auth=None, retain_available=True, persistence=False,
                 retain=None):
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
                'implicit_no_local': implicit_no_local,
                'wildcard_subscription_available': wildcard_subscription_available,
                'retain_available': retain_available,
            },
            'memory': {'client_max_size': max(1114112, max_packet_size + 65536)},
            'log': {'level': 'info', 'file_path': str(self.root / 'broker.log'), 'flush_immediately': True},
            'monitoring': {
                'enabled': monitor,
                'interval_seconds': 1,
                'json_output_file': str(self.monitor_path),
            },
        }
        if topic_alias_maximum is not None:
            config['mqtt']['topic_alias_maximum'] = topic_alias_maximum
        if auth is not None:
            config['auth'] = auth
        if persistence:
            journal = self.root / 'journal'
            journal.mkdir()
            config['persistence'] = {'enabled': True, 'path': str(journal)}
        if retain is not None:
            config['retain'] = retain
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


def u16_property(body, wanted):
    blob = property_length(body)
    index = 0
    while index < len(blob):
        ident = blob[index]
        index += 1
        if ident in (0x13, 0x21, 0x22):
            value = int.from_bytes(blob[index:index + 2], 'big')
            if ident == wanted:
                return value
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
    return None


def server_keep_alive(body):
    return u16_property(body, 0x13)


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


def publish_alias(client, topic, payload, alias, qos=0, packet_id=1):
    props = b'\x23' + struct.pack('!H', alias)
    body = utf(topic)
    if qos:
        body += struct.pack('!H', packet_id)
    body += bytes([len(props)]) + props + payload
    client.send(packet(0x30 | (qos << 1), body))


def expect_puback(client, reason):
    header, body = client.read(2)
    if header != 0x40 or len(body) < 3 or body[2] != reason:
        raise AssertionError('PUBACK 0x%02x %s, want reason 0x%02x' % (header, body.hex(), reason))


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


def sub_code(client, topic, version, flags, packet_id=1):
    body = struct.pack('!H', packet_id) + (b'\x00' if version >= 5 else b'') + utf(topic) + bytes([flags])
    client.send(packet(0x82, body))
    header, result = client.read(3)
    if header != 0x90:
        raise AssertionError('SUBACK 0x%02x %s' % (header, result.hex()))
    return result[-1]


def subscription_options(binary, websocket):
    broker = Broker(binary, 1, 30, 10)
    try:
        watcher = Client(broker.port, websocket)
        sender = Client(broker.port, websocket)
        try:
            connect(watcher, 'opt-watch', 5, 30)
            subscribe(watcher, 'opt/echo', 5, qos=0)
            connect(sender, 'opt-self', 5, 30)
            if sub_code(sender, 'opt/echo', 5, 0) != 0:
                raise AssertionError('No Local 0 was not granted')
            publish(sender, 'opt/echo', b'own', 5)
            topic, payload = publish_of(*sender.read(2), 5)
            if topic != 'opt/echo' or payload != b'own':
                raise AssertionError('publisher with No Local 0 missed %s %s' % (topic, payload))
            topic, payload = publish_of(*watcher.read(2), 5)
            if payload != b'own':
                raise AssertionError(payload)
            if sub_code(sender, 'opt/echo', 5, 0x04, packet_id=2) != 0:
                raise AssertionError('No Local 1 was not granted')
            publish(sender, 'opt/echo', b'hide', 5)
            topic, payload = publish_of(*watcher.read(2), 5)
            if payload != b'hide':
                raise AssertionError(payload)
            quiet(sender, 0.3)
        finally:
            watcher.close()
            sender.close()
        legacy = Client(broker.port, websocket)
        try:
            connect(legacy, 'opt-3x', 4, 30)
            subscribe(legacy, 'opt/echo', 4, qos=0)
            publish(legacy, 'opt/echo', b'v3', 4)
            topic, payload = publish_of(*legacy.read(2), 4)
            if topic != 'opt/echo' or payload != b'v3':
                raise AssertionError('3.x client missed its own publish')
        finally:
            legacy.close()

        low = Client(broker.port, websocket)
        high = Client(broker.port, websocket)
        try:
            connect(low, 'opt-qos', 5, 30)
            if sub_code(low, 'opt/qos', 5, 1) != 1:
                raise AssertionError('QoS 1 grant')
            connect(high, 'opt-qos-pub', 5, 30)
            publish_qos(high, 'opt/qos', b'ab', 5, 2, 1)
            header, body = high.read(2)
            if header != 0x50:
                raise AssertionError('PUBREC 0x%02x' % header)
            header, body = low.read(2)
            if (header >> 1) & 3 != 1:
                raise AssertionError('downgraded header 0x%02x' % header)
            topic, payload = publish_of(header, body, 5)
            if topic != 'opt/qos' or payload != b'ab':
                raise AssertionError((topic, payload))
        finally:
            low.close()
            high.close()

        qos0 = Client(broker.port, websocket)
        retransmit = Client(broker.port, websocket)
        try:
            connect(qos0, 'opt-dup', 5, 30)
            subscribe(qos0, 'opt/dup', 5, qos=0)
            connect(retransmit, 'opt-dup-pub', 5, 30)
            body = utf('opt/dup') + struct.pack('!H', 9) + b'\x00' + b'dup'
            retransmit.send(packet(0x3A, body))
            header, _ = retransmit.read(2)
            if header != 0x40:
                raise AssertionError('DUP publish PUBACK 0x%02x' % header)
            header, body = qos0.read(2)
            if header & 0x08 or (header >> 1) & 3 != 0:
                raise AssertionError('downgraded DUP header 0x%02x' % header)
            topic, payload = publish_of(header, body, 5)
            if topic != 'opt/dup' or payload != b'dup':
                raise AssertionError((topic, payload))
        finally:
            qos0.close()
            retransmit.close()

        overlap = Client(broker.port, websocket)
        try:
            connect(overlap, 'opt-over', 5, 30)
            overlap.send(packet(
                0x82, b'\x00\x09\x00' + utf('opt/a/#') + b'\x00' + utf('opt/a/b') + b'\x01'))
            header, body = overlap.read(2)
            if header != 0x90 or body[3:] != b'\x00\x01':
                raise AssertionError('overlap SUBACK %s' % body.hex())
            publish_qos(overlap, 'opt/a/b', b'once', 5, 1, 3)
            header, _ = overlap.read(2)
            if header != 0x40:
                raise AssertionError('overlap PUBACK 0x%02x' % header)
            header, body = overlap.read(2)
            if (header >> 1) & 3 != 1:
                raise AssertionError('overlap delivery qos 0x%02x' % header)
            topic, payload = publish_of(header, body, 5)
            if topic != 'opt/a/b' or payload != b'once':
                raise AssertionError((topic, payload))
            quiet(overlap, 0.3)
        finally:
            overlap.close()

        dollar = Client(broker.port, websocket)
        other = Client(broker.port, websocket)
        try:
            connect(dollar, 'opt-dollar', 5, 30)
            subscribe(dollar, '#', 5, qos=0)
            connect(other, 'opt-dollar-pub', 5, 30)
            publish(other, '$test/x', b'secret', 5)
            publish(other, 'opt/plain', b'visible', 5)
            topic, payload = publish_of(*dollar.read(2), 5)
            if topic != 'opt/plain' or payload != b'visible':
                raise AssertionError('root # matched %s %s' % (topic, payload))
            quiet(dollar, 0.3)
            subscribe(dollar, '$test/#', 5, qos=0)
            publish(other, '$test/x', b'seen', 5)
            topic, payload = publish_of(*dollar.read(2), 5)
            if topic != '$test/x' or payload != b'seen':
                raise AssertionError((topic, payload))
        finally:
            dollar.close()
            other.close()

        rap_off = Client(broker.port, websocket)
        rap_on = Client(broker.port, websocket)
        rap_pub = Client(broker.port, websocket)
        try:
            connect(rap_off, 'opt-rap0', 5, 30)
            connect(rap_on, 'opt-rap1', 5, 30)
            if sub_code(rap_off, 'opt/rap', 5, 0x00) != 0 or sub_code(rap_on, 'opt/rap', 5, 0x08) != 0:
                raise AssertionError('RAP grant')
            connect(rap_pub, 'opt-rap-pub', 5, 30)
            rap_pub.send(packet(0x31, utf('opt/rap') + b'\x00' + b'kept'))
            off_header, off_body = rap_off.read(2)
            on_header, on_body = rap_on.read(2)
            if off_header & 1 or not (on_header & 1):
                raise AssertionError('retain flags off=0x%02x on=0x%02x' % (off_header, on_header))
            if publish_of(off_header, off_body, 5)[1] != b'kept' or publish_of(on_header, on_body, 5)[1] != b'kept':
                raise AssertionError('RAP payload')
        finally:
            rap_off.close()
            rap_on.close()
            rap_pub.close()

        shared = Client(broker.port, websocket)
        try:
            connect(shared, 'opt-share', 5, 30)
            if sub_code(shared, '$share/g/t', 5, 0) != 0x9E:
                raise AssertionError('shared v5')
            subscribe(shared, 'opt/after-share', 5, qos=0)
        finally:
            shared.close()
        shared3 = Client(broker.port, websocket)
        try:
            connect(shared3, 'opt-share3', 4, 30)
            if sub_code(shared3, '$share/g/t', 4, 0) != 0x80:
                raise AssertionError('shared 3.x')
        finally:
            shared3.close()

        identified = Client(broker.port, websocket)
        try:
            connect(identified, 'opt-subid', 5, 30)
            identified.send(packet(0x82, b'\x00\x01\x02\x0b\x01' + utf('opt/sid') + b'\x00'))
            header, body = identified.read(2)
            if disconnect_reason(header, body) != 0xA1:
                raise AssertionError('subscription identifier 0x%02x %s' % (header, body.hex()))
            identified.expect_closed(1)
        finally:
            identified.close()

        bad = Client(broker.port, websocket)
        try:
            connect(bad, 'opt-bad', 5, 30)
            bad.send(packet(0x82, b'\x00\x01\x00' + utf('opt/bad') + b'\x30'))
            header, body = bad.read(2)
            if disconnect_reason(header, body) != 0x81:
                raise AssertionError('retain handling 3 -> 0x%02x' % header)
            bad.expect_closed(1)
        finally:
            bad.close()
        bad3 = Client(broker.port, websocket)
        try:
            connect(bad3, 'opt-bad3', 4, 30)
            bad3.send(packet(0x82, b'\x00\x01' + utf('opt/bad') + b'\x04'))
            bad3.expect_closed(1)
        finally:
            bad3.close()
    finally:
        broker.close()

    plain = Broker(binary, 1, 30, 10, wildcard_subscription_available=False)
    try:
        client = Client(plain.port, websocket)
        try:
            connect(client, 'opt-wild', 5, 30)
            if sub_code(client, 'opt/+/x', 5, 0) != 0xA2:
                raise AssertionError('wildcard v5')
            if sub_code(client, 'opt/exact', 5, 0, packet_id=2) != 0:
                raise AssertionError('exact filter rejected')
        finally:
            client.close()
        client3 = Client(plain.port, websocket)
        try:
            connect(client3, 'opt-wild3', 4, 30)
            if sub_code(client3, 'opt/#', 4, 0) != 0x80:
                raise AssertionError('wildcard 3.x')
        finally:
            client3.close()
    finally:
        plain.close()

    compat = Broker(binary, 1, 30, 10, implicit_no_local=True)
    try:
        sender = Client(compat.port, websocket)
        watcher = Client(compat.port, websocket)
        try:
            connect(sender, 'opt-compat', 5, 30)
            connect(watcher, 'opt-compat-watch', 5, 30)
            if sub_code(sender, 'opt/compat', 5, 0) != 0:
                raise AssertionError('compat subscribe')
            subscribe(watcher, 'opt/compat', 5, qos=0)
            publish(sender, 'opt/compat', b'nope', 5)
            topic, payload = publish_of(*watcher.read(2), 5)
            if payload != b'nope':
                raise AssertionError(payload)
            quiet(sender, 0.3)
        finally:
            sender.close()
            watcher.close()
    finally:
        compat.close()
    print('PASS P0-4 websocket=%s' % websocket, flush=True)


def expect_disconnect(client, reason):
    header, body = client.read(2)
    got = disconnect_reason(header, body)
    if got != reason:
        raise AssertionError('reason 0x%02x, want 0x%02x' % (got, reason))
    client.expect_closed(1)


def connect_user(client, client_id, username, password):
    body = utf('MQTT') + bytes([5, 0xC2]) + struct.pack('!H', 30) + b'\x00'
    body += utf(client_id) + utf(username) + utf(password)
    client.send(packet(0x10, body))
    header, result = client.read(3)
    if header != 0x20:
        raise AssertionError('CONNACK 0x%02x %s' % (header, result.hex()))
    return result[1]


class AliasPolicy(BaseHTTPRequestHandler):
    seen = []
    password = 'pw'

    def log_message(self, *_args):
        return

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path == '/authentication':
            allowed = req.get('password') == self.password and req.get('username') == req.get('clientid')
        elif self.path == '/authorization':
            if req.get('action') == 'publish':
                AliasPolicy.seen.append(req.get('topic'))
                allowed = not str(req.get('topic', '')).startswith('closed/')
            else:
                allowed = True
        else:
            allowed = False
        body = json.dumps({'result': 'allow' if allowed else 'deny', 'is_superuser': False}).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def topic_alias(binary, websocket):
    broker = Broker(binary, 1, 30, 10, topic_alias_maximum=2)
    try:
        subscriber = Client(broker.port, websocket)
        publisher = Client(broker.port, websocket)
        try:
            body = connect(subscriber, 'alias-sub', 5, 30)
            if u16_property(body, 0x22) != 2:
                raise AssertionError('CONNACK topic alias maximum %s' % u16_property(body, 0x22))
            subscribe(subscriber, 'alias/one', 5)
            subscribe(subscriber, 'alias/two', 5)
            connect(publisher, 'alias-pub', 5, 30)
            publish_alias(publisher, 'alias/one', b'first', 1)
            topic, payload = publish_of(*subscriber.read(2), 5)
            if topic != 'alias/one' or payload != b'first':
                raise AssertionError((topic, payload))
            publish_alias(publisher, '', b'second', 1)
            topic, payload = publish_of(*subscriber.read(2), 5)
            if topic != 'alias/one' or payload != b'second':
                raise AssertionError((topic, payload))
            publish_alias(publisher, 'alias/two', b'third', 1)
            topic, payload = publish_of(*subscriber.read(2), 5)
            if topic != 'alias/two' or payload != b'third':
                raise AssertionError((topic, payload))
            publish_alias(publisher, '', b'fourth', 1)
            topic, payload = publish_of(*subscriber.read(2), 5)
            if topic != 'alias/two' or payload != b'fourth':
                raise AssertionError((topic, payload))
            publish_alias(publisher, 'alias/one', b'bound', 2)
            topic, payload = publish_of(*subscriber.read(2), 5)
            if topic != 'alias/one' or payload != b'bound':
                raise AssertionError((topic, payload))
        finally:
            subscriber.close()
            publisher.close()

        invalid = Client(broker.port, websocket)
        try:
            connect(invalid, 'alias-zero', 5, 30)
            publish_alias(invalid, 'alias/one', b'nope', 0)
            expect_disconnect(invalid, 0x94)
        finally:
            invalid.close()

        over = Client(broker.port, websocket)
        try:
            connect(over, 'alias-over', 5, 30)
            publish_alias(over, 'alias/one', b'nope', 3)
            expect_disconnect(over, 0x94)
        finally:
            over.close()

        unknown = Client(broker.port, websocket)
        try:
            connect(unknown, 'alias-unknown', 5, 30)
            publish_alias(unknown, '', b'nope', 1)
            expect_disconnect(unknown, 0x82)
        finally:
            unknown.close()

        first = Client(broker.port, websocket)
        try:
            connect(first, 'alias-again', 5, 30)
            publish_alias(first, 'alias/one', b'stored', 1)
            first.send(packet(0xE0, b''))
        finally:
            first.close()
        time.sleep(0.2)
        second = Client(broker.port, websocket)
        try:
            connect(second, 'alias-again', 5, 30)
            publish_alias(second, '', b'gone', 1)
            expect_disconnect(second, 0x82)
        finally:
            second.close()
    finally:
        broker.close()

    httpd = ThreadingHTTPServer(('127.0.0.1', 0), AliasPolicy)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        token_root = Path(tempfile.mkdtemp(prefix='mqtts-alias-auth-'))
        (token_root / 'token').write_text('alias-token-0123456789abcdef0123456789')
        auth = {
            'enabled': True,
            'allow_anonymous': False,
            'cache_enabled': False,
            'providers': [{
                'type': 'http',
                'settings': {
                    'authentication_url': 'http://127.0.0.1:%d/authentication' % httpd.server_port,
                    'authorization_url': 'http://127.0.0.1:%d/authorization' % httpd.server_port,
                    'token_file': str(token_root / 'token'),
                    'timeout_ms': '500',
                    'max_payload_bytes': '1024',
                },
            }],
        }
        secured = Broker(binary, 1, 30, 10, topic_alias_maximum=2, auth=auth)
        try:
            reader = Client(secured.port, websocket)
            writer = Client(secured.port, websocket)
            try:
                tag = 'ws' if websocket else 'tcp'
                if connect_user(reader, 'reader-' + tag, 'reader-' + tag, 'pw') != 0:
                    raise AssertionError('reader auth')
                subscribe(reader, '#', 5)
                if connect_user(writer, 'writer-' + tag, 'writer-' + tag, 'pw') != 0:
                    raise AssertionError('writer auth')
                AliasPolicy.seen = []
                publish_alias(writer, 'alias/open', b'yes', 1, qos=1, packet_id=1)
                expect_puback(writer, 0x00)
                topic, payload = publish_of(*reader.read(2), 5)
                if topic != 'alias/open' or payload != b'yes':
                    raise AssertionError((topic, payload))
                AliasPolicy.seen = []
                publish_alias(writer, '', b'yes2', 1, qos=1, packet_id=2)
                expect_puback(writer, 0x00)
                topic, payload = publish_of(*reader.read(2), 5)
                if topic != 'alias/open' or payload != b'yes2' or AliasPolicy.seen != ['alias/open']:
                    raise AssertionError((topic, payload, AliasPolicy.seen))
                AliasPolicy.seen = []
                publish_alias(writer, 'closed/secret', b'no', 2, qos=1, packet_id=3)
                expect_puback(writer, 0x87)
                quiet(reader, 0.3)
                if AliasPolicy.seen != ['closed/secret']:
                    raise AssertionError(AliasPolicy.seen)
                AliasPolicy.seen = []
                publish_alias(writer, '', b'no2', 2, qos=1, packet_id=4)
                expect_puback(writer, 0x87)
                quiet(reader, 0.3)
                if AliasPolicy.seen != ['closed/secret']:
                    raise AssertionError(AliasPolicy.seen)
            finally:
                reader.close()
                writer.close()
        finally:
            secured.close()
    finally:
        httpd.shutdown()
    print('PASS P0-5 websocket=%s' % websocket, flush=True)


def will_packet(client_id, topic, payload, qos=0, retain=False, delay=0, session_expiry=None,
                clean_start=True, keep_alive=30, version=5, username=None, password=None):
    flags = 0x04
    if clean_start:
        flags |= 0x02
    flags |= (qos & 3) << 3
    if retain:
        flags |= 0x20
    if username is not None:
        flags |= 0x80
    if password is not None:
        flags |= 0x40
    name = 'MQTT' if version >= 4 else 'MQIsdp'
    body = utf(name) + bytes([version, flags]) + struct.pack('!H', keep_alive)
    if version >= 5:
        props = b''
        if session_expiry is not None:
            props += bytes([0x11]) + struct.pack('!I', session_expiry)
        body += bytes([len(props)]) + props
    body += utf(client_id)
    if version >= 5:
        will_props = b''
        if delay:
            will_props += bytes([0x18]) + struct.pack('!I', delay)
        body += bytes([len(will_props)]) + will_props
    body += utf(topic) + struct.pack('!H', len(payload)) + payload
    if username is not None:
        body += utf(username)
    if password is not None:
        body += utf(password)
    return packet(0x10, body)


def session_packet(client_id, clean_start, session_expiry, keep_alive=30):
    flags = 0x02 if clean_start else 0
    props = bytes([0x11]) + struct.pack('!I', session_expiry)
    body = utf('MQTT') + bytes([5, flags]) + struct.pack('!H', keep_alive)
    body += bytes([len(props)]) + props + utf(client_id)
    return packet(0x10, body)


def expect_connack(client, encoded, reason):
    client.send(encoded)
    header, body = client.read(3)
    if header != 0x20 or len(body) < 2 or body[1] != reason:
        raise AssertionError('CONNACK 0x%02x %s, want reason 0x%02x' % (header, body.hex(), reason))
    return body


def open_session(client, encoded):
    client.send(encoded)
    header, body = client.read(3)
    if header != 0x20 or len(body) < 2 or body[1] != 0:
        raise AssertionError('CONNACK 0x%02x %s' % (header, body.hex()))
    return body


def disconnect_packet(client, reason):
    client.send(packet(0xE0, b'' if reason == 0 else bytes([reason])))


def expect_will(client, topic, payload, retain=None, timeout=3):
    started = time.monotonic()
    header, body = client.read(timeout)
    elapsed = time.monotonic() - started
    got_topic, got_payload = publish_of(header, body, 5)
    if got_topic != topic or got_payload != payload:
        raise AssertionError((got_topic, got_payload, topic, payload))
    if retain is not None and bool(header & 1) != retain:
        raise AssertionError('retain %s, want %s, header 0x%02x' % (bool(header & 1), retain, header))
    return elapsed


def will_rejections(binary, websocket):
    broker = Broker(binary, 1, 30, 10, max_qos=1, retain_available=False)
    try:
        for client_id, topic, qos, retain, reason in (
            ('will-qos', 'will/qos', 2, False, 0x9B),
            ('will-retain', 'will/retain', 0, True, 0x9A),
            ('will-plus', 'bad/+/x', 0, False, 0x82),
            ('will-hash', 'bad/#', 0, False, 0x82),
            ('will-empty', '', 0, False, 0x82),
        ):
            client = Client(broker.port, websocket)
            try:
                expect_connack(client, will_packet(client_id, topic, b'x', qos=qos, retain=retain), reason)
            finally:
                client.close()
    finally:
        broker.close()


def will_delivery(binary, websocket):
    broker = Broker(binary, 1, 30, 10, monitor=True)
    try:
        watcher = Client(broker.port, websocket)
        try:
            connect(watcher, 'will-watch', 5, 30)
            subscribe(watcher, 'will/live', 5)
            killed = Client(broker.port, websocket)
            try:
                open_session(killed, will_packet('will-kill', 'will/live', b'killed'))
            finally:
                killed.close()
            expect_will(watcher, 'will/live', b'killed')

            normal = Client(broker.port, websocket)
            try:
                open_session(normal, will_packet('will-normal', 'will/live', b'normal'))
                disconnect_packet(normal, 0)
                quiet(watcher, 0.4)
            finally:
                normal.close()

            explicit = Client(broker.port, websocket)
            try:
                open_session(explicit, will_packet('will-zero', 'will/live', b'zero'))
                explicit.send(packet(0xE0, bytes([0x00])))
                quiet(watcher, 0.4)
            finally:
                explicit.close()

            gone = Client(broker.port, websocket)
            try:
                open_session(gone, will_packet('will-with-will', 'will/live', b'disconnect-will'))
                disconnect_packet(gone, 0x04)
                expect_will(watcher, 'will/live', b'disconnect-will')
            finally:
                gone.close()

            legacy = Client(broker.port, websocket)
            try:
                open_session(legacy, will_packet('will-v3', 'will/live', b'legacy', version=4))
                legacy.send(packet(0xE0, b''))
                quiet(watcher, 0.4)
            finally:
                legacy.close()
            legacy_kill = Client(broker.port, websocket)
            try:
                open_session(legacy_kill, will_packet('will-v3-kill', 'will/live', b'legacy-kill', version=4))
            finally:
                legacy_kill.close()
            expect_will(watcher, 'will/live', b'legacy-kill')

            sleepy = Client(broker.port, websocket)
            try:
                open_session(sleepy, will_packet('will-ka', 'will/live', b'keepalive', keep_alive=2))
                started = time.monotonic()
                expect_will(watcher, 'will/live', b'keepalive', timeout=8)
                elapsed = time.monotonic() - started
                if not 2.2 <= elapsed <= 6.5:
                    raise AssertionError('keepalive will after %.2fs' % elapsed)
            finally:
                sleepy.close()

            old = Client(broker.port, websocket)
            try:
                open_session(old, will_packet('will-take', 'will/live', b'taken'))
                new = Client(broker.port, websocket)
                try:
                    connect(new, 'will-take', 5, 30)
                    expect_will(watcher, 'will/live', b'taken')
                finally:
                    new.close()
            finally:
                old.close()

            # 默认订阅会清掉 retain。Retain As Published 才能在在线转发里看见遗嘱的 retain 位。
            # 新订阅者事后收到保留遗嘱要等 P0-7 的保留库。
            published = Client(broker.port, websocket)
            try:
                connect(published, 'will-rap', 5, 30)
                if sub_code(published, 'will/live', 5, 0x08, packet_id=7) != 0:
                    raise AssertionError('Retain As Published was not granted')
                retained = Client(broker.port, websocket)
                try:
                    open_session(retained, will_packet('will-ret', 'will/live', b'kept', retain=True))
                finally:
                    retained.close()
                expect_will(watcher, 'will/live', b'kept', retain=False)
                expect_will(published, 'will/live', b'kept', retain=True)
            finally:
                published.close()
            broker.wait_counter('wills_published', 1)
        finally:
            watcher.close()
    finally:
        broker.close()
    print('PASS P0-6 immediate websocket=%s' % websocket, flush=True)


def will_authorization(binary, websocket):
    httpd = ThreadingHTTPServer(('127.0.0.1', 0), AliasPolicy)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    token_root = Path(tempfile.mkdtemp(prefix='mqtts-will-auth-'))
    try:
        (token_root / 'token').write_text('will-token-0123456789abcdef0123456789ab')
        auth = {
            'enabled': True,
            'allow_anonymous': False,
            'cache_enabled': False,
            'providers': [{
                'type': 'http',
                'settings': {
                    'authentication_url': 'http://127.0.0.1:%d/authentication' % httpd.server_port,
                    'authorization_url': 'http://127.0.0.1:%d/authorization' % httpd.server_port,
                    'token_file': str(token_root / 'token'),
                    'timeout_ms': '500',
                    'max_payload_bytes': '1024',
                },
            }],
        }
        broker = Broker(binary, 1, 30, 10, monitor=True, auth=auth)
        try:
            watcher = Client(broker.port, websocket)
            denied = Client(broker.port, websocket)
            try:
                if connect_user(watcher, 'will-reader', 'will-reader', AliasPolicy.password) != 0:
                    raise AssertionError('watcher auth')
                subscribe(watcher, 'closed/will', 5)
                encoded = will_packet(
                    'will-denied', 'closed/will', b'secret', username='will-denied', password=AliasPolicy.password)
                if open_session(denied, encoded)[1] != 0:
                    raise AssertionError('publisher auth')
            finally:
                denied.close()
            quiet(watcher, 0.6)
            broker.wait_counter('wills_rejected', 1)
            watcher.close()
        finally:
            broker.close()
    finally:
        httpd.shutdown()
    print('PASS P0-6 auth websocket=%s' % websocket, flush=True)


def publish_retain(client, topic, payload, version, qos=0, packet_id=1, expiry=None):
    body = utf(topic)
    if qos:
        body += struct.pack('!H', packet_id)
    if version >= 5:
        props = b''
        if expiry is not None:
            props += bytes([0x02]) + struct.pack('!I', expiry)
        body += bytes([len(props)]) + props
    body += payload
    client.send(packet(0x31 | (qos << 1), body))


def expect_retained(client, topic, payload, version, qos=0, timeout=2):
    header, body = client.read(timeout)
    if header & 1 != 1 or ((header >> 1) & 3) != qos:
        raise AssertionError('header 0x%02x, want retain qos %s' % (header, qos))
    got_topic, got_payload = publish_of(header, body, version)
    if got_topic != topic or got_payload != payload:
        raise AssertionError((got_topic, got_payload, topic, payload))
    if qos:
        puback(client, int.from_bytes(body[2 + int.from_bytes(body[:2], 'big'):4 + int.from_bytes(body[:2], 'big')], 'big'))
    return header, body


class RetainAuth(BaseHTTPRequestHandler):
    password = 'pw'

    def log_message(self, *_args):
        return

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path == '/authentication':
            allowed = req.get('password') == self.password and req.get('username') == req.get('clientid')
        elif self.path == '/authorization':
            topic = str(req.get('topic', ''))
            if req.get('action') == 'publish':
                allowed = True
            else:
                allowed = not topic.startswith('secret/')
        else:
            allowed = False
        body = json.dumps({'result': 'allow' if allowed else 'deny', 'is_superuser': False}).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def retain_messages(binary, websocket):
    broker = Broker(binary, 1, 30, 10, monitor=True)
    try:
        publisher = Client(broker.port, websocket)
        watcher = Client(broker.port, websocket)
        try:
            connect(publisher, 'ret-pub', 5, 30)
            connect(watcher, 'ret-watch', 5, 30)
            subscribe(watcher, 'gone/t', 5)
            subscribe(watcher, 'stay/t', 5)
            publish_retain(publisher, 'keep/t', b'kept', 5)
            publish_retain(publisher, 'a/b', b'ab', 5)
            publish_retain(publisher, 'a/c', b'ac', 5)
            publish_retain(publisher, 'a/b/c', b'deep', 5)
            publish_retain(publisher, 'sport', b'root', 5)
            publish_retain(publisher, 'sport/tennis', b'ball', 5)
            publish_retain(publisher, '$hidden/a', b'secret', 5)
            publish_retain(publisher, 'visible/a', b'ok', 5)
            publish_retain(publisher, 'gone/t', b'x', 5)
            topic, payload = publish_of(*watcher.read(2), 5)
            if topic != 'gone/t' or payload != b'x':
                raise AssertionError((topic, payload))
            publish_retain(publisher, 'gone/t', b'', 5)
            topic, payload = publish_of(*watcher.read(2), 5)
            if topic != 'gone/t' or payload != b'':
                raise AssertionError((topic, payload))
            publish_retain(publisher, 'stay/t', b'first', 5)
            topic, payload = publish_of(*watcher.read(2), 5)
            if payload != b'first':
                raise AssertionError(payload)
            publish(publisher, 'stay/t', b'second', 5)
            topic, payload = publish_of(*watcher.read(2), 5)
            if payload != b'second':
                raise AssertionError(payload)
            publish_retain(publisher, 'qos/t', b'ab', 5, qos=2, packet_id=7)
            header, body = publisher.read(2)
            if header != 0x50:
                raise AssertionError('PUBREC 0x%02x' % header)
            publish_retain(publisher, 'exp/soon', b'gone', 5, expiry=1)
            publish_retain(publisher, 'exp/keep', b'stay', 5)
            publish_retain(publisher, 'rh/t', b'live', 5)

            reader = Client(broker.port, websocket)
            try:
                connect(reader, 'ret-read', 5, 30)
                subscribe(reader, 'keep/t', 5)
                expect_retained(reader, 'keep/t', b'kept', 5)
                subscribe(reader, 'gone/t', 5)
                quiet(reader, 0.3)
                if sub_code(reader, 'a/+', 5, 0, packet_id=2) != 0:
                    raise AssertionError('plus grant')
                expect_retained(reader, 'a/b', b'ab', 5)
                expect_retained(reader, 'a/c', b'ac', 5)
                quiet(reader, 0.2)
                if sub_code(reader, 'a/#', 5, 0, packet_id=3) != 0:
                    raise AssertionError('hash grant')
                expect_retained(reader, 'a/b', b'ab', 5)
                expect_retained(reader, 'a/b/c', b'deep', 5)
                expect_retained(reader, 'a/c', b'ac', 5)
                if sub_code(reader, 'sport/#', 5, 0, packet_id=4) != 0:
                    raise AssertionError('sport hash')
                expect_retained(reader, 'sport', b'root', 5)
                expect_retained(reader, 'sport/tennis', b'ball', 5)
                if sub_code(reader, '#', 5, 0, packet_id=5) != 0:
                    raise AssertionError('root hash')
                saw_secret = False
                saw_visible = False
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline and not saw_visible:
                    header, body = reader.read(max(0.1, deadline - time.monotonic()))
                    topic, payload = publish_of(header, body, 5)
                    if topic == '$hidden/a':
                        saw_secret = True
                    if topic == 'visible/a' and payload == b'ok' and header & 1:
                        saw_visible = True
                if saw_secret or not saw_visible:
                    raise AssertionError('root hash secret=%s visible=%s' % (saw_secret, saw_visible))
                if sub_code(reader, '$hidden/#', 5, 0, packet_id=6) != 0:
                    raise AssertionError('dollar filter')
                expect_retained(reader, '$hidden/a', b'secret', 5)
                subscribe(reader, 'stay/t', 5)
                expect_retained(reader, 'stay/t', b'first', 5)
                if sub_code(reader, 'qos/t', 5, 1, packet_id=8) != 1:
                    raise AssertionError('qos grant')
                expect_retained(reader, 'qos/t', b'ab', 5, qos=1)
            finally:
                reader.close()

            handling = Client(broker.port, websocket)
            try:
                connect(handling, 'ret-rh', 5, 30)
                if sub_code(handling, 'rh/t', 5, 0x10) != 0:
                    raise AssertionError('retain handling 1')
                expect_retained(handling, 'rh/t', b'live', 5)
                if sub_code(handling, 'rh/t', 5, 0x10, packet_id=2) != 0:
                    raise AssertionError('retain handling 1 repeat')
                quiet(handling, 0.3)
            finally:
                handling.close()
            blocked = Client(broker.port, websocket)
            try:
                connect(blocked, 'ret-rh2', 5, 30)
                if sub_code(blocked, 'rh/t', 5, 0x20) != 0:
                    raise AssertionError('retain handling 2')
                quiet(blocked, 0.3)
            finally:
                blocked.close()

            time.sleep(1.6)
            expired = Client(broker.port, websocket)
            try:
                connect(expired, 'ret-exp', 5, 30)
                subscribe(expired, 'exp/#', 5)
                expect_retained(expired, 'exp/keep', b'stay', 5)
                quiet(expired, 0.3)
            finally:
                expired.close()

            legacy = Client(broker.port, websocket)
            try:
                connect(legacy, 'ret-v3', 4, 30)
                publish_retain(legacy, 'v3/t', b'old', 4)
            finally:
                legacy.close()
            legacy_sub = Client(broker.port, websocket)
            try:
                connect(legacy_sub, 'ret-v3-sub', 4, 30)
                subscribe(legacy_sub, 'v3/t', 4)
                expect_retained(legacy_sub, 'v3/t', b'old', 4)
            finally:
                legacy_sub.close()

            will_watch = Client(broker.port, websocket)
            try:
                connect(will_watch, 'ret-will-watch', 5, 30)
                subscribe(will_watch, 'will/kept', 5)
                dying = Client(broker.port, websocket)
                try:
                    open_session(dying, will_packet('ret-will', 'will/kept', b'kept', retain=True))
                finally:
                    dying.close()
                expect_will(will_watch, 'will/kept', b'kept', retain=False)
            finally:
                will_watch.close()
            will_new = Client(broker.port, websocket)
            try:
                connect(will_new, 'ret-will-new', 5, 30)
                subscribe(will_new, 'will/kept', 5)
                expect_retained(will_new, 'will/kept', b'kept', 5)
            finally:
                will_new.close()
            broker.wait_counter('retained_messages', 1)
        finally:
            publisher.close()
            watcher.close()
    finally:
        broker.close()

    limited = Broker(binary, 1, 30, 10, monitor=True, retain={
        'max_messages': 2,
        'max_payload_bytes': 4,
        'max_total_bytes': 4,
        'max_deliver_per_subscribe': 1,
    })
    try:
        publisher = Client(limited.port, websocket)
        online = Client(limited.port, websocket)
        try:
            connect(publisher, 'lim-pub', 5, 30)
            connect(online, 'lim-on', 5, 30)
            subscribe(online, 'lim/#', 5)
            publish_retain(publisher, 'lim/a', b'aa', 5)
            publish_retain(publisher, 'lim/b', b'bb', 5)
            publish_retain(publisher, 'lim/c', b'cc', 5, qos=1, packet_id=1)
            expect_puback(publisher, 0x00)
            publish_retain(publisher, 'lim/big', b'12345', 5)
            for payload in (b'aa', b'bb', b'cc', b'12345'):
                topic, got = publish_of(*online.read(2), 5)
                if not topic.startswith('lim/') or got != payload:
                    raise AssertionError((topic, got, payload))
        finally:
            publisher.close()
            online.close()
        late = Client(limited.port, websocket)
        try:
            connect(late, 'lim-late', 5, 30)
            subscribe(late, 'lim/#', 5)
            header, body = late.read(2)
            if header & 1 != 1:
                raise AssertionError('truncated retain header 0x%02x' % header)
            topic, payload = publish_of(header, body, 5)
            if topic != 'lim/a' or payload != b'aa':
                raise AssertionError((topic, payload))
            quiet(late, 0.3)
            subscribe(late, 'lim/big', 5)
            quiet(late, 0.3)
        finally:
            late.close()
        limited.wait_counter('retained_not_stored', 1)
        limited.wait_counter('retained_delivery_truncated', 1)
        if limited.counter('retained_messages') != 2:
            raise AssertionError('retained_messages %s' % limited.counter('retained_messages'))
    finally:
        limited.close()

    httpd = ThreadingHTTPServer(('127.0.0.1', 0), RetainAuth)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        token_root = Path(tempfile.mkdtemp(prefix='mqtts-retain-auth-'))
        (token_root / 'token').write_text('retain-token-0123456789abcdef0123456789')
        auth = {
            'enabled': True,
            'allow_anonymous': False,
            'cache_enabled': False,
            'providers': [{
                'type': 'http',
                'settings': {
                    'authentication_url': 'http://127.0.0.1:%d/authentication' % httpd.server_port,
                    'authorization_url': 'http://127.0.0.1:%d/authorization' % httpd.server_port,
                    'token_file': str(token_root / 'token'),
                    'timeout_ms': '500',
                    'max_payload_bytes': '1024',
                },
            }],
        }
        secured = Broker(binary, 1, 30, 10, auth=auth)
        try:
            tag = 'ws' if websocket else 'tcp'
            writer = Client(secured.port, websocket)
            reader = Client(secured.port, websocket)
            try:
                if connect_user(writer, 'writer-' + tag, 'writer-' + tag, RetainAuth.password) != 0:
                    raise AssertionError('writer auth')
                publish_retain(writer, 'secret/hidden', b'no', 5)
                publish_retain(writer, 'public/ok', b'yes', 5)
                if connect_user(reader, 'reader-' + tag, 'reader-' + tag, RetainAuth.password) != 0:
                    raise AssertionError('reader auth')
                subscribe(reader, '#', 5)
                expect_retained(reader, 'public/ok', b'yes', 5)
                quiet(reader, 0.4)
            finally:
                writer.close()
                reader.close()
        finally:
            secured.close()
    finally:
        httpd.shutdown()

    disabled = Broker(binary, 1, 30, 10, retain_available=False)
    try:
        client = Client(disabled.port, websocket)
        try:
            connect(client, 'ret-off', 5, 30)
            publish_retain(client, 'off/t', b'x', 5)
            expect_disconnect(client, 0x9A)
        finally:
            client.close()
        legacy = Client(disabled.port, websocket)
        try:
            connect(legacy, 'ret-off-v3', 4, 30)
            publish_retain(legacy, 'off/t', b'x', 4)
            legacy.expect_closed(2)
        finally:
            legacy.close()
        plain = Client(disabled.port, websocket)
        try:
            connect(plain, 'ret-off-ok', 5, 30)
            subscribe(plain, 'off/plain', 5)
            publish(plain, 'off/plain', b'ok', 5)
            topic, payload = publish_of(*plain.read(2), 5)
            if topic != 'off/plain' or payload != b'ok':
                raise AssertionError((topic, payload))
        finally:
            plain.close()
    finally:
        disabled.close()
    print('PASS P0-7 websocket=%s' % websocket, flush=True)


def will_delay_and_no_local(binary):
    broker = Broker(binary, 1, 30, 10, persistence=True)
    try:
        watcher = Client(broker.port, False)
        try:
            open_session(watcher, session_packet('will-delay-watch', True, 60))
            subscribe(watcher, 'will/delay', 5)
            subscribe(watcher, 'will/short', 5)
            subscribe(watcher, 'will/clean', 5)
            subscribe(watcher, 'will/nl', 5)

            delayed = Client(broker.port, False)
            try:
                open_session(delayed, will_packet(
                    'will-delay', 'will/delay', b'later', delay=5, session_expiry=60))
            finally:
                delayed.close()
            elapsed = expect_will(watcher, 'will/delay', b'later', timeout=9)
            if not 4.0 <= elapsed <= 8.5:
                raise AssertionError('delayed will after %.2fs' % elapsed)

            cancelled = Client(broker.port, False)
            try:
                open_session(cancelled, will_packet(
                    'will-cancel', 'will/delay', b'cancelled', delay=5, session_expiry=60))
            finally:
                cancelled.close()
            cancel_at = time.monotonic()
            resumed = Client(broker.port, False)
            try:
                present = open_session(resumed, session_packet('will-cancel', False, 60))
                if present[0] & 1 != 1:
                    raise AssertionError('resume did not present the session: %s' % present.hex())
                remaining = 7 - (time.monotonic() - cancel_at)
                if remaining < 1:
                    remaining = 1
                quiet(watcher, remaining)
            finally:
                resumed.close()

            short = Client(broker.port, False)
            try:
                open_session(short, will_packet(
                    'will-short', 'will/short', b'short', delay=30, session_expiry=2))
            finally:
                short.close()
            short_elapsed = expect_will(watcher, 'will/short', b'short', timeout=6)
            if not 1.2 <= short_elapsed <= 5.0:
                raise AssertionError('session expiry capped will after %.2fs' % short_elapsed)

            cleaning = Client(broker.port, False)
            try:
                open_session(cleaning, will_packet(
                    'will-clean', 'will/clean', b'clean', delay=30, session_expiry=60))
            finally:
                cleaning.close()
            quiet(watcher, 0.8)
            replacement = Client(broker.port, False)
            try:
                open_session(replacement, session_packet('will-clean', True, 60))
                clean_elapsed = expect_will(watcher, 'will/clean', b'clean', timeout=2)
                if clean_elapsed > 2:
                    raise AssertionError('clean start will after %.2fs' % clean_elapsed)
            finally:
                replacement.close()

            def queued_will(client_id, flags):
                owner = Client(broker.port, False)
                try:
                    open_session(owner, will_packet(client_id, 'will/nl', client_id.encode(), qos=1, session_expiry=60))
                    granted = sub_code(owner, 'will/nl', 5, flags, packet_id=1)
                    if granted != 1:
                        raise AssertionError('granted %s for flags 0x%02x' % (granted, flags))
                finally:
                    owner.close()
                expect_will(watcher, 'will/nl', client_id.encode(), timeout=3)
                again = Client(broker.port, False)
                try:
                    present = open_session(again, session_packet(client_id, False, 60))
                    if present[0] & 1 != 1:
                        raise AssertionError('queued resume missing session')
                    if flags & 0x04:
                        quiet(again, 0.8)
                    else:
                        topic, payload = publish_of(*again.read(2), 5)
                        if topic != 'will/nl' or payload != client_id.encode():
                            raise AssertionError((topic, payload))
                finally:
                    again.close()

            queued_will('will-nolocal', 0x05)
            queued_will('will-local', 0x01)
        finally:
            watcher.close()
    finally:
        broker.close()
    print('PASS P0-6 delay', flush=True)


def run(binary):
    for websocket in (False, True):
        will_rejections(binary, websocket)
        will_delivery(binary, websocket)
        retain_messages(binary, websocket)
        will_authorization(binary, websocket)
        topic_alias(binary, websocket)
        subscription_options(binary, websocket)
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
    will_delay_and_no_local(binary)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    args = parser.parse_args()
    run(args.broker)


if __name__ == '__main__':
    main()
