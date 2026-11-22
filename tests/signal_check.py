#!/usr/bin/env python3
import signal
import subprocess
import time
from pathlib import Path
Path("build").mkdir(exist_ok=True)
with open("build/signal-check.log", "w+") as log:
    process = subprocess.Popen(["./build/signal_control_check"], stdout=log, stderr=subprocess.STDOUT)
    def wait_for(text):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            log.flush()
            if text in Path(log.name).read_text():
                return
            assert process.poll() is None, Path(log.name).read_text()
            time.sleep(0.01)
        raise TimeoutError(text)
    try:
        wait_for("Send SIGHUP")
        process.send_signal(signal.SIGHUP)
        wait_for("PASS: SIGHUP")
        process.send_signal(signal.SIGUSR1)
        wait_for("PASS: SIGUSR1")
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=10) == 0
        print(Path(log.name).read_text(), end="")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
