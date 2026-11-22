#!/usr/bin/env python3
"""Check fork/exec, pipe ownership and signal forwarding without network listeners."""
import os
import pathlib
import signal
import subprocess
import tempfile
import time

pathlib.Path("build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="launcher-check-", dir="build") as temporary:
    directory = pathlib.Path(temporary).resolve()
    probe = directory / "probe.py"
    probe.write_text('''#!/usr/bin/env python3
import os, signal, time
from pathlib import Path
running = True
def stop(*_):
    global running
    running = False
signal.signal(signal.SIGINT, stop)
read_fd = int(os.environ["SWITCH_PIPE_READ_FD"])
write_fd = int(os.environ["SWITCH_PIPE_WRITE_FD"])
os.write(write_fd, b"X")
assert os.read(read_fd, 1) == b"X"
side = "a" if os.environ["VSWITCH_SOCKET"].endswith("-a.sock") else "b"
Path(os.environ["PROBE_DIRECTORY"], side).write_text(str(os.getpid()))
while running:
    time.sleep(0.01)
os.close(read_fd)
os.close(write_fd)
''')
    probe.chmod(0o700)
    env = dict(os.environ, VSWITCH_BINARY=str(probe), PROBE_DIRECTORY=str(directory))
    process = subprocess.Popen(["./build/two_switches", "9210", "9211", "3"], env=env)
    try:
        deadline = time.monotonic() + 5
        while not all((directory / side).exists() for side in "ab"):
            assert process.poll() is None, "Launcher exited early"
            if time.monotonic() > deadline:
                raise TimeoutError("Both exec children did not exchange pipe bytes")
            time.sleep(0.01)
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=12) == 0
        for side in "ab":
            pid = int((directory / side).read_text())
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                continue
            raise AssertionError("Child was not reaped")
        print("PASS: launcher fork/exec, bidirectional inherited pipes, SIGINT forwarding and reaping")
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            process.wait(timeout=12)
    failed = subprocess.run(["./build/two_switches", "9210", "9211", "3"],
                            env=dict(env, VSWITCH_BINARY="/nonexistent-vswitch-test"),
                            capture_output=True, text=True, timeout=12)
    assert failed.returncode != 0
    assert subprocess.run(["./build/two_switches", "0", "9211", "3"],
                          capture_output=True).returncode != 0
    print("PASS: exec failure and invalid arguments rejected")
