#!/usr/bin/env python3
"""Exercise a real broker over MQTT 3.1.1, MQTT 5 and WebSocket with HTTP ACLs."""
import argparse
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import tempfile
import threading
import time


def utf(value):
    value = value.encode()
    return struct.pack('!H', len(value)) + value


def packet(header, body):
    size, encoded = len(body), bytearray()
    while True:
        byte, size = size % 128, size // 128
        encoded.append(byte | (128 if size else 0))
        if not size:
            return bytes([header]) + encoded + body


def exact(sock, count):
    data = b''
    while len(data) < count:
        more = sock.recv(count - len(data))
        if not more:
            raise EOFError('connection closed')
        data += more
    return data


class Client:
    def __init__(self, port, version, websocket=False):
        self.version, self.websocket = version, websocket
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.buffer = b''
        if websocket:
            key = base64.b64encode(os.urandom(16)).decode()
            self.sock.sendall((f'GET /mqtt HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: mqtt\r\n\r\n').encode())
            response = b''
            while not response.endswith(b'\r\n\r\n'):
                response += exact(self.sock, 1)
            assert response.startswith(b'HTTP/1.1 101'), response
            accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
            assert accept in response

    def send(self, payload):
        if self.websocket:
            mask = os.urandom(4)
            size = len(payload)
            header = bytes([0x82, 0x80 | size]) if size < 126 else b'\x82\xfe' + struct.pack('!H', size)
            payload = header + mask + bytes(v ^ mask[i % 4] for i, v in enumerate(payload))
        self.sock.sendall(payload)

    def read(self):
        def take(count):
            while len(self.buffer) < count:
                if not self.websocket:
                    self.buffer += exact(self.sock, count - len(self.buffer))
                else:
                    header = exact(self.sock, 2)
                    if header[0] & 15 == 8:
                        raise EOFError('websocket closed')
                    size = header[1] & 127
                    if size == 126:
                        size = struct.unpack('!H', exact(self.sock, 2))[0]
                    elif size == 127:
                        size = struct.unpack('!Q', exact(self.sock, 8))[0]
                    self.buffer += exact(self.sock, size)
            result, self.buffer = self.buffer[:count], self.buffer[count:]
            return result
        header, length, shift = take(1)[0], 0, 0
        while True:
            byte = take(1)[0]
            length += (byte & 127) << shift
            if not byte & 128:
                break
            shift += 7
            assert shift <= 21
        return header, take(length)

    def connect(self, name, password='test-password', client_id=None):
        self.name = name
        body = utf('MQTT') + bytes([self.version, 0xc2, 0, 30])
        if self.version == 5:
            body += b'\x00'
        self.send(packet(0x10, body + utf(client_id or name) + utf(name) + utf(password)))
        header, result = self.read()
        assert header == 0x20
        return result[1]

    def subscribe(self, topic):
        self.send(packet(0x82, b'\x00\x01' + (b'\x00' if self.version == 5 else b'') + utf(topic) + b'\x00'))
        header, result = self.read()
        assert header == 0x90
        return result[-1]

    def publish(self, topic, payload, sender=None):
        payload = json.dumps({'from': {'type': 'user', 'id': sender or self.name}, 'body': payload.decode()}).encode()
        self.send(packet(0x30, utf(topic) + (b'\x00' if self.version == 5 else b'') + payload))

    def message(self):
        header, result = self.read()
        assert header >> 4 == 3
        length = struct.unpack('!H', result[:2])[0]
        offset = 2 + length + (2 if (header >> 1) & 3 else 0)
        if self.version == 5:
            assert result[offset] == 0
            offset += 1
        return result[2:2+length].decode(), json.loads(result[offset:])['body'].encode()

    def no_message(self):
        self.sock.settimeout(.3)
        try:
            received = self.read()
        except (TimeoutError, EOFError, ConnectionError):
            return
        finally:
            self.sock.settimeout(3)
        raise AssertionError(f'unauthorized delivery: {received}')

    def close(self):
        self.sock.close()


