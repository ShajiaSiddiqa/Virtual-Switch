#!/usr/bin/env python3
"""Full launcher/TCP/admin integration. Run from the repository root."""
import os
import pathlib
import select
import signal
import socket
import struct
import subprocess
import time
from contextlib import ExitStack

BASE = int(os.environ.get("TWO_SWITCH_TEST_PORT", "9200"))
A = bytes.fromhex("020000000001")
B = bytes.fromhex("020000000002")
BROADCAST = b"\xff" * 6


def packet(src, dst, vlan, payload, priority=1):
    return struct.pack("!H6s6sHB", len(payload), src, dst, vlan, priority) + payload


def expect(peer, wanted):
    actual = bytearray()
    while len(actual) < len(wanted):
        data = peer.recv(len(wanted) - len(actual))
        assert data, "EOF before complete frame"
        actual.extend(data)
    assert actual == wanted, "Frame mismatch"


def quiet(*peers):
    assert not select.select(peers, [], [], 0.2)[0], "Unexpected frame or EOF"


def until(check, process, description):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        assert process.poll() is None, f"Launcher exited waiting for {description}"
        if check():
            return
        time.sleep(0.05)
    raise TimeoutError(description)


pathlib.Path("build").mkdir(exist_ok=True)
with open("build/two-switch-integration.log", "w+") as log:
    process = subprocess.Popen(
        ["./build/two_switches", str(BASE), str(BASE + 1), "3"],
        stdout=log, stderr=subprocess.STDOUT,
    )
    owner = process.pid
    paths = [f"/tmp/vswitch-{owner}-{side}.sock" for side in "ab"]
    shms = [f"/dev/shm/vswitch_{owner}_{side}_stats" for side in "ab"]

    def command(side, *args, success=True):
        env = dict(os.environ, VSWITCH_SOCKET=paths[side],
                   VSWITCH_STATS=f"/vswitch_{owner}_{'ab'[side]}_stats")
        result = subprocess.run(["./swctl", *args], env=env, text=True,
                                capture_output=True, timeout=15)
        if success:
            assert result.returncode == 0, result.stdout + result.stderr
        else:
            assert result.returncode != 0, "Invalid command accepted"
        return result.stdout

    try:
        until(lambda: all(os.path.exists(p) for p in paths), process, "admin sockets")
        until(lambda: all("3  ACTIVE  1  TRUNK" in command(i, "show", "ports")
                          for i in range(2)), process, "active pipe trunks")
        for side in range(2):
            command(side, "vlan", "1", "10")
            command(side, "vlan", "2", "20")
        with ExitStack() as stack:
            peers = []
            for port in (BASE, BASE, BASE + 1, BASE + 1):
                peer = stack.enter_context(socket.create_connection(("127.0.0.1", port), timeout=5))
                peer.settimeout(5)
                peers.append(peer)
            a10, a20, b10, b20 = peers
            until(lambda: all(command(i, "show", "ports").count("ACTIVE") == 3
                              for i in range(2)), process, "host admission")
            frame = packet(A, BROADCAST, 10, b"A broadcast", 0)
            a10.sendall(frame)
            expect(b10, frame)
            quiet(a10, a20, b20)
            frame = packet(B, BROADCAST, 10, b"B broadcast", 1)
            b10.sendall(frame)
            expect(a10, frame)
            quiet(b10, a20, b20)
            print("PASS: bidirectional cross-switch VLAN10 broadcast, isolation and no echo")

            frame = packet(A, BROADCAST, 20, b"same A, VLAN20", 2)
            a20.sendall(frame)
            expect(b20, frame)
            quiet(a20, a10, b10)
            frame = packet(B, BROADCAST, 20, b"same B, VLAN20", 2)
            b20.sendall(frame)
            expect(a20, frame)
            quiet(b20, a10, b10)
            for side, remote in ((0, "02:00:00:00:00:02"), (1, "02:00:00:00:00:01")):
                rows = [line.split() for line in command(side, "show", "mac").splitlines()[1:]]
                assert [remote, "3", "10"] in rows, rows
                assert [remote, "3", "20"] in rows, rows
            print("PASS: remote MACs learned on trunk independently in VLAN10 and VLAN20")

            for source, target, src, dst, vlan, priority in (
                (a10, b10, A, B, 10, 0), (b10, a10, B, A, 10, 1),
                (a20, b20, A, B, 20, 2), (b20, a20, B, A, 20, 2),
            ):
                frame = packet(src, dst, vlan, b"learned unicast", priority)
                source.sendall(frame)
                expect(target, frame)
                quiet(*(peer for peer in peers if peer is not target))
            print("PASS: learned unicast in both directions preserves VLAN, priority and payload")

            for side in range(2):
                output = command(side, "stats")
                row = next(line.split() for line in output.splitlines() if line.split()[0] == "3")
                assert int(row[1]) > 0 and int(row[2]) > 0, output
                command(side, "vlan", "3", "20", success=False)
                command(side, "trunk", "0", success=False)
                command(side, "shutdown", "3", success=False)
            print("PASS: independent shared statistics and reserved pipe-trunk validation")

            command(0, "clear", "mac")
            assert len(command(0, "show", "mac").splitlines()) == 1
            frame = packet(B, A, 10, b"after clear")
            b10.sendall(frame)
            expect(a10, frame)
            quiet(b10, a20, b20)
            print("PASS: MAC clear on one switch leaves cross-switch forwarding operational")

            # Leave a receiver partway through a header, then stop the launcher.
            a10.sendall(b"x")
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=15) == 0, "Launcher or switch failed shutdown"
        assert not any(os.path.exists(p) for p in paths + shms), "IPC resources remain"
        print("PASS: launcher reaped both switches and removed both instances' IPC resources")
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                # Kill children before the parent to avoid leaving test workers behind.
                for line in pathlib.Path(f"/proc/{process.pid}/task/{process.pid}/children").read_text().split():
                    try:
                        os.kill(int(line), signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                process.kill()
                process.wait()
        log.seek(0)
        print("--- launcher/switch output ---")
        print(log.read(), end="")
