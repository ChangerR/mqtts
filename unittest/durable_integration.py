#!/usr/bin/env python3
"""Disposable real broker: persistent QoS 1, restart, expiry and current ACLs."""
import argparse
import json
import socket
import os
from pathlib import Path
import tempfile
import threading
import struct
import subprocess
import time
from http_auth_cache_integration import Fixture
from http_auth_integration import Client, packet, utf

class DurableClient(Client):
    def connect_session(self, name, clean=False, expiry=60, receive=32, owner=None, maximum=None):
        self.name=name
        props=b'\x11'+struct.pack('!I',expiry)+b'\x21'+struct.pack('!H',receive)
        if maximum is not None: props+=b'\x27'+struct.pack('!I',maximum)
        body=utf('MQTT')+bytes([self.version,0xc2 if clean else 0xc0,0,60])
        if self.version==5: body+=bytes([len(props)])+props
        self.send(packet(0x10,body+utf(name)+utf(owner or name)+utf('test-password')))
        head,body=self.read();assert head==0x20
        return body[1],bool(body[0]&1)
    def sub(self, topic, qos=1):
        self.send(packet(0x82,b'\0\1'+(b'\0' if self.version==5 else b'')+utf(topic)+bytes([qos])))
        h,b=self.read();assert h==0x90
        return b[-1]
    def pub(self, topic, data, expiry=None):
        props=b'' if expiry is None else b'\x02'+struct.pack('!I',expiry)
        self.send(packet(0x32,utf(topic)+b'\0\1'+(bytes([len(props)])+props if self.version==5 else b'')+data))
        h,b=self.read();assert h==0x40 and b[:2]==b'\0\1' and (len(b)==2 or b[2]<128),(h,b)
    def delivery(self, ack=True):
        h,b=self.read();assert h>>4==3 and (h>>1)&3==1,(h,b)
        n=int.from_bytes(b[:2],'big');topic=b[2:2+n].decode();pos=n+2;ident=int.from_bytes(b[pos:pos+2],'big');pos+=2
        assert ident
        if self.version==5:
            length=b[pos];assert length<128;pos+=1+length
        if ack: self.ack(ident)
        return topic,b[pos:],ident,bool(h&8)
    def ack(self, ident): self.send(packet(0x40,struct.pack('!H',ident)))
    def disconnect(self, expiry=None):
        body=b'' if expiry is None else b'\0\5\x11'+struct.pack('!I',expiry)
        self.send(packet(0xe0,body))
        # Wait for server closure: reconnecting before DISCONNECT is processed
        # is a takeover and must correctly fence the old expiry update.
        try:
            while self.sock.recv(4096):pass
        finally:self.close()
    def quiet(self):
        self.sock.settimeout(.15)
        try: raise AssertionError(('unexpected packet',self.read()))
        except TimeoutError: pass
        finally:self.sock.settimeout(3)

def client(f,name,version=5,clean=False,expiry=60,receive=32,owner=None,websocket=False,expected=0):
    c=DurableClient(f.port,version,websocket);c.sock.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1);f.clients.append(c)
    code,present=c.connect_session(name,clean,expiry,receive,owner)
    assert (code==0)==(expected==0),(name,code,expected)
    return c,present

def restart(f, before_start=None):
    f.proc.kill();f.proc.wait()
    if before_start: before_start()
    f.proc=subprocess.Popen([f.binary,'-c',str(f.root/'config.json')],stdout=f.log,stderr=f.log,env=f.process_env)
    end=time.monotonic()+8
    while True:
        assert f.proc.poll() is None and time.monotonic()<end
        try:
            with socket.create_connection(('127.0.0.1',f.port),timeout=.1):return
        except OSError:time.sleep(.02)

