#!/usr/bin/env python3
"""Offline, read-only v1 SQLite export into a new native partition-log store."""
import argparse
import fcntl
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import tempfile
import zlib

class Encoder:
    def __init__(self): self.data=bytearray()
    def u8(self,v): self.data.extend(struct.pack('<B',v))
    def u32(self,v): self.data.extend(struct.pack('<I',v))
    def u64(self,v): self.data.extend(struct.pack('<Q',v))
    def text(self,v):
        if isinstance(v,str):v=v.encode()
        self.u32(len(v));self.data.extend(v)

def mqtt_topic(wire):
    if not wire or wire[0]>>4!=3:raise ValueError('legacy record is not PUBLISH')
    pos=1
    for _ in range(4):
        byte=wire[pos];pos+=1
        if not byte&128:break
    else:raise ValueError('invalid legacy remaining length')
    length=int.from_bytes(wire[pos:pos+2],'big');pos+=2
    if not length or pos+length>len(wire):raise ValueError('invalid legacy topic')
    return wire[pos:pos+length].decode()

def migrate(source,destination,importer,partitions):
    if source.is_symlink():raise ValueError('source must be the broker configured regular file, not a symlink')
    source=source.resolve(strict=True);destination=destination.absolute()
    if destination.exists():raise ValueError('destination must be a new path; source is never overwritten')
    lock=os.open(str(source)+'.lock',os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600)
    try:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        db=sqlite3.connect(source.as_uri()+'?mode=ro',uri=True)
        try:
            db.execute('BEGIN')
            if db.execute('PRAGMA user_version').fetchone()!=(1,):raise ValueError('unsupported legacy schema')
            sessions=db.execute('SELECT client,owner,epoch,expiry,deadline,next_packet FROM sessions ORDER BY client').fetchall()
            messages=db.execute('SELECT id,wire,expires FROM messages ORDER BY id').fetchall()
            if len(sessions)>10000 or len(messages)>100000:raise ValueError('source exceeds default importer limits')
            recipients={}
            for client,ident,packet in db.execute('SELECT client,message,packet FROM deliveries ORDER BY message,client'):
                recipients.setdefault(ident,[]).append((client,packet))
            if sum(len(v) for v in recipients.values())>100000:raise ValueError('source exceeds pending delivery limit')
            e=Encoder();e.text('MQTTS-SQLITE-EXPORT-1')
            e.u64(max([0]+[s[2] for s in sessions]+[m[0] for m in messages]));e.u32(len(sessions))
            generations={s[0]:s[2] for s in sessions}
            for client,owner,epoch,expiry,deadline,next_packet in sessions:
                e.text(client);e.text(owner);e.u64(epoch);e.u32(expiry);e.u64(deadline);e.u32(next_packet)
                subs=db.execute('SELECT filter,qos FROM subscriptions WHERE client=? ORDER BY filter',(client,)).fetchall()
                e.u32(len(subs))
                for topic,qos in subs:e.text(topic);e.u8(qos)
            e.u32(len(messages))
            for ident,wire,expires in messages:
                e.u64(ident);e.u64(expires);e.text(mqtt_topic(wire));e.text(wire)
                targets=recipients.get(ident,[]);e.u32(len(targets))
                for client,packet in targets:e.text(client);e.u64(generations[client]);e.u32(packet)
        finally:db.close()
        with tempfile.TemporaryDirectory(prefix='mqtts-offline-export-') as temporary:
            export=Path(temporary)/'export.bin'
            fd=os.open(export,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
            with os.fdopen(fd,'wb') as output:
                output.write(struct.pack('<I',zlib.crc32(e.data)));output.write(e.data)
            subprocess.run([str(importer.resolve(strict=True)),str(export),str(destination),str(partitions)],check=True)
    finally:os.close(lock)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',required=True,type=Path);p.add_argument('--destination',required=True,type=Path)
    p.add_argument('--importer',required=True,type=Path);p.add_argument('--partitions',type=int,default=4)
    a=p.parse_args();migrate(a.source,a.destination,a.importer,a.partitions)
