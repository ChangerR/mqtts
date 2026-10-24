#!/usr/bin/env python3
"""连接生命周期：WS 与 TCP 对等的畸形包、DISCONNECT 关闭、CONNACK 之前不发 DISCONNECT、
接管等待上限、遗嘱延迟截断、WS 分片与大报文。"""
import argparse
import os
import struct
import time

from protocol_baseline_integration import (
    Broker, Client, collect, connect, connect_packet, disconnect_reason, expect_will,
    open_session, packet, publish, subscribe, utf, will_packet)


def ws_frame(payload, opcode, fin):
    mask = os.urandom(4)
    size = len(payload)
    first = (0x80 if fin else 0) | opcode
    if size < 126:
        header = bytes([first, 0x80 | size])
    elif size < 65536:
        header = bytes([first, 0xFE]) + struct.pack('!H', size)
    else:
        header = bytes([first, 0xFF]) + struct.pack('!Q', size)
    return header + mask + bytes(value ^ mask[index % 4] for index, value in enumerate(payload))


def expect_malformed_close(client, version):
    if version >= 5:
        header, body = client.read(2)
        if disconnect_reason(header, body) != 0x81:
            raise AssertionError('want 0x81, got 0x%02x %s' % (header, body.hex()))
    client.expect_closed(2)


def truncated_field(binary, websocket):
    """Remaining Length 完整但字段截断：两种传输都回 0x81 并断开，不能挂起等更多字节。"""
    broker = Broker(binary, 1, 30, 10)
    truncated = bytes.fromhex('8206000100000161')
    try:
        for version in (5, 4):
            client = Client(broker.port, websocket)
            try:
                connect(client, 'trunc-%d' % version, version, 30)
                client.send(truncated)
                expect_malformed_close(client, version)
            finally:
                client.close()
    finally:
        broker.close()


def disconnect_closes(binary):
    """客户端 DISCONNECT 后立刻关闭，不再等保活超时，也不计 keepalive_timeouts。"""
    broker = Broker(binary, 1, 30, 10, monitor=True)
    try:
        for websocket in (False, True):
            client = Client(broker.port, websocket)
            try:
                connect(client, 'disc-%d' % websocket, 5, 2)
                started = time.monotonic()
                client.send(packet(0xE0, b''))
                closed = client.expect_closed(2)
                if closed - started > 1.0:
                    raise AssertionError('closed %.2fs after DISCONNECT' % (closed - started))
            finally:
                client.close()
        # 1.5 x K = 3 s, plus one monitor refresh.
        time.sleep(4.2)
        count = broker.counter('keepalive_timeouts')
        if count != 0:
            raise AssertionError('keepalive_timeouts %s after DISCONNECT\n%s' % (count, broker.tail()))
    finally:
        broker.close()


def silent_before_connack(binary, websocket):
    """MQTT-3.14.0-1：CONNACK 之前的任何故障都直接关闭，3.x 和 5 都不发 DISCONNECT。"""
    broker = Broker(binary, 1, 30, 10, max_packet_size=256)
    cases = (
        ('PINGREQ first', b'\xc0\x00'),
        ('PUBLISH first', packet(0x30, utf('t') + b'\x00x')),
        ('bad RL', b'\x10\x80\x80\x80\x80'),
        ('truncated CONNECT', bytes.fromhex('1003000451')),
        ('oversized v5 CONNECT', connect_packet('c' * 300, 5, 30)),
        ('oversized v4 CONNECT', connect_packet('c' * 300, 4, 30)),
    )
    try:
        for name, data in cases:
            client = Client(broker.port, websocket)
            try:
                client.send(data)
                try:
                    client.expect_closed(2)
                except AssertionError as error:
                    raise AssertionError('%s: %s' % (name, error))
            finally:
                client.close()
    finally:
        broker.close()


