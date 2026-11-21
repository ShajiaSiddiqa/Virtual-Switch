"""Run from the repository root, with no other switch instance running."""
import os
import select
import signal
import socket
import struct
import subprocess
import time
import threading
import shlex
from contextlib import ExitStack

PORT = int(os.environ.get("PORT_ADMIN_TEST_PORT", "9100"))

def command(*args, ok=True):
    r = subprocess.run(["./swctl", *args], capture_output=True, text=True, timeout=8)
    assert (r.returncode == 0) == ok, (args, r.stdout, r.stderr)
    return r.stdout

def exact(peer, expected):
    data = bytearray()
    while len(data) < len(expected):
        chunk = peer.recv(len(expected) - len(data))
        assert chunk, "Unexpected EOF"
        data.extend(chunk)
    assert bytes(data) == expected, (data, expected)

def quiet(*peers):
    assert not select.select(peers, [], [], 0.2)[0], "Unexpected frame or EOF"

def packet(src, dst, vlan, payload):
    return struct.pack("!H6s6sHB", len(payload), src, dst, vlan, 1) + payload

def exact_reply(peer):
    data=bytearray()
    while True:
        chunk=peer.recv(64)
        if not chunk:
            return bytes(data)
        data.extend(chunk)

A = bytes.fromhex("020000000001")
C = bytes.fromhex("020000000003")
D = bytes.fromhex("020000000004")
BROADCAST = b"\xff" * 6
os.makedirs("build", exist_ok=True)
with open("build/port-admin-integration.log", "w+") as log:
    launcher = shlex.split(os.environ.get("SWITCH_COMMAND", "./switch"))
    process = subprocess.Popen([*launcher, str(PORT), "3"], stdout=log,
                               stderr=subprocess.STDOUT)
    stop = threading.Event()
    traffic = None
    failures = []
    verified = [0]
    try:
        deadline = time.monotonic() + 15
        while True:
            assert process.poll() is None, "Switch exited during startup"
            if os.path.exists("/tmp/vswitch.sock"):
                try:
                    if "PORT" in command("show", "ports"):
                        break
                except (AssertionError, OSError):
                    pass
            assert time.monotonic() < deadline, "Startup timeout"
            time.sleep(0.02)
        with ExitStack() as stack:
            def connect():
                return stack.enter_context(socket.create_connection(
                    ("127.0.0.1", PORT), timeout=5))
            def active(count):
                deadline = time.monotonic() + 8
                while command("show", "ports").count("ACTIVE") != count:
                    assert time.monotonic() < deadline, "Admission timeout"
                    time.sleep(0.02)
            a,b,c = connect(),connect(),connect()
            active(3)
            f = packet(A,BROADCAST,1,b"learn A")
            a.sendall(f); exact(b,f); exact(c,f)
            a.sendall(f[:5])
            command("shutdown","1")
            assert "1  DOWN" in command("show","ports")
            assert "02:00:00:00:00:01" not in command("show","mac")
            try:
                assert a.recv(1)==b"", "Disabled client remained connected"
            except ConnectionResetError:
                pass
            a.close()
            command("shutdown","1")
            waiting=connect()
            assert exact_reply(waiting)==b"switch full"
            waiting.close()
            print("PASS: shutdown disconnects, removes MAC, and prevents slot reuse")
            command("up","1"); command("up","1")
            d=connect(); active(3)
            f=packet(D,BROADCAST,1,b"replacement")
            d.sendall(f); exact(b,f); exact(c,f)
            print("PASS: repeated up does not create extra permits; replacement forwards")
            f=packet(C,BROADCAST,1,b"learn C")
            c.sendall(f); exact(d,f); exact(b,f)

            def forwarding():
                try:
                    i=0
                    while not stop.is_set():
                        f=packet(D,C,1,i.to_bytes(4,"big"))
                        d.sendall(f); exact(c,f)
                        verified[0]+=1; i+=1
                        time.sleep(0.002)
                except Exception as error:
                    failures.append(error)
                    stop.set()
            traffic=threading.Thread(target=forwarding)
            traffic.start()
            for _ in range(5):
                # Refresh source learning; lookup alone does not refresh aging.
                announce=packet(C,BROADCAST,1,b"refresh C")
                c.sendall(announce); exact(b,announce)
                command("shutdown","2")
                try:
                    assert b.recv(1)==b""
                except ConnectionResetError:
                    pass
                b.close()
                command("up","2")
                b=connect(); active(3)
            stop.set(); traffic.join(timeout=8)
            assert not traffic.is_alive(), "Traffic thread did not finish"
            assert not failures, failures
            assert verified[0]>0
            print(f"PASS: five shutdown/up/reconnect cycles with {verified[0]} verified unrelated frames")
            command("shutdown","0",ok=False)
            command("up","4",ok=False)
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=10)==0
        assert not os.path.exists("/tmp/vswitch.sock")
        assert not os.path.exists("/dev/shm/vswitch_stats")
        print("PASS: invalid commands rejected and global cleanup complete")
    finally:
        stop.set()
        if process.poll() is None:
            process.kill(); process.wait()
        if traffic is not None:
            traffic.join(timeout=6)
        log.seek(0)
        print(log.read(),end="")
