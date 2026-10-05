#!/usr/bin/env python3
"""Regression cases for persistent-session review findings, using real sockets."""
import argparse
import json
import struct
import zlib
import os
from pathlib import Path
import tempfile
import time
from durable_integration import DurableClient, client, restart
from http_auth_cache_integration import Fixture
from http_auth_integration import packet, utf


def eventually(check, timeout=4):
    deadline = time.monotonic() + timeout
    while not check():
        assert time.monotonic() < deadline, 'condition did not complete'
        time.sleep(.02)


def maintenance(binary, fault):
    with tempfile.TemporaryDirectory(prefix='mqtts-review-fault-') as directory:
        control = Path(directory)
        environment = dict(os.environ, LD_PRELOAD=fault, MQTTS_JOURNAL_FAULT_CONTROL=directory)
        with Fixture(binary, persistence=dict(checkpoint_interval_ms=200), process_env=environment) as f:
            reader, _ = client(f, 'reader')
            assert reader.sub('fixture/review') == 1
            writer, _ = client(f, 'writer', clean=True, expiry=0)
            writer.pub('fixture/review', f.payload(writer, 0))
            reader.delivery()
            (control/'write').write_text('/CHECKPOINT.tmp')
            try:
                eventually(lambda: 'maintenance will retry' in (f.root/'broker.log').read_text())
                client(f, 'clean-during-checkpoint-error', clean=True, expiry=0)
                for nonce in range(1, 5):
                    data = f.payload(writer, nonce)
                    writer.pub('fixture/review', data)
                    assert reader.delivery()[1] == data
            finally:
                (control/'write').unlink()
            eventually(lambda: (f.root/'journal'/'CHECKPOINT').exists(), timeout=5)
            # Freeze only the checkpoint file's fsync, never an append writer.
            (control/'checkpoint_delay').write_text('/CHECKPOINT.tmp')
            try:
                eventually(lambda: (control/'checkpoint-entered').exists())
                until = time.monotonic() + .8
                nonce = 5
                while time.monotonic() < until:
                    data = f.payload(writer, nonce)
                    writer.pub('fixture/review', data)
                    assert reader.delivery()[1] == data
                    nonce += 1
                    time.sleep(.01)
                client(f, 'clean-during-checkpoint-delay', clean=True, expiry=0)
                # A post-snapshot publication must survive checkpoint install
                # and reclamation even when the reader has not ACKed it.
                data = f.payload(writer, nonce)
                writer.pub('fixture/review', data)
                first = reader.delivery(ack=False)
            finally:
                (control/'checkpoint_delay').unlink()
            time.sleep(.35)
            restart(f)
            reader, present = client(f, 'reader')
            assert present
            replay = reader.delivery()
            assert replay[1:3] == first[1:3]
            reader.quiet()
            print('PASS checkpoint failure retry, >500ms fsync isolation, and concurrent-append crash recovery', flush=True)
        with Fixture(binary, persistence={}, process_env=environment) as f:
            reader, _ = client(f, 'reader')
            assert reader.sub('fixture/failure') == 1
            writer, _ = client(f, 'writer', clean=True, expiry=0)
            (control/'write').write_text('/messages-')
            try:
                try:
                    writer.pub('fixture/failure', f.payload(writer, 0))
                except (EOFError, ConnectionError):
                    pass
                else:
                    raise AssertionError('failed append received success')
                client(f, 'clean-after-journal-failure', clean=True, expiry=0)
                client(f, 'durable-after-journal-failure', expected=1)
            finally:
                (control/'write').unlink()
            print('PASS terminal append failure rejects durable traffic without rejecting new clean clients', flush=True)


def qos_zero_takeover(binary):
    for version in (4, 5):
        with Fixture(binary, persistence={}, server_threads=2, http_workers=4, http_queue_capacity=64) as f:
            reader, _ = client(f, 'takeover-reader', version=version)
            assert reader.sub('fixture/takeover', qos=0) == 0
            writer, _ = client(f, 'writer', clean=True, expiry=0)
            for nonce in range(16):
                previous = reader
                reader, present = client(f, 'takeover-reader', version=version)
                assert present
                previous.close()
                time.sleep(.03)
                data = f.payload(writer, nonce)
                writer.pub('fixture/takeover', data)
                assert reader.message() == ('fixture/takeover', data)
    print('PASS MQTT 3/5 QoS 0 subscriptions survive repeated cross-thread takeover', flush=True)


