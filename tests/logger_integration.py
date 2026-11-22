"""Run from the repository root, with no other switch instance running."""
import os
import shlex
import select
import signal
import socket
import struct
import subprocess
import time
import re
from pathlib import Path
from contextlib import ExitStack

PORT = int(os.environ.get("LOGGER_TEST_PORT", "9110"))

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
BROADCAST = b"\xff" * 6
os.makedirs("build", exist_ok=True)
log_path = Path(f"build/logger-integration-{PORT}.log")
log_path.unlink(missing_ok=True)
with open("build/logger-switch-output.log", "w+") as output:
    process = subprocess.Popen([*shlex.split(os.environ.get("SWITCH_COMMAND", os.environ.get("SWITCH", "./switch"))), str(PORT), "2"], stdout=output,
        stderr=subprocess.STDOUT,
        env={**os.environ, "SWITCH_LOG_PATH": str(log_path)})
    try:
        deadline=time.monotonic()+10
        while True:
            assert process.poll() is None, "Switch exited during startup"
            if os.path.exists("/tmp/vswitch.sock"):
                try:
                    if "PORT" in command("show","ports"): break
                except (AssertionError,OSError): pass
            assert time.monotonic()<deadline, "Startup timeout"
            time.sleep(.02)
        with ExitStack() as stack:
            a,b=[stack.enter_context(socket.create_connection(
                ("127.0.0.1",PORT),timeout=5)) for _ in range(2)]
            deadline=time.monotonic()+5
            while command("show","ports").count("ACTIVE")!=2:
                assert time.monotonic()<deadline
                time.sleep(.02)
            for i in range(20):
                f=packet(A,BROADCAST,1,i.to_bytes(4,"big"))
                source,target=(a,b) if i%2==0 else (b,a)
                source.sendall(f);exact(target,f)
            print("PASS: forwarding continues through 19 MAC moves")
            command("clear","mac")
            process.send_signal(signal.SIGUSR1)
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=10)==0
        text=log_path.read_text()
        lines=text.splitlines()
        assert lines, "No log records"
        pattern=r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z "
        assert all(re.match(pattern,line) for line in lines),text
        assert "MAC moved:" in text,text
        assert "VLAN 1 port" in text,text
        print("PASS: timestamped asynchronous MAC move records written")
        assert not os.path.exists("/tmp/vswitch.sock")
        assert not os.path.exists("/dev/shm/vswitch_stats")
        output.seek(0)
        result=output.read()
        assert "Logger:" in result,result
        print("PASS: logger drained/joined and switch resources removed")
        print(result,end="")
        print(f"Log file: {log_path}")
    finally:
        if process.poll() is None:
            process.kill();process.wait()
