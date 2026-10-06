#!/usr/bin/env python3
"""Bounded load generator; receipts, real delivery and scheduler lag are separate."""
import argparse
import asyncio
import json
import os
import sys
import subprocess
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from durable_concurrency import PersistentClient
from http_auth_concurrency import AsyncClient, Pair
from http_auth_cache_integration import Fixture

class TimedPair(Pair):
    def __init__(self, *args):
        super().__init__(*args)
        self.acks = []

    async def transfer(self):
        self.sequence += 1
        seq = self.sequence
        data = json.dumps(dict(actor=self.writer.name, nonce=seq, data='x'*self.size), separators=(',', ':')).encode()
        future = asyncio.get_running_loop().create_future()
        self.waiters[seq] = future
        start = time.perf_counter()
        try:
            reason = await self.writer.publish(self.topic, data)
            ack = time.perf_counter()
            assert reason < 128
            received = await asyncio.wait_for(future, 5)
            self.acks.append((ack-start)*1000)
            return (received-start)*1000
        finally:
            self.waiters.pop(seq, None)

def distribution(values):
    values = sorted(values)
    return {key: round(values[min(len(values)-1, int(len(values)*fraction))], 3) for key, fraction in [('p50',.5),('p95',.95),('p99',.99),('max',1)]}

def cpu_snapshot(pid):
    result = {}
    for path in Path('/proc',str(pid),'task').glob('*/stat'):
        try:
            data = path.read_text().rsplit(')',1)[1].split()
            result[path.parent.name] = int(data[11])+int(data[12])
        except FileNotFoundError:
            pass
    return result

async def measure(fixture, pairs, clients, label, rate=None, duration=6, batch_messages=64, window=4):
    for pair in pairs:
        pair.acks.clear()
    samples = []
    lag = []
    schedule_lag = []
    running = True
    async def monitor():
        while running:
            start = time.perf_counter()
            await asyncio.sleep(.01)
            lag.append(max(0,(time.perf_counter()-start-.01)*1000))
    task = asyncio.create_task(monitor())
    before_calls = fixture.counts['/authorization']
    before_cpu = cpu_snapshot(fixture.proc.pid)
    started = time.perf_counter()
    peak = 0
    if rate is None:
        groups = await asyncio.gather(*(pair.batch(batch_messages,window) for pair in pairs))
        samples = [value for group in groups for value in group]
    else:
        gates = [asyncio.Semaphore(window) for _ in pairs]
        active = set()
        async def send(index, scheduled):
            async with gates[index]:
                schedule_lag.append(max(0,(time.perf_counter()-scheduled)*1000))
                samples.append(await pairs[index].transfer())
        limit=asyncio.Semaphore(len(pairs)*window)
        failures=[]
        def finished(item):
            active.discard(item)
            limit.release()
            if not item.cancelled() and item.exception() is not None:failures.append(item.exception())
        for i in range(int(rate*duration)):
            scheduled = started+i/rate
            remaining = scheduled-time.perf_counter()
            if remaining>0:
                await asyncio.sleep(remaining)
            await limit.acquire()
            if failures:raise failures[0]
            item=asyncio.create_task(send(i%len(pairs),scheduled))
            active.add(item)
            item.add_done_callback(finished)
            peak=max(peak,len(active))
        await asyncio.gather(*active)
        if failures:raise failures[0]
    elapsed = time.perf_counter()-started
    after_cpu = cpu_snapshot(fixture.proc.pid)
    running = False
    await task
    assert all(c.failure is None for c in clients)
    assert all(p.received == p.sequence for p in pairs)
    ticks = os.sysconf(os.sysconf_names['SC_CLK_TCK'])
    cpu = sorted([(after_cpu[t]-before_cpu.get(t,after_cpu[t]))/ticks/elapsed*100 for t in after_cpu],reverse=True)
    result = dict(filesystem=subprocess.check_output(['stat','-f','-c','%T',str(fixture.root)],text=True).strip(),scenario=label, connections=len(clients), offered_messages_per_second=rate,
                  delivered=len(samples), elapsed_s=round(elapsed,3), delivered_per_second=round(len(samples)/elapsed),
                  publish_to_receive_ms=distribution(samples), publish_to_puback_ms=distribution([v for p in pairs for v in p.acks]),
                  client_event_loop_lag_ms=distribution(lag), broker_thread_cpu_percent=[round(v,1) for v in cpu[:5]],
                  remote_authorization_calls=fixture.counts['/authorization']-before_calls)
    if rate is not None:
        result.update(schedule_lag_ms=distribution(schedule_lag),peak_active_tasks=peak)
    print(json.dumps(result),flush=True)
    return result

async def run(fixture, count, rates, seconds, saturated_messages):
    clients=[]
    gate=asyncio.Semaphore(8)
    async def prepare(index):
        async with gate:
            reader=await PersistentClient.connect(fixture.port,'reader-'+str(index));clients.append(reader)
            writer=await AsyncClient.connect(fixture.port,'writer-'+str(index));clients.append(writer)
            pair=TimedPair(reader,writer,index,4096)
            assert await reader.subscribe(pair.topic)==1
            await pair.transfer()
            return pair
    try:
        pairs=await asyncio.gather(*(prepare(i) for i in range(count)))
        if count==1:
            return [await measure(fixture,pairs,clients,'one publisher and one persistent consumer',batch_messages=256,window=1)]
        result=[]
        for rate in rates:
            result.append(await measure(fixture,pairs,clients,f'{count*2} connections at {rate} messages/s',rate=rate,duration=seconds))
        if saturated_messages:
            result.append(await measure(fixture,pairs,clients,f'{count*2} connections, saturated window 4',batch_messages=saturated_messages))
        return result
    finally:
        await asyncio.gather(*(c.close() for c in clients),return_exceptions=True)

def main():
    parser=argparse.ArgumentParser(description='Actual publish-to-receive latency at bounded offered load, including checkpoint intervals.')
    parser.add_argument('--broker',required=True)
    parser.add_argument('--pairs',type=int,default=500)
    parser.add_argument('--seconds',type=int,default=30)
    parser.add_argument('--rates',default='1000,3000')
    parser.add_argument('--saturated-messages',type=int,default=128)
    parser.add_argument('--report',required=True)
    args=parser.parse_args()
    rates=[int(value) for value in args.rates.split(',')]
    assert 2<=args.pairs<=500 and 1<=args.seconds<=120 and all(0<rate<=20000 for rate in rates)
    results=[]
    for count in [1,args.pairs]:
        with Fixture(args.broker,persistence={},server_threads=2,http_workers=4,http_queue_capacity=64,cache_max_entries=16384,cache_ttl_ms=300000) as fixture:
            fixture.fresh=300000
            results.extend(asyncio.run(run(fixture,count,rates,args.seconds,args.saturated_messages)))
    Path(args.report).write_text(json.dumps(dict(storage='native partition log v2',message_partitions=4,state_partitions=4,
        io_workers=8,event_threads=2,payload_bytes=4096,seconds_per_rate=args.seconds,measurements=results),indent=2)+'\n')

if __name__=='__main__':main()
