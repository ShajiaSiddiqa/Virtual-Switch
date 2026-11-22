#!/usr/bin/env python3
"""Actual switch crash/recovery test. Uses the test build's 60-second MAC age."""
import os
from pathlib import Path
import re
import select
import shlex
import signal
import socket
import struct
import subprocess
import time
from contextlib import ExitStack

PORT = int(os.environ.get("RECOVERY_TEST_PORT", "9300"))
SWITCH = os.environ.get("RECOVERY_SWITCH", "./build/switch_recovery")
PREFIX = shlex.split(os.environ.get("SUPERVISOR_COMMAND", "./build/switch_supervisor"))
A = bytes.fromhex("020000000001")
B = bytes.fromhex("020000000002")
C = bytes.fromhex("020000000003")
D = bytes.fromhex("020000000004")
BROADCAST = b"\xff" * 6


def frame(source, destination, payload):
    return struct.pack("!H6s6sHB", len(payload), source, destination, 1, 1) + payload


def expect(peer, wanted):
    actual = bytearray()
    while len(actual) < len(wanted):
        data = peer.recv(len(wanted) - len(actual))
        assert data, "Unexpected EOF"
        actual.extend(data)
    assert actual == wanted, f"Frame mismatch: {bytes(actual)!r}"


def quiet(*peers):
    assert not select.select(peers, [], [], 0.2)[0], "Unexpected data or EOF"


