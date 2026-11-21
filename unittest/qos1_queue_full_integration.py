#!/usr/bin/env python3
"""QoS 1/2 must not Success-ACK a publish the live pending queue dropped.

A non-reading subscriber lets send workers block and the 10000-deep pending
queue fill. MQTT 5 publishers then get PUBACK/PUBREC 0x97 (Quota Exceeded) and
keep the connection. MQTT 3.1.1 has no reason code, so the broker withholds
the ACK and the publisher can still PING. MQTT 3.x QoS 2 also releases the
inbound packet-id reservation so a DUP retry is not Success-PUBREC'd.
"""
from __future__ import print_function

import argparse
import os
import struct
import time

from protocol_baseline_integration import (
    Broker, Client, connect, packet, publish_qos, subscribe, utf,
)


TOPIC = 'overflow/qos1'
PAYLOAD = b'x' * 64
PUBLISHES = 20000
ACK_DEADLINE = 25


def ping(client):
    client.send(packet(0xC0, b''))
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        try:
            header, body = client.read(min(0.5, remaining))
        except TimeoutError:
            continue
        if header == 0xD0:
            return
        if header == 0x40:
            continue
        raise AssertionError('PINGRESP 0x%02x %s' % (header, body.hex()))
    raise AssertionError('no PINGRESP')


def flood(publisher, version, start=1, count=PUBLISHES):
    for packet_id in range(start, start + count):
        publish_qos(publisher, TOPIC, PAYLOAD, version, 1, packet_id)


def publish_qos2(client, version, packet_id, dup=False):
    body = utf(TOPIC) + struct.pack('!H', packet_id)
    if version >= 5:
        body += b'\x00'
    body += PAYLOAD
    flags = 0x30 | (2 << 1)
    if dup:
        flags |= 0x08
    client.send(packet(flags, body))


def read_one(client, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            return client.read(min(0.5, remaining))
        except TimeoutError:
            continue
    return None, None


def collect_pubacks(publisher, version, deadline):
    success = 0
    quota = 0
    last = time.monotonic()
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            header, body = publisher.read(min(0.5, remaining))
        except TimeoutError:
            # MQTT 5 must wait for a QuotaExceeded. MQTT 3.x stops after silence.
            if version < 5 and success > 0 and time.monotonic() - last > 2:
                break
            continue
        last = time.monotonic()
        if header != 0x40:
            raise AssertionError('expected PUBACK, got 0x%02x %s' % (header, body.hex()))
        if version >= 5:
            if len(body) < 3:
                raise AssertionError('MQTT 5 PUBACK too short: %s' % body.hex())
            reason = body[2]
            if reason == 0:
                success += 1
            elif reason == 0x97:
                quota += 1
                break
            else:
                raise AssertionError('PUBACK reason 0x%02x %s' % (reason, body.hex()))
        else:
            success += 1
    return success, quota


def mqtt5_quota_nack(binary, websocket):
    broker = Broker(binary, 1, 60, 10, max_qos=2)
    subscriber = None
    publisher = None
    qos2_pub = None
    try:
        suffix = 'ws' if websocket else 'tcp'
        subscriber = Client(broker.port, websocket, rcvbuf=256)
        connect(subscriber, 'overflow-sub-v5-%s' % suffix, 5, 60)
        subscribe(subscriber, TOPIC, 5, qos=1)

        publisher = Client(broker.port, websocket)
        connect(publisher, 'overflow-pub-v5-%s' % suffix, 5, 60)
        flood(publisher, 5)
        success, quota = collect_pubacks(publisher, 5, time.monotonic() + ACK_DEADLINE)
        print('mqtt5 websocket=%s success_puback=%s quota_puback=%s' % (websocket, success, quota),
              flush=True)
        if quota < 1:
            raise AssertionError(
                'MQTT 5 QoS1 overflow never returned QuotaExceeded PUBACK '
                '(success=%s quota=%s sent=%s)\n%s' % (success, quota, PUBLISHES, broker.tail()))
        ping(publisher)
        flood(publisher, 5, PUBLISHES + 1, PUBLISHES)

        qos2_pub = Client(broker.port, websocket)
        connect(qos2_pub, 'overflow-qos2-v5-%s' % suffix, 5, 60)
        publish_qos2(qos2_pub, 5, 1)
        header, body = read_one(qos2_pub, 5)
        if header != 0x50 or len(body) < 3 or body[2] != 0x97:
            raise AssertionError(
                'MQTT 5 QoS2 overflow want PUBREC 0x97, got 0x%s %s\n%s' % (
                    header if header is None else '%02x' % header, body.hex() if body else '',
                    broker.tail()))
        ping(qos2_pub)
    finally:
        if qos2_pub is not None:
            qos2_pub.close()
        if publisher is not None:
            publisher.close()
        if subscriber is not None:
            subscriber.close()
        broker.close()
    print('PASS mqtt5 websocket=%s QoS1/2 queue-full returns 0x97 and keeps the connection' % websocket,
          flush=True)


def mqtt311_withhold(binary):
    broker = Broker(binary, 1, 60, 10, max_qos=2)
    subscriber = None
    publisher = None
    qos2_pub = None
    try:
        subscriber = Client(broker.port, rcvbuf=256)
        connect(subscriber, 'overflow-sub-v4', 4, 60)
        subscribe(subscriber, TOPIC, 4, qos=1)

        publisher = Client(broker.port)
        connect(publisher, 'overflow-pub-v4', 4, 60)
        flood(publisher, 4)
        success, _quota = collect_pubacks(publisher, 4, time.monotonic() + ACK_DEADLINE)
        print('mqtt311 success_puback=%s sent=%s' % (success, PUBLISHES), flush=True)
        if success >= PUBLISHES:
            raise AssertionError(
                'MQTT 3.1.1 Success-PUBACK\'d every overflow publish (%s/%s); '
                'expected withheld ACKs\n%s' % (success, PUBLISHES, broker.tail()))
        ping(publisher)
        flood(publisher, 4, PUBLISHES + 1, PUBLISHES)

        qos2_pub = Client(broker.port)
        connect(qos2_pub, 'overflow-qos2-v4', 4, 60)
        publish_qos2(qos2_pub, 4, 1)
        header, body = read_one(qos2_pub, 2)
        if header is not None:
            raise AssertionError(
                'MQTT 3.1.1 QoS2 overflow should withhold PUBREC, got 0x%02x %s\n%s' % (
                    header, body.hex(), broker.tail()))
        publish_qos2(qos2_pub, 4, 1, dup=True)
        header, body = read_one(qos2_pub, 2)
        if header is not None:
            raise AssertionError(
                'MQTT 3.1.1 QoS2 DUP retry must not Success-PUBREC after withhold, '
                'got 0x%02x %s\n%s' % (header, body.hex(), broker.tail()))
        ping(qos2_pub)
    finally:
        if qos2_pub is not None:
            qos2_pub.close()
        if publisher is not None:
            publisher.close()
        if subscriber is not None:
            subscriber.close()
        broker.close()
    print('PASS mqtt311 QoS1 withhold + QoS2 DUP does not Success-PUBREC', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    args = parser.parse_args()
    if not os.path.isfile(args.broker):
        raise SystemExit('broker binary not found: %s' % args.broker)
    mqtt5_quota_nack(args.broker, False)
    mqtt5_quota_nack(args.broker, True)
    mqtt311_withhold(args.broker)
    print('PASS mqtt_qos1_queue_full_integration', flush=True)


if __name__ == '__main__':
    main()
