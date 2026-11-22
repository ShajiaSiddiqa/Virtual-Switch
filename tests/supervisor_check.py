#!/usr/bin/env python3
"""Standalone engine tests. These use a recovery probe, not the switch yet."""
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile
import time

PREFIX = shlex.split(os.environ.get("SUPERVISOR_COMMAND", "./build/switch_supervisor"))


class Case:
    def __init__(self, mode="hold", extra=None, worker=None):
        self.temporary = tempfile.TemporaryDirectory(prefix="supervisor-check-", dir="build")
        self.directory = Path(self.temporary.name)
        self.report = self.directory / "probe.txt"
        self.log_path = self.directory / "supervisor.log"
        self.log = self.log_path.open("w+")
        env = dict(os.environ, SUPERVISOR_PROBE_LOG=str(self.report),
                   VSWITCH_RESTART_DELAY_MS="50", VSWITCH_SUPERVISOR_GRACE_MS="2000")
        if extra:
            env.update(extra)
        command = worker if worker is not None else ["./build/supervisor_probe", mode]
        self.process = subprocess.Popen([*PREFIX, *command], env=env,
                                        stdout=self.log, stderr=subprocess.STDOUT)

    def output(self):
        return self.log_path.read_text()

    def rows(self):
        if not self.report.exists():
            return []
        return [line.split() for line in self.report.read_text().splitlines()
                if len(line.split()) == 4]

    def wait(self, condition, description):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if condition():
                return
            assert self.process.poll() is None, self.output()
            time.sleep(0.02)
        raise TimeoutError(description + "\n" + self.output())

    def paths(self):
        found = re.search(r"SUPERVISOR pid=(\d+) store=(\S+) socket=(\S+) stats=(\S+)", self.output())
        assert found, self.output()
        _, store, socket, stats = found.groups()
        return [Path("/dev/shm") / store.lstrip("/"), Path(socket),
                Path("/dev/shm") / stats.lstrip("/")]

    def stopped(self, expected):
        result = self.process.wait(timeout=15)
        assert result == expected, self.output()
        assert not any(path.exists() for path in self.paths()), self.output()
        for row in self.rows():
            try:
                os.kill(int(row[0]), 0)
            except ProcessLookupError:
                continue
            raise AssertionError("Unreaped probe process " + row[0])

    def __enter__(self):
        return self

    def __exit__(self, exception_type, *_):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                children = Path(f"/proc/{self.process.pid}/task/{self.process.pid}/children")
                if children.exists():
                    for text in children.read_text().split():
                        try:
                            os.kill(int(text), signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                self.process.kill()
                self.process.wait()
        if exception_type is not None:
            print(self.output(), end="")
        self.log.close()
        self.temporary.cleanup()


Path("build").mkdir(exist_ok=True)
with Case() as case:
    case.wait(lambda: len(case.rows()) == 1, "initial worker")
    paths = case.paths()
    inode = paths[0].stat().st_ino
    first_pid = int(case.rows()[0][0])
    for forwarded in (signal.SIGHUP, signal.SIGUSR1):
        case.process.send_signal(forwarded)
        case.wait(lambda: f"SIGNAL {first_pid} {int(forwarded)}" in case.report.read_text(),
                  "forwarded control signal")
    for expected in (2, 3):
        old_pid = int(case.rows()[-1][0])
        os.kill(old_pid, signal.SIGKILL)
        case.wait(lambda: len(case.rows()) >= expected, "replacement worker")
        rows = case.rows()
        assert int(rows[-1][0]) != old_pid
        assert all(row[1:3] == ["1", "1"] for row in rows), rows
        assert len({row[3] for row in rows}) == 1, rows
        assert paths[0].stat().st_ino == inode, "MAC store was recreated"
    case.process.send_signal(signal.SIGINT)
    case.stopped(0)
print("PASS: two SIGKILL crashes restart fresh workers with the same committed MAC store")
print("PASS: SIGHUP and SIGUSR1 are forwarded to the active worker")
print("PASS: graceful supervisor stop reaps worker and removes owned IPC resources")

with Case(mode="clean-exit") as case:
    case.stopped(0)
    assert len(case.rows()) == 1, case.output()
print("PASS: successful worker exit does not restart")

with Case(mode="crash-loop", extra={"VSWITCH_RESTART_LIMIT": "2"}) as case:
    case.stopped(1)
    assert len(case.rows()) == 3, case.output()
    assert "restart limit reached" in case.output()
print("PASS: repeated abnormal exits stop at the configured restart limit")

with Case(worker=["/nonexistent-vswitch-worker-test"]) as case:
    case.stopped(1)
    assert case.output().count("WORKER pid=") == 1, case.output()
    assert "exec/startup failure" in case.output()
print("PASS: exec failure stops without a restart loop")

with Case(mode="ignore-stop", extra={"VSWITCH_SUPERVISOR_GRACE_MS": "300"}) as case:
    case.wait(lambda: len(case.rows()) == 1, "unresponsive worker")
    case.process.send_signal(signal.SIGTERM)
    case.stopped(1)
    assert "graceful stop timed out" in case.output(), case.output()
print("PASS: unresponsive worker is killed and reaped after the shutdown deadline")

with Case(mode="crash-loop", extra={"VSWITCH_RESTART_DELAY_MS": "60000"}) as case:
    case.wait(lambda: "restarting" in case.output(), "restart backoff")
    case.process.send_signal(signal.SIGINT)
    case.stopped(0)
    assert len(case.rows()) == 1, case.output()
print("PASS: shutdown during restart backoff prevents another worker from starting")
