import os
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import time
from contextlib import ExitStack

A = bytes.fromhex("020000000001")
B = bytes.fromhex("020000000002")
D = bytes.fromhex("020000000004")
BROADCAST = b"\xff" * 6

def frame(src, dst, payload):
    return struct.pack("!H6s6sHB", len(payload), src, dst, 1, 1) + payload

def expect(peer, expected):
    actual = bytearray()
    while len(actual) < len(expected):
        chunk = peer.recv(len(expected) - len(actual))
        assert chunk, "Connection closed before the full frame arrived"
        actual.extend(chunk)
    assert actual == expected, "Frame bytes differ"

def quiet(*peers):
    assert not select.select(peers, [], [], 0.3)[0], "Unexpected data or EOF"

def connect(stack, port, slow=False):
    peer = stack.enter_context(socket.socket())
    peer.settimeout(10)
    if slow:
        peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    peer.connect(("127.0.0.1", port))
    return peer

def wait_for_all_ports(a, b, c):
    ready = frame(bytes.fromhex("020000000003"), BROADCAST, b"ready")
    c.sendall(ready)
    expect(a, ready)
    expect(b, ready)


def forwarding_and_reuse(port):
    with ExitStack() as stack:
        a, b, c = [connect(stack, port) for _ in range(3)]
        wait_for_all_ports(a, b, c)

        unknown = frame(A, B, b"unknown B")
        a.sendall(unknown)
        expect(b, unknown)
        expect(c, unknown)
        quiet(a)

        announce = frame(B, BROADCAST, b"learn B")
        b.sendall(announce)
        expect(a, announce)
        expect(c, announce)

        known = frame(A, B, b"known B")
        a.sendall(known)
        expect(b, known)
        quiet(a, c)
        print("PASS: unknown unicast, broadcast, known unicast, and no echo")

        a.close()
        d = connect(stack, port)
        replacement = frame(D, B, b"replacement sends valid traffic")
        d.sendall(replacement)
        expect(b, replacement)
        quiet(c)

        old_mac = frame(B, A, b"old A is now unknown")
        b.sendall(old_mac)
        expect(c, old_mac)
        expect(d, old_mac)
        print("PASS: replacement forwards and the disconnected MAC floods")

def slow_host(port):
    with ExitStack() as stack:
        a = connect(stack, port)
        b = connect(stack, port, slow=True)
        c = connect(stack, port)
        wait_for_all_ports(a, b, c)

        for i in range(6000):
            packet = frame(A, BROADCAST, i.to_bytes(4, "big") + b"x" * 1496)
            a.sendall(packet)
            expect(c, packet)

        print("PASS: C received all 6000 frames while B never read")

def run_case(command, port, test):
    with open(f"build/tier1-{port}.log", "w+") as log:
        process = subprocess.Popen(
            [*command, str(port), "3"],
            stdout=log, stderr=subprocess.STDOUT,
        )
        try:
            # Check listener readiness without consuming a client slot.
            deadline = time.monotonic() + 10
            while True:
                listeners = subprocess.check_output(
                    ["ss", "-ltnH", f"sport = :{port}"], text=True
                )
                if process.poll() is not None:
                    raise RuntimeError(f"Switch exited during startup on {port}")
                if listeners.strip():
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"Switch did not listen on {port}")
                time.sleep(0.05)

            test(port)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
            try:
                result = process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise RuntimeError("Switch did not stop cleanly")
            finally:
                log.seek(0)
                output = log.read()
                print(output, end="")

        assert result == 0, f"Switch or diagnostic tool exited with status {result}"
        return output

command = sys.argv[1:] or ["./switch"]
base = int(os.environ.get("TIER1_PORT", "9010"))

run_case(command, base, forwarding_and_reuse)
output = run_case(command, base + 1, slow_host)
drops = dict(re.findall(r"Port (\d+): (\d+) dropped deliveries", output))
assert int(drops.get("2", "0")) > 0, "Slow port did not record any rejected deliveries"
assert drops.get("3") == "0", "Fast port did not report zero drops"
print("PASS: forwarding, port reuse, slow-host isolation, and clean exit")
