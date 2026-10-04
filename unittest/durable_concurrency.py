#!/usr/bin/env python3
"""Exercise actual fsync-backed deliveries concurrently with warm/offline ACLs."""
import argparse
import asyncio
import json
import sqlite3
import time
from pathlib import Path
from http_auth_cache_integration import Fixture
from http_auth_concurrency import AsyncClient, Pair, metrics
from http_auth_integration import packet, utf

class PersistentClient(AsyncClient):
    @classmethod
    async def connect(cls, port, name):
        reader,writer=await asyncio.open_connection('127.0.0.1',port)
        item=cls(reader,writer,name)
        props=b'\x11\0\0\0\x3c'
        writer.write(packet(0x10,utf('MQTT')+bytes([5,0xc2,0,60,len(props)])+props+utf(name)+utf(name)+utf('test-password')))
        h,b=await asyncio.wait_for(item.read_packet(),5);assert h==0x20 and b[1]==0
        item.task=asyncio.create_task(item.read_loop());return item

async def load(f,count,messages,size,fan_in=False):
    clients=[];admission=asyncio.Semaphore(8)
    shared=None;routes={}
    if fan_in:
        shared=await PersistentClient.connect(f.port,'collector');clients.append(shared)
        assert await shared.subscribe('fixture/#')==1
    async def prepare(i):
        async with admission:
            r=shared or await PersistentClient.connect(f.port,'reader-'+str(i))
            if not shared:clients.append(r)
            w=await AsyncClient.connect(f.port,'writer-'+str(i));clients.append(w)
            pair=Pair(r,w,i,size)
            if shared:
                routes[pair.topic]=pair
                shared.on_message=lambda topic,data:routes[topic].receive(topic,data)
            else:assert await r.subscribe(pair.topic)==1
            await pair.transfer();return pair
    try:
        pairs=await asyncio.gather(*(prepare(i) for i in range(count)))
        reports=[]
        for label in ('durable healthy','durable cached authorization outage'):
            if 'outage' in label:f.mode='offline'
            before=f.counts['/authorization'];start=time.perf_counter()
            groups=await asyncio.gather(*(p.batch(messages,4) for p in pairs))
            elapsed=time.perf_counter()-start;samples=[s for g in groups for s in g]
            assert all(p.received==p.sequence for p in pairs)
            assert all(c.failure is None for c in clients)
            callbacks=f.counts['/authorization']-before;assert callbacks==0
            row=metrics(label,count,samples,elapsed,callbacks,2,4,size)
            row.update(connections=len(clients),persistent_consumers=1 if fan_in else count,durability='SQLite WAL synchronous FULL',disk_threads=1,io_queue_limit=1024,io_queue_bytes=16777216)
            reports.append(row);print(json.dumps(row),flush=True)
        return reports
    except BaseException as error:
        print('FAILED phase:',repr(error),'broker status',f.proc.poll(),flush=True)
        print('Client failures:',[(c.name,repr(c.failure)) for c in clients if c.failure][:10],flush=True)
        if 'pairs' in locals(): print('Messages received/requested:',sum(p.received for p in pairs),sum(p.sequence for p in pairs),flush=True)
        with sqlite3.connect(f.root/'sessions.db') as snapshot:
            print('Queue count/inflight:',snapshot.execute('SELECT count(*),sum(packet>0) FROM deliveries').fetchone(),flush=True)
        Path('/tmp/mqtts-durable-failure.log').write_text((f.root/'broker.log').read_text())
        raise
    finally:await asyncio.gather(*(c.close() for c in clients),return_exceptions=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--broker',required=True);p.add_argument('--pairs',type=int,default=128);p.add_argument('--messages',type=int,default=64);p.add_argument('--payload-bytes',type=int,default=4096);p.add_argument('--report');p.add_argument('--fan-in',action='store_true')
    a=p.parse_args();assert 1<=a.pairs<=500 and a.messages>0
    with Fixture(a.broker,persistence={},server_threads=2,http_workers=4,http_queue_capacity=64,cache_max_entries=16384,cache_ttl_ms=300000) as f:
        f.fresh=300000
        f.wildcards=a.fan_in
        result=asyncio.run(load(f,a.pairs,a.messages,a.payload_bytes,a.fan_in))
        if a.report:Path(a.report).write_text(json.dumps(result,indent=2)+'\n')
