#!/usr/bin/env python3
"""Read-only legacy migration and actual MQTT replay from the imported log."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import sqlite3
import struct
import subprocess
import tempfile
import time
from durable_integration import client, restart
from http_auth_cache_integration import Fixture
from http_auth_integration import packet, utf

def run(binary, importer):
    script=Path(__file__).resolve().parent.parent/'bin/migrate-sqlite-journal.py'
    with tempfile.TemporaryDirectory(prefix='mqtts-migration-') as directory:
        root=Path(directory);source=root/'legacy.db';target=root/'journal'
        db=sqlite3.connect(source)
        db.executescript('''
          PRAGMA user_version=1;
          CREATE TABLE sessions(client TEXT PRIMARY KEY,owner TEXT,epoch INTEGER,expiry INTEGER,deadline INTEGER,next_packet INTEGER);
          CREATE TABLE subscriptions(client TEXT,filter TEXT,qos INTEGER);
          CREATE TABLE messages(id INTEGER PRIMARY KEY,wire BLOB,expires INTEGER);
          CREATE TABLE deliveries(client TEXT,message INTEGER,packet INTEGER);
        ''')
        db.execute('INSERT INTO sessions VALUES(?,?,?,?,?,?)',('reader','reader',7,60,int(time.time()*1000)+60000,42))
        db.execute('INSERT INTO subscriptions VALUES(?,?,?)',('reader','fixture/#',1))
        expected=[]
        for i,pid in [(1,41),(2,0)]:
            payload=json.dumps(dict(actor='writer',nonce=i,data='migrated')).encode();expected.append(payload)
            wire=packet(0x32,utf('fixture/migrated')+struct.pack('!H',1)+b'\0'+payload)
            db.execute('INSERT INTO messages VALUES(?,?,?)',(i,wire,0));db.execute('INSERT INTO deliveries VALUES(?,?,?)',('reader',i,pid))
        db.commit();db.close();before=hashlib.sha256(source.read_bytes()).hexdigest()
        command=['python3',str(script),'--source',str(source),'--destination',str(target),'--importer',importer]
        with open(str(source)+'.lock','w') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            result=subprocess.run(command,capture_output=True,text=True)
            assert result.returncode and not target.exists(),'migration ignored broker ownership lock'
        subprocess.run(command,check=True)
        assert hashlib.sha256(source.read_bytes()).hexdigest()==before,'migration modified source'
        result=subprocess.run(command,capture_output=True,text=True);assert result.returncode,'migration overwrote destination'
        with Fixture(binary,persistence=dict(path=str(target))) as f:
            r,present=client(f,'reader',receive=1);assert present
            first=r.delivery();assert first[1]==expected[0] and first[2]==41 and first[3]
            assert r.delivery()[1]==expected[1];r.quiet()
            restart(f);r,present=client(f,'reader');assert present;r.quiet()
            # Legacy rows have no option columns, so No Local stays clear and the
            # restored session receives its own publish.
            data=json.dumps(dict(actor='reader',nonce=7,data='legacy-self')).encode()
            r.send(packet(0x32,utf('fixture/migrated')+b'\0\1\0'+data))
            saw_ack=saw_echo=False
            for _ in range(2):
                head,body=r.read()
                if head==0x40: saw_ack=body[:2]==b'\0\1' and (len(body)==2 or body[2]<128)
                elif head>>4==3: saw_echo=body.endswith(data) and (head&1)==0
                else: raise AssertionError((head,body))
            assert saw_ack and saw_echo
            r.quiet()
            client(f,'reader',owner='attacker',clean=True,expected=1)
        assert hashlib.sha256(source.read_bytes()).hexdigest()==before
        print('PASS read-only migration, ownership lock, destination refusal, session/Packet ID/ACK replay',flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--broker',required=True);p.add_argument('--importer',required=True)
    a=p.parse_args();run(a.broker,a.importer)
