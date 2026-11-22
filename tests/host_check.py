#!/usr/bin/env python3
"""Test the actual host against a protocol peer, independently of switch policy."""
import os
import shlex
import socket
import struct
import subprocess
import threading
import time
from contextlib import contextmanager

COMMAND = shlex.split(os.environ.get("HOST_COMMAND", os.environ.get("HOST", "./host")))
MAC = "02:00:00:00:00:0a"
SRC = bytes.fromhex("02000000000a")


@contextmanager
def host():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        process = subprocess.Popen([*COMMAND, "127.0.0.1", str(listener.getsockname()[1]), MAC],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        lines = []
        # Append lines as they arrive, rather than waiting for EOF.
        def collect():
            for line in process.stdout:
                lines.append(line.rstrip("\n"))
        reader = threading.Thread(target=collect)
        reader.start()
        try:
            peer, _ = listener.accept()
            with peer:
                peer.settimeout(5)
                deadline = time.monotonic() + 10
                while not any(line.startswith("CONNECTED") for line in lines):
                    assert process.poll() is None, lines
                    assert time.monotonic() < deadline, lines
                    time.sleep(0.01)
                yield process, peer, lines
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if not process.stdin.closed:
                process.stdin.close()
            reader.join(timeout=5)
            assert not reader.is_alive(), "Output reader did not stop"
            process.stdout.close()
            assert not any("ThreadSanitizer" in line for line in lines), lines


def read_exact(peer, length):
    result = bytearray()
    while len(result) < length:
        part = peer.recv(length - len(result))
        assert part, "EOF in outgoing host frame"
        result.extend(part)
    return bytes(result)


def packet(peer):
    length, src, dst, vlan, priority = struct.unpack("!H6s6sHB", read_exact(peer, 17))
    return src, dst, vlan, priority, read_exact(peer, length)


def wire(payload, vlan=1, priority=1):
    return struct.pack("!H6s6sHB", len(payload), b"\x02\0\0\0\0\x0b", SRC, vlan, priority) + payload


def write(process, text):
    process.stdin.write(text)
    process.stdin.flush()


with host() as (process, peer, lines):
    write(process, "send ff:ff:ff:ff:ff:ff 1 hello from host\nsendp 02:00:00:00:00:0b 20 0 high priority\n")
    assert packet(peer) == (SRC, b"\xff" * 6, 1, 1, b"hello from host")
    assert packet(peer) == (SRC, bytes.fromhex("02000000000b"), 20, 0, b"high priority")
    payload = (bytes(range(256)) * 6)[:1500]
    message = wire(payload, 20, 2)
    peer.sendall(message[:5])
    peer.sendall(message[5:] + wire(b"") + wire(b"reply"))
    deadline = time.monotonic() + 10
    while sum(line.startswith("FRAME ") for line in lines) < 3:
        assert time.monotonic() < deadline, lines
        assert process.poll() is None, lines
        time.sleep(0.01)
    assert any("length=1500 text=\\x00" in line for line in lines), lines
    assert any("length=0 text=" in line for line in lines), lines
    assert any("text=reply" in line for line in lines), lines
    process.stdin.close()
    assert process.wait(timeout=10) == 0, lines
print("PASS: host sends normal/priority frames and receives fragmented, coalesced, binary and empty frames")

with host() as (process, peer, lines):
    write(process, "recv 1\n")
    process.stdin.close()
    peer.sendall(wire(b"hello"))
    assert process.wait(timeout=10) == 0, lines
    assert any("text=hello" in line for line in lines), lines
print("PASS: piped recv 1 exits after the requested frame despite stdin EOF")

with host() as (process, peer, lines):
    write(process, "send ff:ff:ff:ff:ff:ff 0 invalid\n")
    assert process.wait(timeout=10) == 1, lines
print("PASS: invalid host command rejected")

with host() as (process, peer, lines):
    peer.sendall(wire(b"truncated")[:19])
    peer.shutdown(socket.SHUT_WR)
    assert process.wait(timeout=10) == 1, lines
print("PASS: truncated incoming frame fails without hanging")

with host() as (process, peer, lines):
    process.terminate()
    assert process.wait(timeout=10) == 0, lines
print("PASS: SIGTERM stops a host blocked on input/network")