def takeover_deadline(binary):
    """旧连接写阻塞时，新连接 3 秒内拿到 CONNACK 0x88；旧连接清理完以后能正常接管。"""
    broker = Broker(binary, 2, 30, 10)
    old = new = publisher = again = None
    try:
        # The owner never reads, so its socket fills and the broker's writes to it block.
        old = Client(broker.port, False, rcvbuf=4096)
        connect(old, 'slow-owner', 5, 30)
        subscribe(old, 'slow/t', 5)

        publisher = Client(broker.port, False)
        connect(publisher, 'slow-pub', 5, 30)
        payload = b'x' * 60000
        for _ in range(200):
            publish(publisher, 'slow/t', payload, 5)
        time.sleep(0.3)

        new = Client(broker.port, False)
        started = time.monotonic()
        new.send(connect_packet('slow-owner', 5, 30))
        header, body = new.read(8)
        elapsed = time.monotonic() - started
        if header != 0x20 or body[1] != 0x88:
            raise AssertionError('CONNACK 0x%02x %s after %.2fs, want 0x88' % (header, body.hex(), elapsed))
        if not 2.5 <= elapsed <= 4.0:
            raise AssertionError('CONNACK 0x88 after %.2fs, want about 3s' % elapsed)
        new.expect_closed(2)

        old.close()
        old = None
        deadline = time.monotonic() + 10
        while True:
            again = Client(broker.port, False)
            again.send(connect_packet('slow-owner', 5, 30))
            header, body = again.read(5)
            if header == 0x20 and body[1] == 0:
                break
            again.close()
            again = None
            if time.monotonic() >= deadline:
                raise AssertionError('Client ID still reserved: CONNACK %s' % body.hex())
    finally:
        for item in (old, new, publisher, again):
            if item is not None:
                item.close()
        broker.close()


def will_delay_clamped(binary):
    """DISCONNECT 带的 Session Expiry 超过服务端上限时按上限算遗嘱延迟。"""
    broker = Broker(binary, 1, 30, 10, persistence={'max_session_expiry_seconds': 3})
    try:
        watcher = Client(broker.port, False)
        try:
            connect(watcher, 'clamp-watch', 5, 30)
            subscribe(watcher, 'clamp/#', 5)
            owner = Client(broker.port, False)
            try:
                open_session(owner, will_packet('clamp-owner', 'clamp/w', b'will', delay=10,
                                                session_expiry=3))
                props = bytes([0x11]) + struct.pack('!I', 60)
                owner.send(packet(0xE0, bytes([0x04, len(props)]) + props))
                owner.expect_closed(2)
            finally:
                owner.close()
            elapsed = expect_will(watcher, 'clamp/w', b'will', timeout=8)
            if not 2.0 <= elapsed <= 5.0:
                raise AssertionError('will after %.2fs, want about 3s' % elapsed)
        finally:
            watcher.close()
    finally:
        broker.close()