def run(binary, fault_library):
    for version in (4,5):
        with Fixture(binary,persistence={},server_threads=2,version_feed=True,http_workers=4,http_queue_capacity=64) as f:
            f.wildcards=True
            r,present=client(f,'reader',version);assert not present
            assert r.sub('fixture/#')==1
            r.disconnect();time.sleep(.05)
            w,_=client(f,'writer',5,clean=True,expiry=0)
            expected=[f.payload(w,i) for i in range(40)]
            for data in expected:w.pub('fixture/offline',data)
            # All positive publisher ACKs precede a hard broker crash.
            restart(f)
            r,present=client(f,'reader',version,receive=1);assert present
            first=r.delivery(ack=False);assert first[1]==expected[0]
            if version==5:r.quiet()
            r.close();time.sleep(.05)
            r,present=client(f,'reader',version,receive=1);assert present
            again=r.delivery();assert again[1:3]==first[1:3] and again[3]
            assert [r.delivery()[1] for _ in expected[1:]]==expected[1:]
            r.quiet()
            # Browser transport must enter the same durable path.
            w,_=client(f,'browser',5,clean=True,expiry=0,websocket=True)
            data=f.payload(w,100);w.pub('fixture/browser',data);assert r.delivery()[1]==data
            client(f,'reader',version,owner='different-owner',expected=1)
            # Revoked current READ prevents delivery even for a persisted filter.
            f.revoked.add('reader');f.revision='revoked';time.sleep(.25)
            data=f.payload(w,101);w.pub('fixture/revoked',data);r.quiet()
            f.revoked.clear();f.revision='restored';time.sleep(.35)
            r.quiet()  # Authoritative denial durably removes the revoked delivery.
            data=f.payload(w,104);w.pub('fixture/restored',data);assert r.delivery()[1]==data
            r.quiet()  # Allow asynchronous ACK deletion to commit before takeover.
            # A replacement socket must not be unregistered by the old one.
            for _ in range(8):
                prior=r
                r,present=client(f,'reader',version,receive=1);assert present
                data=f.payload(w,201);w.pub('fixture/takeover',data);assert r.delivery()[1]==data;r.quiet()
                prior.close()
            r.disconnect();time.sleep(.05)
            w.pub('fixture/clean',f.payload(w,102))
            r,present=client(f,'reader',version,clean=True,expiry=60);assert not present;r.quiet()
            if version==5:
                assert r.sub('fixture/#')==1
                w.pub('fixture/expiry',f.payload(w,103),expiry=0);r.quiet()
                r.disconnect(expiry=0);time.sleep(.05)
                r,present=client(f,'reader',version);assert not present
                # Persistent WS is explicitly rejected instead of promising durability.
                client(f,'ws-persistent',websocket=True,expected=1)
            print('PASS MQTT',version,'offline -> crash -> session present -> ACK/DUP/window -> WebSocket -> ACL -> clean/expiry',flush=True)
    with Fixture(binary,persistence={},server_threads=2,http_workers=4,http_queue_capacity=64) as f:
        f.wildcards=True
        readers=[]
        # Router exclusion uses the durable target list. Allocation order must
        # not affect deduplication against the ordinary live-delivery path.
        for name in ['zulu','alpha','tango','bravo','sierra','charlie','romeo','delta']:
            reader,_=client(f,name)
            assert reader.sub('fixture/#')==1
            assert reader.sub('fixture/fanout')==1
            readers.append(reader)
        writer,_=client(f,'writer',clean=True,expiry=0)
        expected=[f.payload(writer,i) for i in range(4)]
        for data in expected:writer.pub('fixture/fanout',data)
        for reader in readers:
            assert [reader.delivery()[1] for _ in expected]==expected,'duplicate or reordered live fanout'
            reader.quiet()
        print('PASS multiple durable subscribers and overlapping filters receive each publication once',flush=True)
    with Fixture(binary,persistence=dict(max_messages=2,max_messages_per_session=2)) as f:
        r,_=client(f,'bounded');assert r.sub('fixture/full')==1;r.disconnect();time.sleep(.05)
        w,_=client(f,'writer',clean=True,expiry=0)
        for i in range(2):w.pub('fixture/full',f.payload(w,i))
        try:w.pub('fixture/full',f.payload(w,3))
        except (EOFError,ConnectionError):pass
        else:raise AssertionError('overflow received a positive PUBACK')
        r,present=client(f,'bounded');assert present
        assert [json.loads(r.delivery()[1])['nonce'] for _ in range(2)]==[0,1]
        print('PASS disk backlog exhaustion closes publisher without positive PUBACK',flush=True)
    if fault_library:
        journal_faults(binary, fault_library)
    with Fixture(binary,persistence=dict(checkpoint_interval_ms=200,segment_bytes=4096),http_workers=4,http_queue_capacity=64) as f:
        f.wildcards=True
        r,_=client(f,'checkpoint-reader',receive=8);assert r.sub('fixture/#')==1;r.disconnect()
        w,_=client(f,'writer',clean=True,expiry=0)
        expected=[f.payload(w,i) for i in range(200)]
        for i,data in enumerate(expected):w.pub('fixture/partition-'+str(i%8),data)
        time.sleep(.4);assert (f.root/'journal'/'CHECKPOINT').exists();restart(f)
        r,present=client(f,'checkpoint-reader',receive=8);assert present
        first=[r.delivery(ack=False) for _ in range(8)]
        assert [m[1] for m in first]==expected[:8]
        for index in [3,5]:r.ack(first[index][2])
        time.sleep(.4);restart(f)
        r,present=client(f,'checkpoint-reader',receive=8);assert present
        replay=r.delivery();assert replay[1:3]==first[0][1:3] and replay[3]
        assert [replay[1]]+[r.delivery()[1] for _ in range(197)]==[data for i,data in enumerate(expected) if i not in [3,5]]
        r.quiet()
        print('PASS checkpoints, cross-partition ordering, segment reclamation and ACK gaps across SIGKILL',flush=True)

