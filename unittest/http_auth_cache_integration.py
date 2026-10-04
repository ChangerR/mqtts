#!/usr/bin/env python3
"""Real MQTT cache/failure-isolation checks; no application code or database."""
import argparse
import base64
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import http.server
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import threading
import time

from http_auth_integration import Client


class Fixture:
    def __init__(self, binary, **settings):
        self.binary, self.settings = binary, settings
        self.token = secrets.token_hex(32)
        self.counts, self.revoked = Counter(), set()
        self.requests_by_user = Counter()
        self.revision = 'initial'
        self.delay, self.mode, self.consent = 0, 'normal', True
        self.fresh, self.age, self.expiry = 60000, 300000, 0
        self.active, self.peak = 0, 0
        self.wildcards = False
        self.delayed_users = None
        self.lock, self.seen = threading.Lock(), threading.Event()
        self.clients = []

    def __enter__(self):
        fixture = self
        class Callback(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def setup(self):
                super().setup()
                self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            def log_message(self, *_):
                pass
            def do_POST(self):
                assert self.headers.get('X-Broker-Token') == fixture.token
                req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                with fixture.lock:
                    fixture.counts[self.path] += 1
                    revision, mode, delay = fixture.revision, fixture.mode, fixture.delay
                    name, topic = req.get('username', ''), req.get('topic', '')
                    fixture.requests_by_user[(self.path, name)] += 1
                    allowed = name not in fixture.revoked
                    if self.path == '/authentication':
                        allowed &= req.get('password') == 'test-password'
                    elif self.path == '/authorization':
                        allowed &= topic.startswith('fixture/') and ((fixture.wildcards and req.get('action') == 'subscribe') or ('#' not in topic and '+' not in topic))
                        if req.get('action') == 'publish':
                            raw = base64.b64decode(req['payload'], validate=True)
                            try:
                                value = json.loads(raw)
                                # Case aliases are deliberately interpreted here;
                                # the broker must never ignore unknown JSON fields.
                                identity = {k.lower(): v for k, v in value.items()}
                                allowed &= identity.get('actor') == name and identity.get('scope', topic) == topic
                            except (ValueError, AttributeError):
                                allowed = False
                    body = {'result': 'allow' if allowed else 'deny'}
                    if fixture.expiry:
                        body['expire_at'] = fixture.expiry
                    if fixture.consent:
                        body.update(cache_ttl_ms=fixture.fresh, cache_max_age_ms=fixture.age, cache_revision=revision)
                    if self.path == '/version':
                        body = {'cache_revision': revision}
                        delay = 0
                    else:
                        if fixture.delayed_users is not None and name not in fixture.delayed_users:
                            delay = 0
                        fixture.active += 1
                        fixture.peak = max(fixture.peak, fixture.active)
                        fixture.seen.set()
                if delay:
                    time.sleep(delay)
                if self.path != '/version':
                    with fixture.lock:
                        fixture.active -= 1
                data = json.dumps(body).encode() if mode != 'malformed' else b'broken JSON'
                try:
                    self.send_response(503 if mode == 'offline' else 200)
                    self.send_header('Content-Length', str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass
        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Callback)
        self.httpd.daemon_threads = True
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.directory = tempfile.TemporaryDirectory(prefix='mqtts-cache-')
        self.root = Path(self.directory.name)
        (self.root / 'token').write_text(self.token)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        url = f'http://127.0.0.1:{self.httpd.server_port}'
        settings = dict(authentication_url=url+'/authentication', authorization_url=url+'/authorization',
                        token_file=str(self.root/'token'), timeout_ms=400, publish_payload='base64',
                        cache_ttl_ms=60000, cache_max_age_ms=300000, cache_max_entries=256,
                        publish_cache_ignored_fields='["nonce","data"]', http_workers=2,
                        http_queue_capacity=4, failure_cooldown_ms=200)
        settings.update(self.settings)
        server_threads = settings.pop('server_threads', 1)
        if settings.pop('version_feed', False):
            settings.update(cache_version_url=url+'/version', cache_version_interval_ms=100)
        if not settings['cache_ttl_ms']:
            settings.pop('publish_cache_ignored_fields')
        config = dict(server=dict(bind_address='127.0.0.1', port=self.port, thread_count=server_threads),
                      monitoring=dict(enabled=False), log=dict(level='warn'),
                      auth=dict(enabled=True, allow_anonymous=False, providers=[dict(type='http', settings=settings)]))
        (self.root/'config.json').write_text(json.dumps(config))
        self.log = (self.root/'broker.log').open('w')
        self.proc = subprocess.Popen([self.binary, '-c', str(self.root/'config.json')], stdout=self.log, stderr=self.log)
        deadline = time.monotonic()+8
        while True:
            assert self.proc.poll() is None, (self.root/'broker.log').read_text()
            try:
                with socket.create_connection(('127.0.0.1', self.port), timeout=.1):
                    break
            except OSError:
                assert time.monotonic() < deadline
                time.sleep(.02)
        return self

    def __exit__(self, kind, *_):
        if kind:
            print((self.root/'broker.log').read_text()[-5000:])
        for client in self.clients:
            client.close()
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait()
        self.log.close()
        self.httpd.shutdown()
        self.httpd.server_close()
        self.directory.cleanup()

    def client(self, name, password='test-password', expected=0):
        client = Client(self.port, 5)
        client.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.clients.append(client)
        code = client.connect(name, password)
        assert (code == 0) == (expected == 0), (name, code, expected)
        return client

    def pair(self):
        reader, writer = self.client('reader'), self.client('writer')
        assert reader.subscribe('fixture/allowed') == 0
        self.transfer(reader, writer)
        return reader, writer

    def payload(self, writer, nonce=0):
        return json.dumps(dict(actor=writer.name, nonce=nonce, data='content '+str(nonce))).encode()

    def transfer(self, reader, writer, count=1):
        samples = []
        for i in range(count):
            data = self.payload(writer, i)
            started = time.perf_counter()
            writer.publish('fixture/allowed', data)
            assert reader.message() == ('fixture/allowed', data)
            samples.append((time.perf_counter()-started)*1000)
        return samples


def summary(label, samples, requests):
    ordered = sorted(samples)
    return dict(scenario=label, messages=len(samples), authorization_requests=requests,
                messages_per_second=round(1000*len(samples)/sum(samples)),
                p50_ms=round(ordered[len(samples)//2], 3),
                p95_ms=round(ordered[int(len(samples)*.95)], 3),
                p99_ms=round(ordered[int(len(samples)*.99)], 3))


def benchmark(binary, count):
    results = []
    with Fixture(binary, cache_ttl_ms=0) as f:
        reader, writer = f.pair()
        before = f.counts['/authorization']
        samples = f.transfer(reader, writer, min(count, 200))
        requests = f.counts['/authorization']-before
        assert requests >= 2*len(samples)
        results.append(summary('uncached healthy', samples, requests))
    with Fixture(binary, version_feed=True) as f:
        reader, writer = f.pair()
        # Allow the initial revision poll to settle, then warm the same keys.
        time.sleep(.15)
        f.transfer(reader, writer)
        before = f.counts['/authorization']
        samples = f.transfer(reader, writer, count)
        assert f.counts['/authorization'] == before, 'cache hits performed HTTP'
        results.append(summary('cached healthy', samples, 0))
        cold = [f.client('cold-'+str(i)) for i in range(12)]
        f.delay = .8
        f.seen.clear()
        with ThreadPoolExecutor(max_workers=12) as pool:
            jobs = [pool.submit(c.subscribe, 'fixture/new-'+str(i)) for i, c in enumerate(cold)]
            assert f.seen.wait(1)
            started = time.monotonic()
            samples = f.transfer(reader, writer, 100)
            # A single MQTT event thread must still progress while HTTP is slow.
            assert time.monotonic()-started < .7, 'slow policy blocked warm MQTT event loop'
            assert all(j.result() >= 128 for j in jobs), 'unknown scopes failed open'
        results.append(summary('cached with slow cold misses', samples, f.counts['/authorization']-before))
        assert f.counts['/authorization']-before <= 4, 'bounded workers/queue/circuit allowed request storm'
        f.mode, f.delay = 'offline', 0
        f.client('unknown-during-outage', expected=1)
        before = f.counts['/authorization']
        results.append(summary('cached during outage', f.transfer(reader, writer, 100), f.counts['/authorization']-before))
    for row in results:
        print('BENCHMARK '+json.dumps(row), flush=True)
    return results


def payload_and_consent(binary):
    with Fixture(binary) as f:
        reader, writer = f.pair()
        before = f.counts['/authorization']
        # Warm grants cannot authorize changed identities, aliases, or duplicates.
        for index, raw in enumerate([b'{"actor":"intruder"}', b'{"actor":"writer","ACTOR":"intruder"}',
                    b'{"actor":"intruder","actor":"writer","ACTOR":"intruder"}',
                    b'{"actor":"writer","scope":"fixture/other"}', b'not JSON']):
            if index:
                writer = f.client('writer')
                f.transfer(reader, writer)
            writer.publish('fixture/allowed', raw)
            reader.no_message()
        assert f.counts['/authorization'] >= before+5
        # CONNECT never reuses another session's successful credential decision.
        f.client('writer', password='wrong', expected=1)
    with Fixture(binary) as f:
        f.consent = False
        reader, writer = f.pair()
        before = f.counts['/authorization']
        f.transfer(reader, writer, 10)
        assert f.counts['/authorization'] >= before+20, 'service did not consent to caching'
    with Fixture(binary, publish_cache_ignored_fields='[]') as f:
        reader, writer = f.pair()
        before = f.counts['/authorization']
        f.transfer(reader, writer, 10)
        assert f.counts['/authorization'] >= before+9, 'default must bind entire payload'
    print('PASS identity/alias/duplicate-field isolation, fresh CONNECT, dual cache consent, exact payload default', flush=True)


def leases(binary):
    with Fixture(binary, cache_ttl_ms=100, cache_max_age_ms=1400) as f:
        f.fresh, f.age = 100, 1400
        reader, writer = f.pair()
        start = time.monotonic()
        f.mode = 'offline'
        time.sleep(.15)
        f.transfer(reader, writer, 100)
        time.sleep(.45)
        f.transfer(reader, writer, 10)
        time.sleep(max(0, 1.5-(time.monotonic()-start)))
        writer.publish('fixture/allowed', f.payload(writer))
        reader.no_message()
        assert f.counts['/authorization'] < 15, 'refresh failures caused a request storm'
    with Fixture(binary, cache_ttl_ms=100) as f:
        f.fresh = 100
        reader, writer = f.pair()
        f.revoked.add('writer')
        time.sleep(.15)
        # First soft-expired hit returns promptly and refreshes in background.
        f.transfer(reader, writer)
        time.sleep(.15)
        writer.publish('fixture/allowed', f.payload(writer))
        reader.no_message()
    with Fixture(binary) as f:
        f.expiry = int(time.time())+2
        reader, writer = f.pair()
        f.mode = 'offline'
        time.sleep(max(0, f.expiry-time.time()+.1))
        writer.publish('fixture/allowed', f.payload(writer))
        reader.no_message()
    print('PASS stale grants, background refresh, explicit deny, hard lease and original session expiry', flush=True)


def revisions(binary):
    with Fixture(binary, version_feed=True) as f:
        reader, writer = f.pair()
        time.sleep(.15)
        f.transfer(reader, writer)
        f.revoked.add('reader')
        f.revision = 'revoke-reader'
        time.sleep(.2)
        writer.publish('fixture/allowed', f.payload(writer))
        reader.no_message()
        f.revoked.clear()
        f.revision = 'restore-reader'
        time.sleep(.2)
        f.transfer(reader, writer)
        cold = f.client('race')
        f.delay = .3
        f.seen.clear()
        with ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(cold.subscribe, 'fixture/race')
            assert f.seen.wait(1)
            f.revoked.add('race')
            f.revision = 'revoke-in-flight'
            assert pending.result() >= 128, 'old in-flight allow restored a revoked grant'
        f.delay = 0
        assert cold.subscribe('fixture/race') >= 128
    # A tiny cache must evict under a large topic working set.
    with Fixture(binary, cache_max_entries=16) as f:
        reader = f.client('reader')
        for i in range(128):
            assert reader.subscribe('fixture/'+str(i)) == 0
        before = f.counts['/authorization']
        assert reader.subscribe('fixture/0') == 0
        assert f.counts['/authorization'] == before+1
    with Fixture(binary, http_queue_bytes=8192) as f:
        reader, writer = f.pair()
        other = f.client('large-cold')
        before = f.counts['/authorization']
        other.publish('fixture/allowed', json.dumps(dict(actor=other.name, data='x'*10000)).encode())
        reader.no_message()
        assert f.counts['/authorization'] == before, 'oversized queued body reached HTTP'
        f.transfer(reader, writer)
    print('PASS revision invalidation, recovery, in-flight revocation race, bounded LRU and queue bytes', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    parser.add_argument('--messages', type=int, default=1000)
    parser.add_argument('--report')
    args = parser.parse_args()
    binary = os.path.abspath(args.broker)
    payload_and_consent(binary)
    leases(binary)
    revisions(binary)
    results = benchmark(binary, args.messages)
    if args.report:
        Path(args.report).write_text(json.dumps(results, indent=2)+'\n')