def poison_messages(binary):
    with Fixture(binary, persistence={}, cache_ttl_ms=0, http_workers=4, http_queue_capacity=64) as f:
        f.wildcards = True
        reader, _ = client(f, 'reader')
        assert reader.sub('fixture/#') == 1
        reader.disconnect()
        writer, _ = client(f, 'writer', clean=True, expiry=0)
        f.denied_deliveries.add(('reader', 'fixture/denied'))
        writer.pub('fixture/denied', f.payload(writer, 0))
        writer.pub('fixture/large', json.dumps(dict(actor='writer', data='x'*1024)).encode())
        good = f.payload(writer, 2)
        writer.pub('fixture/good', good)
        reader = DurableClient(f.port, 5); f.clients.append(reader)
        assert reader.connect_session('reader', maximum=256, receive=1) == (0, True)
        assert reader.delivery()[1] == good
        reader.quiet()
        log = (f.root/'broker.log').read_text()
        assert 'reason not_authorized' in log and 'reason packet_too_large' in log
        restart(f)
        reader, present = client(f, 'reader'); assert present
        reader.quiet()  # Discard decisions survived a crash and did not loop.
    with Fixture(binary, persistence={}) as f:
        f.wildcards = True
        writer, _ = client(f, 'writer', clean=True, expiry=0)
        writer.pub('fixture/outage', f.payload(writer, 0))  # Warm only the writer's grant.
        reader, _ = client(f, 'reader')
        assert reader.sub('fixture/#') == 1
        f.mode = 'offline'
        good = f.payload(writer, 1)
        writer.pub('fixture/outage', good)
        reader.quiet()
        f.mode = 'normal'
        assert reader.delivery()[1] == good
        assert 'reason not_authorized' not in (f.root/'broker.log').read_text()
    # Empty topics and aliases must be rejected equally on TCP and WebSocket.
    for websocket in (False, True):
        with Fixture(binary, persistence={}) as f:
            reader, _ = client(f, 'reader'); assert reader.sub('fixture/invalid') == 1
            writer, _ = client(f, 'writer', clean=True, expiry=0, websocket=websocket)
            props = b'\x23\x00\x01'
            writer.send(packet(0x32, utf('fixture/invalid') + b'\0\1' + bytes([len(props)]) + props + f.payload(writer, 0)))
            try: result = writer.read()
            except (EOFError, ConnectionError): pass
            else: assert result[0] == 0xe0, result
            reader.quiet()
    print('PASS permanent deny and oversize discard, transient outage retry, and alias rejection', flush=True)


def replace_first_stored_wire(f, replacement):
    # Emulate a legacy import containing invalid/large wire data. Recompute frame
    # checksums deliberately; physical corruption must still fail closed.
    assert not (f.root/'journal'/'CHECKPOINT').exists()
    replaced = False
    for path in sorted((f.root/'journal').glob('messages-*/*.log')):
        source = path.read_bytes(); output = bytearray(); offset = 0
        while offset < len(source):
            magic, size, serial, _, _ = struct.unpack_from('<IIQII', source, offset)
            data = source[offset+24:offset+24+size]
            if not replaced:
                assert data[0] == 8
                wire_size = struct.unpack_from('<I', data, 17)[0]
                wire = replacement(data[21:21+wire_size])
                data = data[:17] + struct.pack('<I', len(wire)) + wire + data[21+wire_size:]
                replaced = True
            header = struct.pack('<IIQI', magic, len(data), serial, zlib.crc32(data))
            output += header + struct.pack('<I', zlib.crc32(header)) + data
            offset += 24 + size
        path.write_bytes(output)
    assert replaced


def stored_wire_budget(binary):
    for malformed in (True, False):
        with Fixture(binary, persistence={}) as f:
            reader, _ = client(f, 'reader'); assert reader.sub('fixture/record') == 1
            reader.disconnect()
            writer, _ = client(f, 'writer', clean=True, expiry=0)
            writer.pub('fixture/record', f.payload(writer, 0))
            next_data = f.payload(writer, 1)
            writer.pub('fixture/record', next_data)
            large = b'x' * (1024 * 1024 + 100)
            replacement = (lambda wire: b'\x20' + wire[1:]) if malformed else (
                lambda _: packet(0x32, utf('fixture/record') + b'\0\1\0' + large))
            restart(f, lambda: replace_first_stored_wire(f, replacement))
            reader, present = client(f, 'reader'); assert present
            if not malformed: assert reader.delivery()[1] == large
            assert reader.delivery()[1] == next_data
            reader.quiet()
            if malformed: assert 'reason malformed' in (f.root/'broker.log').read_text()
    print('PASS malformed stored record isolation and replay larger than the 1 MiB client pool', flush=True)


def authorization_failures(binary):
    for version in (4, 5):
        with Fixture(binary, persistence={}, cache_ttl_ms=0) as f:
            f.mode = 'offline'
            cold = DurableClient(f.port, version); f.clients.append(cold)
            code, _ = cold.connect_session('cold-unavailable', clean=True, expiry=0)
            assert code == (3 if version == 4 else 0x88), code
            f.mode = 'normal'
            f.revoked.add('denied')
            cold = DurableClient(f.port, version); f.clients.append(cold)
            code, _ = cold.connect_session('denied', clean=True, expiry=0)
            assert code == (5 if version == 4 else 0x87), code
    print('PASS unavailable authentication and authoritative denial use distinct MQTT reason codes', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    parser.add_argument('--fault-library', required=True)
    args = parser.parse_args()
    maintenance(args.broker, args.fault_library)
    authorization_failures(args.broker)
    qos_zero_takeover(args.broker)
    poison_messages(args.broker)
    stored_wire_budget(args.broker)
