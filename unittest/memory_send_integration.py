#!/usr/bin/env python3
"""阶段 0 评审里内存与收发的回归：接近上限的报文、每连接预算、出站窗口、遗嘱延迟截断、零值流控属性。"""
import argparse
import json
import struct
import time

from protocol_baseline_integration import (
    Broker, Client, connect, connect_packet, disconnect_reason, flow_properties, open_session,
    packet, publish_alias, publish_meta, publish_qos, puback, quiet, session_packet, subscribe,
    will_packet, expect_will)

MAX_PACKET = 1048576


def expect_ack(client, packet_id, timeout=5):
    header, body = client.read(timeout)
    if header != 0x40 or int.from_bytes(body[:2], 'big') != packet_id:
        raise AssertionError('PUBACK 0x%02x %s' % (header, body[:8].hex()))
    if len(body) > 2 and body[2] != 0:
        raise AssertionError('PUBACK reason 0x%02x' % body[2])


def expect_alive(broker):
    if broker.proc.poll() is not None:
        raise AssertionError('broker exited with %s\n%s' % (broker.proc.returncode, broker.tail()))
    probe = Client(broker.port)
    try:
        connect(probe, 'alive-probe', 5, 30)
    finally:
        probe.close()


def near_limit_publish(binary):
    broker = Broker(binary, 1, 30, 10, max_packet_size=MAX_PACKET)
    try:
        subscriber = Client(broker.port)
        publisher = Client(broker.port)
        try:
            connect(subscriber, 'near-sub', 5, 60)
            subscribe(subscriber, 'near/t', 5, qos=1)
            # 订阅者自己先发一条大报文，读缓冲涨到接近上限，再给它投递大消息。
            publish_qos(subscriber, 'near/other', b'g' * int(MAX_PACKET * 0.9), 5, 1, 7)
            expect_ack(subscriber, 7)
            connect(publisher, 'near-pub', 5, 60)
            for index, fraction in enumerate((0.6, 0.9)):
                payload = bytes([0x41 + index]) * int(MAX_PACKET * fraction)
                publish_qos(publisher, 'near/t', payload, 5, 1, index + 1)
                expect_ack(publisher, index + 1)
                header, body = subscriber.read(10)
                topic, packet_id, got = publish_meta(header, body, 5)
                if topic != 'near/t' or got != payload:
                    raise AssertionError('delivery %s: %s %d bytes' % (fraction, topic, len(got)))
                puback(subscriber, packet_id)
            expect_alive(broker)
        finally:
            subscriber.close()
            publisher.close()
    finally:
        broker.close()


def websocket_large_publish_survives(binary):
    broker = Broker(binary, 1, 30, 10, max_packet_size=MAX_PACKET)
    try:
        client = Client(broker.port, True)
        try:
            connect(client, 'ws-near', 5, 60)
            publish_qos(client, 'ws/near', b'w' * int(MAX_PACKET * 0.9), 5, 1, 1)
            try:
                client.read(5)
            except (EOFError, ConnectionError, OSError, TimeoutError):
                pass
        finally:
            client.close()
        time.sleep(0.3)
        expect_alive(broker)
    finally:
        broker.close()


def connection_budget_quota(binary):
    broker = Broker(binary, 1, 30, 10, max_packet_size=MAX_PACKET, topic_alias_maximum=65535)
    try:
        client = Client(broker.port)
        try:
            connect(client, 'alias-hog', 5, 60)
            reason = None
            for alias in range(1, 200):
                topic = 'hog/%d/' % alias + 'x' * 60000
                try:
                    publish_alias(client, topic, b'p', alias, qos=1, packet_id=alias)
                    header, body = client.read(5)
                except (EOFError, ConnectionError, OSError) as error:
                    raise AssertionError('closed without DISCONNECT after %d aliases: %r'
                                         % (alias, error))
                if header == 0x40:
                    continue
                reason = disconnect_reason(header, body)
                break
            if reason != 0x97:
                raise AssertionError('budget exhaustion reason %s' % reason)
            client.expect_closed(2)
        finally:
            client.close()
        expect_alive(broker)
    finally:
        broker.close()