Path("build").mkdir(exist_ok=True)
log_path = Path(f"build/supervisor-integration-{PORT}.log")
with log_path.open("w+") as log:
    process = subprocess.Popen([*PREFIX, SWITCH, str(PORT), "3"],
                               stdout=log, stderr=subprocess.STDOUT)

    def output():
        return log_path.read_text()

    def wait_for(condition, description):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            assert process.poll() is None, output()
            if condition():
                return
            time.sleep(0.05)
        raise TimeoutError(description + "\n" + output())

    try:
        wait_for(lambda: "SUPERVISOR pid=" in output(), "supervisor startup")
        match = re.search(r"SUPERVISOR pid=(\d+) store=(\S+) socket=(\S+) stats=(\S+)", output())
        assert match, output()
        owner, store_name, socket_path, stats_name = match.groups()
        store_path = Path("/dev/shm") / store_name.lstrip("/")
        stats_path = Path("/dev/shm") / stats_name.lstrip("/")
        env = dict(os.environ, VSWITCH_SOCKET=socket_path, VSWITCH_STATS=stats_name)

        def command(*arguments):
            result = subprocess.run(["./swctl", *arguments], env=env,
                                    text=True, capture_output=True, timeout=15)
            assert result.returncode == 0, result.stdout + result.stderr
            return result.stdout

        def rows():
            return [line.split() for line in command("show", "mac").splitlines()[1:]]

        def workers():
            return re.findall(r"WORKER pid=(\d+) restart=(\d+) recovered_generation=(\d+) entries=(\d+)", output())

        def ready():
            try:
                return command("show", "ports").count("FREE") == 3
            except (AssertionError, subprocess.TimeoutExpired):
                return False

        def active(count):
            # connect() completing only confirms the TCP handshake, not admission.
            # Observe readiness without sending a frame that validates a MAC hint.
            try:
                return command("show", "ports").count("ACTIVE") == count
            except (AssertionError, subprocess.TimeoutExpired):
                return False

        def connect(stack):
            peer = stack.enter_context(socket.create_connection(("127.0.0.1", PORT), timeout=5))
            peer.settimeout(5)
            return peer

        def restart():
            previous = workers()
            old = int(previous[-1][0])
            os.kill(old, signal.SIGKILL)
            wait_for(lambda: len(workers()) > len(previous), "fresh replacement PID")
            assert int(workers()[-1][0]) != old
            wait_for(ready, "replacement admin listener and empty TCP slots")
            assert store_path.stat().st_ino == inode, "Supervisor recreated the recovery object"

        wait_for(ready, "first worker readiness")
        inode = store_path.stat().st_ino
        with ExitStack() as stack:
            a, b, c = [connect(stack) for _ in range(3)]
            wait_for(lambda: active(3), "three admitted initial clients")
            packet = frame(C, BROADCAST, b"ready")
            c.sendall(packet)
            expect(a, packet)
            expect(b, packet)
            packet = frame(A, B, b"unknown B")
            a.sendall(packet)
            expect(b, packet)
            expect(c, packet)
            packet = frame(B, BROADCAST, b"learn B")
            b.sendall(packet)
            expect(a, packet)
            expect(c, packet)
            packet = frame(A, B, b"known B")
            a.sendall(packet)
            expect(b, packet)
            quiet(a, c)
            before = rows()
            assert ["02:00:00:00:00:01", "1", "1"] in before, before
            assert ["02:00:00:00:00:02", "2", "1"] in before, before
            a.sendall(b"x")  # Leave a live partial frame at crash time.
            restart()  # Kill before context cleanup can delete live learned entries.
        assert sorted(rows()) == sorted(before), rows()
        assert int(workers()[-1][2]) > 0 and workers()[-1][3] == "3", workers()
        print("PASS: actual switch SIGKILL restarts a fresh executable with all three MAC mappings")

        with ExitStack() as stack:
            unrelated, b, a = [connect(stack) for _ in range(3)]
            wait_for(lambda: active(3), "three admitted replacement clients")
            # A used to occupy slot 1. It now occupies slot 3. A recovered hint
            # must not select the unrelated client now occupying old slot 1.
            packet = frame(B, A, b"unvalidated destination floods")
            b.sendall(packet)
            expect(unrelated, packet)
            expect(a, packet)
            quiet(b)
            print("PASS: recovered mapping is a hint; reused slot does not steal known unicast")
            packet = frame(A, BROADCAST, b"A validates its new slot")
            a.sendall(packet)
            expect(unrelated, packet)
            expect(b, packet)
            packet = frame(B, A, b"validated A")
            b.sendall(packet)
            expect(a, packet)
            quiet(unrelated, b)
            moved = rows()
            assert ["02:00:00:00:00:01", "3", "1"] in moved, moved
            # Test binary has active-MAC limit 1: stale recovered C on slot 3
            # must not block fresh A's legitimate source learning there.
            print("PASS: source relearning moves A, respects active security limit, restores unicast")
            restart()
        assert sorted(rows()) == sorted(moved), rows()
        print("PASS: second crash preserves the updated MAC move without recreating shared memory")

        assert "MAC table cleared" in command("clear", "mac")
        assert rows() == []
        restart()
        assert rows() == [] and workers()[-1][3] == "0", workers()
        print("PASS: clear mac commits before acknowledgment and stays empty after another crash")

        with ExitStack() as stack:
            d, peer = [connect(stack) for _ in range(2)]
            wait_for(lambda: active(2), "two admitted final clients")
            packet = frame(D, BROADCAST, b"after third restart")
            d.sendall(packet)
            expect(peer, packet)
            assert ["02:00:00:00:00:04", "1", "1"] in rows()
            d.sendall(b"x")
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=15) == 0, output()
        assert not any(path.exists() for path in (store_path, stats_path, Path(socket_path)))
        assert "ThreadSanitizer" not in output(), output()
        print("PASS: post-restart forwarding, partial-frame shutdown and complete supervisor IPC cleanup")
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                children = Path(f"/proc/{process.pid}/task/{process.pid}/children")
                if children.exists():
                    for child in children.read_text().split():
                        try:
                            os.kill(int(child), signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                process.kill()
                process.wait()
        print("--- supervisor/switch output ---")
        print(output(), end="")
        if "owner" in locals():
            event_log = Path(f"build/super-{owner}.log")
            if event_log.exists():
                print("--- last worker events ---")
                print("\n".join(event_log.read_text().splitlines()[-30:]))
