#!/usr/bin/env python3
"""Regression cases for persistent-session review findings, using real sockets."""
import argparse
import os
from pathlib import Path
import tempfile
import time
from durable_integration import client, restart
from http_auth_cache_integration import Fixture


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


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    parser.add_argument('--fault-library', required=True)
    args = parser.parse_args()
    maintenance(args.broker, args.fault_library)
