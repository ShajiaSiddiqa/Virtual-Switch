"""Run from the repository root, with no other switch instance running."""
import os
import select
import signal
import socket
import struct
import subprocess
import time
from contextlib import ExitStack

PORT = int(os.environ.get("MIRROR_TEST_PORT", "9090"))

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
C = bytes.fromhex("020000000003")
BROADCAST = b"\xff" * 6
os.makedirs("build", exist_ok=True)
with open("build/mirror-integration.log", "w+") as log:
    process = subprocess.Popen(["./switch", str(PORT), "3"], stdout=log,
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
            a, b = [stack.enter_context(socket.create_connection(
                ("127.0.0.1", PORT), timeout=5)) for _ in range(2)]
            sniffer = stack.enter_context(socket.socket())
            sniffer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            sniffer.settimeout(5)
            sniffer.connect(("127.0.0.1", PORT))
            deadline = time.monotonic() + 5
            while command("show", "ports").count("ACTIVE") != 3:
                assert time.monotonic() < deadline, "Admission timeout"
                time.sleep(0.02)
            command("vlan", "1", "10")
            command("vlan", "2", "10")
            command("vlan", "3", "20")
            command("mirror", "1", "3")
            assert "SNIFFER" in command("show", "ports")
            f = packet(A, BROADCAST, 10, b"mirrored")
            a.sendall(f); exact(b, f); exact(sniffer, f); quiet(a)
            print("PASS: monitored ingress reaches normal host and cross-VLAN sniffer")
            f = packet(B, BROADCAST, 10, b"not monitored")
            b.sendall(f); exact(a, f); quiet(b, sniffer)
            print("PASS: sniffer excluded from ordinary broadcast flooding")
            f = packet(C, BROADCAST, 20, b"forbidden injection")
            sniffer.sendall(f); quiet(a, b, sniffer)
            command("mirror", "1", "1", ok=False)
            command("mirror", "3", "2", ok=False)
            command("trunk", "3", ok=False)
            command("mirror", "1", "0")
            f = packet(A, B, 10, b"disabled")
            a.sendall(f); exact(b, f); quiet(a, sniffer)
            print("PASS: sniffer injection blocked, invalid commands rejected, disable works")
            command("mirror", "1", "3")
            # B's MAC was learned above. Sniffer does not read this workload.
            for i in range(6000):
                f = packet(A, B, 10, i.to_bytes(4, "big") + b"x" * 1496)
                a.sendall(f); exact(b, f)
            print("PASS: normal host received all 6000 frames while sniffer never read")
            rows = command("stats").splitlines()[1:]
            counters = {int(row.split()[0]): list(map(int, row.split()[1:]))
                        for row in rows if row.strip()}
            assert counters[2][2] == 0, counters
            assert counters[3][3] > 0, counters
            assert counters[3][2] > 1, counters
            print("PASS: mirror counter advanced, sniffer drops counted, normal port drops zero")
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