def ws_fragments(binary):
    """WS 消息边界对 MQTT 没有意义：分片与单帧行为一致，上限按单个 MQTT 报文算。"""
    broker = Broker(binary, 1, 30, 10, max_packet_size=256)
    try:
        sub = Client(broker.port, False)
        connect(sub, 'frag-sub', 5, 30)
        subscribe(sub, 'f/t', 5)
        pub = packet(0x30, utf('f/t') + b'\x00' + b'a' * 140)

        one = Client(broker.port, True)
        try:
            connect(one, 'frag-one', 5, 30)
            one.sock.sendall(ws_frame(pub + pub, 2, True))
            if len(collect(sub, 5, 0.8)) != 2:
                raise AssertionError('single frame with two packets')
            one.send(b'\xc0\x00')
            if one.read(2)[0] != 0xD0:
                raise AssertionError('single frame client lost')
        finally:
            one.close()

        two = Client(broker.port, True)
        try:
            connect(two, 'frag-two', 5, 30)
            # Fragments that cut packets mid-way, with a PING between them.
            data = pub + pub
            two.sock.sendall(ws_frame(data[:100], 2, False) + ws_frame(b'', 9, True) +
                             ws_frame(data[100:250], 0, False) + ws_frame(data[250:], 0, True))
            if len(collect(sub, 5, 0.8)) != 2:
                raise AssertionError('fragmented message with two packets')
            two.send(b'\xc0\x00')
            if two.read(2)[0] != 0xD0:
                raise AssertionError('fragmented client lost')
        finally:
            two.close()

        big = Client(broker.port, True)
        try:
            connect(big, 'frag-big', 5, 30)
            oversized = packet(0x30, utf('f/t') + b'\x00' + b'b' * 290)
            big.sock.sendall(ws_frame(oversized[:150], 2, False) + ws_frame(oversized[150:], 0, True))
            header, body = big.read(2)
            if disconnect_reason(header, body) != 0x95:
                raise AssertionError('want 0x95, got 0x%02x %s' % (header, body.hex()))
            big.expect_closed(2)
        finally:
            big.close()

        bad = Client(broker.port, True)
        try:
            connect(bad, 'frag-bad', 5, 30)
            bad.sock.sendall(ws_frame(b'\x00', 0, True))
            bad.expect_closed(2)
        finally:
            bad.close()
        sub.close()
    finally:
        broker.close()


def large_packets(binary):
    """大帧和接近上限的报文：WS 不再比 TCP 多攒几份拷贝，进程不能退出。"""
    broker = Broker(binary, 1, 30, 10)
    try:
        junk = Client(broker.port, True)
        try:
            frame = ws_frame(b'\x00' * (2 * 1024 * 1024), 2, True)
            try:
                junk.sock.sendall(frame)
            except OSError:
                pass
            junk.expect_closed(3)
        finally:
            junk.close()
        if broker.proc.poll() is not None:
            raise AssertionError('broker exited on a 2 MiB pre-CONNECT frame\n' + broker.tail())

        # 450KB 要能投递。约 0.9× 上限的 WS 发布不能再把进程打崩；订阅者那边的
        # 发送拷贝仍可能丢消息，那部分不在这里。
        cases = ((False, 450 * 1024, True), (True, 450 * 1024, True), (True, 900 * 1024, False))
        for websocket, payload_size, expect_delivery in cases:
            payload = b'z' * payload_size
            sub = Client(broker.port, False)
            client = Client(broker.port, websocket)
            try:
                topic = 'large/%d' % payload_size
                connect(sub, 'large-sub-%d-%d' % (websocket, payload_size), 5, 30)
                subscribe(sub, topic, 5)
                connect(client, 'large-%d-%d' % (websocket, payload_size), 5, 30)
                publish(client, topic, payload, 5)
                got = collect(sub, 5, 3)
                if expect_delivery and got != [(topic, payload)]:
                    raise AssertionError('large publish ws=%s size=%d: %d messages\n%s' % (
                        websocket, payload_size, len(got), broker.tail()))
                client.send(b'\xc0\x00')
                if client.read(2)[0] != 0xD0:
                    raise AssertionError('large publisher lost ws=%s size=%d' % (websocket, payload_size))
                sub.send(b'\xc0\x00')
                if sub.read(2)[0] != 0xD0:
                    raise AssertionError('subscriber lost after large publish size=%d' % payload_size)
            finally:
                client.close()
                sub.close()
            if broker.proc.poll() is not None:
                raise AssertionError('broker exited ws=%s size=%d\n%s' % (
                    websocket, payload_size, broker.tail()))
    finally:
        broker.close()


def run(binary):
    for websocket in (False, True):
        truncated_field(binary, websocket)
        silent_before_connack(binary, websocket)
    disconnect_closes(binary)
    ws_fragments(binary)
    large_packets(binary)
    takeover_deadline(binary)
    will_delay_clamped(binary)
    print('PASS connection lifecycle', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    args = parser.parse_args()
    run(args.broker)


if __name__ == '__main__':
    main()
