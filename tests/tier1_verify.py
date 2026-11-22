#!/usr/bin/env python3
"""Core forwarding, port reuse and slow-host isolation; owns each test instance."""
import os
from pathlib import Path
import re
import select
import shlex
import signal
import socket
import struct
import subprocess
import sys
import time
from contextlib import ExitStack
A=bytes.fromhex("020000000001")
B=bytes.fromhex("020000000002")
C=bytes.fromhex("020000000003")
D=bytes.fromhex("020000000004")
BROADCAST=b"\xff"*6

def frame(src,dst,payload):
    return struct.pack("!H6s6sHB",len(payload),src,dst,1,1)+payload

def expect(peer,wanted):
    actual=bytearray()
    while len(actual)<len(wanted):
        part=peer.recv(len(wanted)-len(actual))
        assert part,"EOF in frame"
        actual.extend(part)
    assert actual==wanted,"Frame mismatch"

def quiet(*peers):
    assert not select.select(peers,[],[],0.3)[0],"Unexpected frame or EOF"

def connect(stack,port,slow=False):
    peer=stack.enter_context(socket.socket())
    peer.settimeout(10)
    if slow:peer.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096)
    peer.connect(("127.0.0.1",port))
    return peer

def ready(a,b,c):
    packet=frame(C,BROADCAST,b"ready")
    c.sendall(packet);expect(a,packet);expect(b,packet)

def forwarding(port):
    with ExitStack() as stack:
        a,b,c=[connect(stack,port) for _ in range(3)]
        ready(a,b,c)
        packet=frame(A,B,b"unknown B")
        a.sendall(packet);expect(b,packet);expect(c,packet);quiet(a)
        packet=frame(B,BROADCAST,b"learn B")
        b.sendall(packet);expect(a,packet);expect(c,packet)
        packet=frame(A,B,b"known B")
        a.sendall(packet);expect(b,packet);quiet(a,c)
        print("PASS: unknown unicast, broadcast, known unicast, and no echo")
        a.close()
        d=connect(stack,port)
        packet=frame(D,B,b"replacement")
        d.sendall(packet);expect(b,packet);quiet(c)
        packet=frame(B,A,b"old A unknown")
        b.sendall(packet);expect(c,packet);expect(d,packet)
        print("PASS: replacement forwards and the disconnected MAC floods")

def slow_host(port):
    with ExitStack() as stack:
        a=connect(stack,port);b=connect(stack,port,True);c=connect(stack,port)
        ready(a,b,c)
        for i in range(6000):
            packet=frame(A,BROADCAST,i.to_bytes(4,"big")+b"x"*1496)
            a.sendall(packet);expect(c,packet)
        print("PASS: C received all 6000 frames while B never read the load")

def run_case(command,port,test):
    name=f"vsw_tier1_{os.getpid()}_{port}"
    socket_path=f"/tmp/{name}.sock"
    shm_path=Path("/dev/shm")/name
    env=dict(os.environ,VSWITCH_SOCKET=socket_path,VSWITCH_STATS="/"+name,
             SWITCH_LOG_PATH=f"build/tier1-{port}-events.log")
    for key in ("VSWITCH_MAC_STORE","SWITCH_PIPE_READ_FD","SWITCH_PIPE_WRITE_FD"):
        env.pop(key,None)
    with open(f"build/tier1-{port}.log","w+") as log:
        process=subprocess.Popen([*command,str(port),"3"],env=env,stdout=log,stderr=subprocess.STDOUT)
        try:
            deadline=time.monotonic()+15
            while not Path(socket_path).exists():
                assert process.poll() is None,"Switch exited during startup"
                assert time.monotonic()<deadline,"Startup timeout"
                time.sleep(0.05)
            test(port)
        finally:
            if process.poll() is None:process.send_signal(signal.SIGINT)
            try:result=process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill();process.wait()
                raise RuntimeError("Switch failed bounded shutdown")
            finally:
                log.seek(0);output=log.read();print(output,end="")
                # Check self-cleanup before removing any failure leftovers.
                remains=Path(socket_path).exists() or shm_path.exists()
                Path(socket_path).unlink(missing_ok=True);shm_path.unlink(missing_ok=True)
        assert result==0,f"Switch/tool status {result}"
        assert not remains,"Switch left IPC resources"
        assert "ThreadSanitizer" not in output,output
        return output

Path("build").mkdir(exist_ok=True)
command=sys.argv[1:] or shlex.split(os.environ.get("SWITCH_COMMAND",os.environ.get("SWITCH","./switch")))
base=int(os.environ.get("TIER1_PORT","9010"))
run_case(command,base,forwarding)
output=run_case(command,base+1,slow_host)
drops=dict(re.findall(r"Port (\d+): (\d+) dropped deliveries",output))
assert int(drops.get("2","0"))>0,"Slow host did not record queue rejection"
assert drops.get("3")=="0","Fast port dropped frames"
print("PASS: forwarding, port reuse, slow-host isolation, and clean exit")
