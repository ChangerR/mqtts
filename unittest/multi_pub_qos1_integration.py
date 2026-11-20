#!/usr/bin/env python3
"""Regression: unlimited-rate QoS1 from multiple publishers must not starve delivery.

The pre-fix process_loop only yielded on EAGAIN, so a handful of publishers
sending as fast as possible could keep every worker thread inside recv and
leave subscribers at recv==0 or a trickle. This test starts a real broker,
one topic, N>=4 publishers with no rate limit (a small C flooder, so the
sockets stay readable), and asserts each subscriber got a meaningful floor
of messages.
"""
from __future__ import print_function

import argparse
import os
import subprocess
import time

from protocol_baseline_integration import (
    Broker, Client, connect, puback, publish_meta, subscribe,
)


TOPIC = 'starve/qos1'
PUBLISHERS = 4
SUBSCRIBERS = 2
THREADS = 2
FLOOD_SECONDS = 4
DRAIN_SECONDS = 1
WINDOW = 100
PAYLOAD = 8
# Starved brokers deliver a trickle (mqttbench 4x4 QoS1 unlimited on 357ed27
# was ~150 recv/s total, ~300/sub in 8s). Fixed brokers deliver tens of
# thousands per subscriber in a few seconds. 1000 sits between those.
MIN_RECV_EACH = 1000


def default_flood_path(broker):
    here = os.path.dirname(os.path.abspath(broker))
    candidates = [
        os.path.join(here, 'unittest', 'multi_pub_qos1_flood'),
        os.path.join(here, 'multi_pub_qos1_flood'),
        os.path.join(os.path.dirname(here), 'unittest', 'multi_pub_qos1_flood'),
    ]
    env = os.environ.get('MQTT_MULTI_PUB_FLOOD')
    if env:
        candidates.insert(0, env)
    for path in candidates:
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


def multi_pub_qos1_no_starvation(binary, flood):
    broker = Broker(binary, THREADS, 60, 10, max_qos=1)
    subscribers = []
    proc = None
    try:
        for index in range(SUBSCRIBERS):
            client = Client(broker.port)
            connect(client, 'starve-sub-%d' % index, 4, 60)
            subscribe(client, TOPIC, 4, qos=1)
            subscribers.append(client)

        proc = subprocess.Popen(
            [flood, '127.0.0.1', str(broker.port), TOPIC,
             str(PUBLISHERS), str(FLOOD_SECONDS), str(WINDOW), str(PAYLOAD)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        counts = [0] * SUBSCRIBERS
        deadline = time.monotonic() + FLOOD_SECONDS + DRAIN_SECONDS
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
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

        try:
            out, err = proc.communicate(timeout=FLOOD_SECONDS + 5)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
            raise AssertionError('flooder timed out: %s %s' % (out, err))
        print('flooder stdout: %s' % out.strip(), flush=True)
        if err.strip():
            print('flooder stderr: %s' % err.strip(), flush=True)
        if proc.returncode != 0:
            raise AssertionError('flooder rc=%s stdout=%s stderr=%s\n%s'
                                 % (proc.returncode, out, err, broker.tail()))

        print('multi_pub_qos1 recv=%s min_each=%s' % (counts, MIN_RECV_EACH), flush=True)
        starved = [i for i, n in enumerate(counts) if n < MIN_RECV_EACH]
        if starved:
            raise AssertionError(
                'subscriber starvation: recv=%s (min %s each), starved=%s\n%s'
                % (counts, MIN_RECV_EACH, starved, broker.tail()))
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            try:
                proc.communicate(timeout=2)
            except Exception:
                pass
        for client in subscribers:
            try:
                client.close()
            except OSError:
                pass
        broker.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    parser.add_argument('--flood', default=None)
    args = parser.parse_args()
    if not os.path.isfile(args.broker):
        raise SystemExit('broker binary not found: %s' % args.broker)
    flood = args.flood or default_flood_path(args.broker)
    if not flood or not os.path.isfile(flood):
        raise SystemExit('flooder binary not found (pass --flood)')
    multi_pub_qos1_no_starvation(args.broker, flood)
    print('PASS mqtt_multi_pub_qos1_integration', flush=True)


if __name__ == '__main__':
    main()