def outbound_window_hold(binary, websocket):
    broker = Broker(binary, 1, 60, 10, monitor=True)
    try:
        subscriber = Client(broker.port, websocket)
        publisher = Client(broker.port, websocket)
        try:
            connect(subscriber, 'hold-sub', 5, 60, flow_properties(receive_maximum=1))
            subscribe(subscriber, 'hold/t', 5, qos=1)
            connect(publisher, 'hold-pub', 5, 60)
            for index in range(3):
                publish_qos(publisher, 'hold/t', bytes([index]), 5, 1, index + 1)
                expect_ack(publisher, index + 1)
            header, body = subscriber.read(3)
            _topic, packet_id, payload = publish_meta(header, body, 5)
            received = [payload]
            # 扣住 PUBACK 超过发送任务的 5 秒过期时间，排在后面的消息不能被丢。
            time.sleep(6)
            puback(subscriber, packet_id)
            for _ in range(2):
                header, body = subscriber.read(3)
                _topic, packet_id, payload = publish_meta(header, body, 5)
                received.append(payload)
                puback(subscriber, packet_id)
            if received != [bytes([0]), bytes([1]), bytes([2])]:
                raise AssertionError('websocket=%s received %s' % (websocket, received))
            # 第 2、3 条各被窗口挡住一次，5 ms 一次的重试不再计数。
            broker.wait_counter('inflight_window_delays', 2)
            time.sleep(1.3)
            delays = broker.counter('inflight_window_delays')
            if delays != 2:
                raise AssertionError('websocket=%s inflight_window_delays=%s, want 2'
                                     % (websocket, delays))
        finally:
            subscriber.close()
            publisher.close()
    finally:
        broker.close()


def will_delay_capped_by_disconnect_expiry(binary):
    broker = Broker(binary, 1, 30, 10, persistence=True)
    try:
        config_path = broker.root / 'config.json'
        config = json.loads(config_path.read_text())
        config['persistence']['max_session_expiry_seconds'] = 3
        config_path.write_text(json.dumps(config))
        broker.restart()
        watcher = Client(broker.port)
        will = Client(broker.port)
        try:
            open_session(watcher, session_packet('clamp-watch', True, 0))
            subscribe(watcher, 'clamp/will', 5)
            open_session(will, will_packet('clamp-will', 'clamp/will', b'gone', delay=10,
                                           session_expiry=3))
            props = bytes([0x11]) + struct.pack('!I', 60)
            will.send(packet(0xE0, bytes([0x04, len(props)]) + props))
            elapsed = expect_will(watcher, 'clamp/will', b'gone', timeout=9)
            if not 2.0 <= elapsed <= 6.0:
                raise AssertionError('will after %.2fs, want about 3s' % elapsed)
        finally:
            watcher.close()
            will.close()
    finally:
        broker.close()


def zero_flow_properties(binary, websocket):
    broker = Broker(binary, 1, 30, 10)
    try:
        cases = (('rm-zero', bytes([0x21, 0, 0])), ('mps-zero', bytes([0x27, 0, 0, 0, 0])))
        for client_id, properties in cases:
            client = Client(broker.port, websocket)
            try:
                client.send(connect_packet(client_id, 5, 30, properties))
                header, body = client.read(3)
                if header != 0x20 or len(body) < 2 or body[1] != 0x82:
                    raise AssertionError('%s websocket=%s CONNACK 0x%02x %s, want 0x82'
                                         % (client_id, websocket, header, body.hex()))
                client.expect_closed(2)
            finally:
                client.close()
        expect_alive(broker)
    finally:
        broker.close()


def run(binary):
    near_limit_publish(binary)
    websocket_large_publish_survives(binary)
    connection_budget_quota(binary)
    print('PASS near-limit packets and connection budget', flush=True)
    for websocket in (False, True):
        outbound_window_hold(binary, websocket)
        zero_flow_properties(binary, websocket)
    print('PASS outbound window hold and zero flow properties', flush=True)
    will_delay_capped_by_disconnect_expiry(binary)
    print('PASS will delay capped by DISCONNECT session expiry', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    args = parser.parse_args()
    run(args.broker)


if __name__ == '__main__':
    main()
