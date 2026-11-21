"""Run from the repository root, with no other switch instance running."""
import os
import select
import signal
import socket
import struct
import subprocess
import time
from contextlib import ExitStack

PORT = int(os.environ.get("VLAN_TEST_PORT", "9080"))

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

A = bytes.fromhex("020000000001")
B = bytes.fromhex("020000000002")
D = bytes.fromhex("020000000004")
BROADCAST = b"\xff" * 6
os.makedirs("build", exist_ok=True)
with open("build/vlan-integration.log", "w+") as log:
    process = subprocess.Popen(["./switch", str(PORT), "4"], stdout=log,
                               stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 8
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
            a, b, c, trunk = [stack.enter_context(socket.create_connection(
                ("127.0.0.1", PORT), timeout=5)) for _ in range(4)]
            deadline = time.monotonic() + 5
            while command("show", "ports").count("ACTIVE") != 4:
                assert time.monotonic() < deadline, "Admission timeout"
                time.sleep(0.02)
            for number, vlan in ((1, 10), (2, 10), (3, 20)):
                command("vlan", str(number), str(vlan))
            command("trunk", "4")
            print(command("show", "ports"), end="")

            f = packet(A, BROADCAST, 10, b"VLAN10")
            a.sendall(f); exact(b, f); exact(trunk, f); quiet(a, c)
            print("PASS: VLAN10 broadcast confined to B and trunk")
            f = packet(A, BROADCAST, 20, b"forbidden")
            a.sendall(f); quiet(a, b, c, trunk)
            print("PASS: access port cannot inject a different VLAN")

            f = packet(A, BROADCAST, 20, b"same MAC VLAN20")
            c.sendall(f); exact(trunk, f); quiet(a, b, c)
            macs = command("show", "mac")
            assert macs.count("02:00:00:00:00:01") == 2, macs
            for vlan, target, others in ((10, a, (b, c, trunk)),
                                         (20, c, (a, b, trunk))):
                f = packet(D, A, vlan, b"known")
                trunk.sendall(f); exact(target, f); quiet(*others)
            print("PASS: identical MACs route independently through trunk")

            command("vlan", "2", "20")
            f = packet(B, BROADCAST, 20, b"B moved VLAN")
            b.sendall(f); exact(c, f); exact(trunk, f); quiet(a, b)
            print("PASS: live VLAN reassignment changes forwarding")
            command("vlan", "0", "10", ok=False)
            command("vlan", "2", "4095", ok=False)
            command("trunk", "1")
            assert command("show", "ports").count("TRUNK") == 1
            command("trunk", "0")
            assert "TRUNK" not in command("show", "ports")
            print("PASS: invalid commands rejected and at most one trunk")
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=8) == 0
        assert not os.path.exists("/tmp/vswitch.sock")
        assert not os.path.exists("/dev/shm/vswitch_stats")
        print("PASS: clean shutdown and resource removal")
    finally:
        if process.poll() is None:
            process.kill(); process.wait()
        log.seek(0)
        print(log.read(), end="")
