#!/usr/bin/env python3
"""Real MQTT + C++ gRPC client + independently running Go authorization module."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import subprocess
import tempfile
import time
import urllib.request

from http_auth_concurrency import AsyncClient, Pair, metrics


async def load(port, authz, metrics_url, count, messages, size):
    clients, pairs, reports = [], [], []
    admission = asyncio.Semaphore(8)
    async def prepare(index):
        async with admission:
            reader = await AsyncClient.connect(port, 'reader-'+str(index)); clients.append(reader)
            writer = await AsyncClient.connect(port, 'writer-'+str(index)); clients.append(writer)
            pair = Pair(reader, writer, index, size)
            assert await reader.subscribe(pair.topic) == 1
            await pair.transfer()
            return pair
    def stats():
        with urllib.request.urlopen(metrics_url, timeout=2) as response:
            return json.load(response)
    try:
        pairs = await asyncio.gather(*(prepare(i) for i in range(count)))
        initial = stats()
        for offline in [False, True]:
            if offline:
                os.kill(authz.pid, signal.SIGSTOP)
            before = stats() if not offline else initial
            start = time.perf_counter()
            groups = await asyncio.gather(*(pair.batch(messages, 4) for pair in pairs))
            elapsed = time.perf_counter()-start
            samples = [sample for group in groups for sample in group]
            assert all(pair.received == pair.sequence for pair in pairs)
            assert all(client.failure is None for client in clients)
            if offline:
                os.kill(authz.pid, signal.SIGCONT)
                await asyncio.sleep(.1)
            after = stats()
            assert before['items'] == after['items'], 'fresh cache hit reached RPC authorization'
            report = metrics('gRPC authz paused' if offline else 'gRPC healthy', count, samples, elapsed,
                             after['batches']-before['batches'], 2, 4, size)
            reports.append(report)
            print(json.dumps(report), flush=True)
        print(json.dumps(dict(cold_rpc_batches=initial['batches'], cold_decisions=initial['items'], largest_batch=initial['max_batch'])), flush=True)
        return reports
    finally:
        os.kill(authz.pid, signal.SIGCONT)
        await asyncio.gather(*(client.close() for client in clients), return_exceptions=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    parser.add_argument('--authz-fixture', required=True)
    parser.add_argument('--pairs', type=int, default=32)
    parser.add_argument('--messages', type=int, default=64)
    parser.add_argument('--payload-bytes', type=int, default=4096)
    parser.add_argument('--report')
    args = parser.parse_args()
    assert 1 <= args.pairs <= 512 and args.messages > 0 and 1 <= args.payload_bytes <= 65536
    children = []
    with tempfile.TemporaryDirectory(prefix='mqtts-grpc-') as directory:
        root = Path(directory)
        env = dict(os.environ, AUTHZ_QUERY_TOKEN=secrets.token_hex(32), AUTHZ_ADMIN_TOKEN=secrets.token_hex(32))
        with (root/'runtime.log').open('w') as log:
            try:
                authz = subprocess.Popen([args.authz_fixture, '--directory', directory, '--pairs', str(args.pairs)], env=env, stdout=log, stderr=log)
                children.append(authz)
                deadline = time.monotonic()+10
                while not (root/'ports.json').exists():
                    assert authz.poll() is None and time.monotonic() < deadline
                    time.sleep(.02)
                ports = json.loads((root/'ports.json').read_text())
                with socket.socket() as probe:
                    probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
                settings = dict(endpoint=ports['rpc'], insecure=True, token_env='AUTHZ_QUERY_TOKEN',
                                timeout_ms=500, rpc_workers=4, rpc_queue_capacity=256,
                                cache_ttl_ms=60000, cache_max_age_ms=300000, cache_max_entries=16384,
                                publish_payload='bytes', publish_cache_ignored_fields='["nonce","data"]')
                config = dict(server=dict(bind_address='127.0.0.1', port=port, thread_count=2, max_connections=2048),
                              monitoring=dict(enabled=False), log=dict(level='warn'),
                              auth=dict(enabled=True, allow_anonymous=False, providers=[dict(type='grpc', settings=settings)]))
                (root/'broker.json').write_text(json.dumps(config))
                broker = subprocess.Popen([args.broker, '-c', str(root/'broker.json')], env=env, stdout=log, stderr=log)
                children.append(broker)
                while True:
                    assert broker.poll() is None and time.monotonic() < deadline
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=.1): break
                    except OSError: time.sleep(.02)
                result = asyncio.run(load(port, authz, 'http://'+ports['metrics'], args.pairs, args.messages, args.payload_bytes))
                if args.report: Path(args.report).write_text(json.dumps(result, indent=2)+'\n')
            except BaseException:
                print((root/'runtime.log').read_text()[-4000:])
                raise
            finally:
                for child in reversed(children):
                    child.send_signal(signal.SIGCONT); child.terminate()
                    try: child.wait(timeout=5)
                    except subprocess.TimeoutExpired: child.kill(); child.wait()


if __name__ == '__main__':
    main()