def journal_faults(binary, library):
    with tempfile.TemporaryDirectory(prefix='mqtts-fault-control-') as control:
        root=Path(control)
        environment=dict(os.environ,LD_PRELOAD=library,MQTTS_JOURNAL_FAULT_CONTROL=control)
        for mode in ['write','sync']:
            with Fixture(binary,persistence={},process_env=environment) as f:
                r,_=client(f,'disk-fault');assert r.sub('fixture/disk')==1;r.disconnect();time.sleep(.05)
                w,_=client(f,'writer',clean=True,expiry=0)
                (root/mode).write_text('/messages-')
                try:
                    try:w.pub('fixture/disk',f.payload(w,1))
                    except (EOFError,ConnectionError):pass
                    else:raise AssertionError('failed journal write/sync was acknowledged')
                finally:(root/mode).unlink()
                restart(f)
                r,present=client(f,'disk-fault');assert present
                if mode=='write':r.quiet()
                # A completed write with failed sync may replay an unacknowledged
                # publication; MQTT QoS 1 permits this ambiguity.
                print('PASS journal',mode,'failure withholds PUBACK; broker restarts',flush=True)
        with Fixture(binary,persistence={},process_env=environment) as f:
            r,_=client(f,'ack-fault');assert r.sub('fixture/ack')==1
            w,_=client(f,'writer',clean=True,expiry=0)
            w.pub('fixture/ack',f.payload(w,1));first=r.delivery(ack=False)
            (root/'write').write_text('/sessions-')
            r.ack(first[2]);time.sleep(.08)
            (root/'write').unlink();restart(f)
            r,present=client(f,'ack-fault');assert present
            replay=r.delivery();assert replay[1:3]==first[1:3] and replay[3]
            print('PASS failed asynchronous ACK preserves exact packet for replay',flush=True)
        with Fixture(binary,persistence={},process_env=environment,http_workers=4,http_queue_capacity=64) as f:
            def partition(topic):
                value=14695981039346656037
                for c in topic.encode():value=((value^c)*1099511628211)&((1<<64)-1)
                return value%4
            topics=['fixture/slow','fixture/fast']
            while partition(topics[0])==partition(topics[1]):topics[1]+='x'
            slow,_=client(f,'slow');assert slow.sub(topics[0])==1
            fast,_=client(f,'fast');assert fast.sub(topics[1])==1
            a,_=client(f,'a',clean=True,expiry=0);b,_=client(f,'b',clean=True,expiry=0)
            # Warm current authorization before faulting a disk worker.
            a.pub(topics[0],f.payload(a,0));slow.delivery()
            b.pub(topics[1],f.payload(b,0));fast.delivery()
            marker=root/'delay';marker.write_text('/messages-'+str(partition(topics[0]))+'/')
            errors=[]
            def send_slow():
                try:a.pub(topics[0],f.payload(a,1))
                except BaseException as error:errors.append(error)
            blocked=threading.Thread(target=send_slow);blocked.start()
            try:
                time.sleep(.1);assert blocked.is_alive()
                started=time.monotonic();b.pub(topics[1],f.payload(b,1));assert json.loads(fast.delivery()[1])['nonce']==1
                assert time.monotonic()-started<.5,'slow partition blocked independent topic'
                slow.quiet()
            finally:
                marker.unlink();blocked.join(timeout=4)
            assert not blocked.is_alive() and not errors,errors
            assert json.loads(slow.delivery()[1])['nonce']==1
            print('PASS stalled partition cannot acknowledge early; independent partition keeps delivering',flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--broker',required=True);p.add_argument('--fault-library')
    a=p.parse_args();run(a.broker,a.fault_library)