def run(binary):
    token = secrets.token_hex(32)
    state = {'revoked': set(), 'mode': 'normal'}
    class Callback(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            assert self.headers.get('X-Broker-Token') == token
            req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            name = req.get('username', '')
            allowed = name.startswith(('reader', 'writer', 'quoted')) and name not in state['revoked'] and req.get('clientid') == name
            if self.path == '/authentication':
                allowed &= req.get('password') == ('p"\\word' if name.startswith('quoted') else 'test-password')
            elif self.path == '/authorization':
                allowed &= req.get('topic') == 'chat/allowed' and req.get('action') in ('subscribe', 'publish')
                if req.get('action') == 'publish':
                    allowed &= req.get('message', {}).get('from', {}).get('id') == name
                if name.startswith('reader'):
                    allowed &= req.get('action') == 'subscribe'
            else:
                allowed = False
            body = json.dumps({'result': 'allow' if allowed else 'deny', 'is_superuser': False}).encode()
            if state['mode'] == 'broken':
                body = b'not json'
            if state['mode'] == 'oversized':
                body = b'x' * 9000
            if state['mode'] == 'expired':
                body = json.dumps({'result': 'allow', 'expire_at': int(time.time()) - 1}).encode()
            if state['mode'] == 'short-lived':
                body = json.dumps({'result': 'allow', 'expire_at': int(time.time()) + 2}).encode()
            self.send_response(503 if state['mode'] == 'offline' else 200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Callback)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='mqtts-auth-') as directory:
        root = Path(directory)
        (root / 'token').write_text(token)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        config = {'server': {'bind_address': '127.0.0.1', 'port': port, 'thread_count': 2}, 'monitoring': {'enabled': False}, 'log': {'level': 'warn'},
                  'auth': {'enabled': True, 'allow_anonymous': False, 'cache_enabled': True, 'providers': [{'type': 'http', 'settings': {
                      'authentication_url': f'http://127.0.0.1:{httpd.server_port}/authentication', 'authorization_url': f'http://127.0.0.1:{httpd.server_port}/authorization', 'token_file': str(root / 'token'), 'timeout_ms': 500, 'message_identity_prefix': 'chat/'}}]}}
        (root / 'config.json').write_text(json.dumps(config))
        with (root / 'broker.log').open('w') as log:
            proc = subprocess.Popen([binary, '-c', str(root / 'config.json')], stdout=log, stderr=log)
            clients = []
            def client(version, websocket=False):
                item = Client(port, version, websocket)
                clients.append(item)
                return item
            try:
                deadline = time.monotonic() + 8
                while True:
                    if proc.poll() is not None:
                        raise AssertionError((root / 'broker.log').read_text())
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=.2):
                            break
                    except OSError:
                        assert time.monotonic() < deadline, 'broker did not start'
                        time.sleep(.05)
                for version, websocket in [(4, False), (5, False), (4, True), (5, True)]:
                    tag = f'{version}-{websocket}'
                    assert client(version, websocket).connect('unknown') != 0
                    assert client(version, websocket).connect('writer-bad', 'wrong') != 0
                    assert client(version, websocket).connect('writer-id', client_id='other') != 0
                    reader, writer = client(version, websocket), client(version, websocket)
                    assert reader.connect('reader-' + tag) == 0
                    assert writer.connect('writer-' + tag) == 0
                    assert reader.subscribe('chat/#') >= 128
                    assert reader.subscribe('chat/allowed') == 0
                    writer.publish('chat/allowed', b'permitted')
                    assert reader.message() == ('chat/allowed', b'permitted')
                    spoof = client(version, websocket)
                    assert spoof.connect('writer-spoof-' + tag) == 0
                    spoof.publish('chat/allowed', b'forged sender', sender='someone-else')
                    reader.no_message()
                    state['revoked'].add('reader-' + tag)
                    writer.publish('chat/allowed', b'revoked reader')
                    reader.no_message()
                    # Existing subscriptions are checked again after role/membership changes.
                    state['revoked'].remove('reader-' + tag)
                    state['revoked'].add('writer-' + tag)
                    writer.publish('chat/allowed', b'revoked writer')
                    reader.no_message()
                    print(f'PASS MQTT {version}, websocket={websocket}: credentials, client binding, ACL, sender identity, live revocation')
                assert client(5).connect('quoted-user', 'p"\\word') == 0
                for mode in ['broken', 'oversized', 'expired', 'offline']:
                    state['mode'] = mode
                    assert client(5).connect('writer-failure') != 0, mode
                state['mode'] = 'normal'
                state['mode'] = 'short-lived'
                expiring = client(5, True)
                assert expiring.connect('reader-expiring') == 0
                assert expiring.subscribe('chat/allowed') == 0
                state['mode'] = 'normal'
                time.sleep(2.1)
                active = client(5)
                assert active.connect('writer-active') == 0
                active.publish('chat/allowed', b'after session expiry')
                expiring.no_message()
                # Deny legacy JSON/text commands before MQTT authentication.
                legacy = client(5, True)
                mask = b'abcd'
                data = b'{"type":"subscribe","topic":"chat/allowed"}'
                legacy.sock.sendall(bytes([0x81, 0x80 | len(data)]) + mask + bytes(v ^ mask[i % 4] for i, v in enumerate(data)))
                legacy.no_message()
                print('PASS fail-closed callbacks, expired credentials, JSON escaping, legacy WebSocket denial')
            finally:
                for item in clients:
                    item.close()
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait()
                httpd.shutdown()
        # Enabled authentication must never open an anonymous listener when
        # configuration is incomplete or the callback secret is absent.
        for broken_auth in [dict(enabled=True, allow_anonymous=False, providers=[]),
                            dict(enabled=True, allow_anonymous=True, providers=[]),
                            dict(enabled=True, allow_anonymous=False, providers=[{'type': 'http', 'settings': {}}])]:
            config['auth'] = broken_auth
            (root / 'invalid.json').write_text(json.dumps(config))
            rejected = subprocess.run([binary, '-c', str(root / 'invalid.json')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
            assert rejected.returncode != 0, 'invalid authentication configuration started successfully'
        print('PASS credential expiry on existing subscriptions and fail-closed startup')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    run(str(Path(parser.parse_args().broker).resolve()))
