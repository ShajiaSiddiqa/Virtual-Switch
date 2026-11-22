#!/usr/bin/env bash
# basic_forward.sh - tests unknown flooding, broadcast, learned unicast and no echo.
# Plan: A sends to unknown B (B/C receive); B announces (A/C receive);
# A sends to learned B (only B receives); a later FIFO marker checks A/C absence.
set -u
set -o pipefail
cd "$(dirname "$0")/.." || exit 1

SWITCH=${SWITCH:-./switch}
PORT=${PORT:-$((20000 + RANDOM % 20000))}
OUT=$(mktemp -d) || exit 1
VSWITCH_NAME="vsw_test_${PORT}_$$"
export VSWITCH_NAME
export VSWITCH_SOCKET="$OUT/control.sock"
export VSWITCH_STATS="/$VSWITCH_NAME"
export SWITCH_LOG_PATH="$OUT/events.log"
export SWITCH_DUMP_PATH="$OUT/mac_dump.txt"
export SWITCH_CONFIG_PATH="$OUT/switch.conf"
unset SWITCH_PIPE_READ_FD SWITCH_PIPE_WRITE_FD VSWITCH_MAC_STORE
printf '5\n' > "$SWITCH_CONFIG_PATH"
SW_PID=''
FAIL=0

finish() {
    local status=$? result=0 attempt
    trap - EXIT
    if [ -n "$SW_PID" ]; then
        if kill -0 "$SW_PID" 2>/dev/null; then
            kill -INT "$SW_PID" 2>/dev/null || true
            for ((attempt=0; attempt<200; attempt++)); do
                kill -0 "$SW_PID" 2>/dev/null || break
                sleep 0.05
            done
            if kill -0 "$SW_PID" 2>/dev/null; then
                echo 'FAIL: switch did not stop within 10 seconds'
                kill -KILL "$SW_PID" 2>/dev/null || true
                FAIL=1
            fi
        fi
        wait "$SW_PID" 2>/dev/null || result=$?
        if [ "$result" -ne 0 ]; then
            echo "FAIL: switch exited with status $result"
            FAIL=1
        fi
    fi
    # Check after shutdown so the complete diagnostic output is available.
    if grep -q 'ThreadSanitizer' "$OUT/switch.log" 2>/dev/null; then
        echo 'FAIL: ThreadSanitizer diagnostic'
        FAIL=1
    fi
    # A clean switch must remove the names it owned itself.
    if [ -e "$VSWITCH_SOCKET" ] || [ -e "/dev/shm/$VSWITCH_NAME" ]; then
        echo 'FAIL: switch left IPC resources behind'
        FAIL=1
    fi
    rm -f -- "$VSWITCH_SOCKET" "/dev/shm/$VSWITCH_NAME"
    if [ "$status" -ne 0 ] || [ "$FAIL" -ne 0 ]; then
        echo '--- switch stdout/stderr ---'
        cat "$OUT/switch.log" 2>/dev/null || true
        echo '--- switch event log ---'
        cat "$OUT/events.log" 2>/dev/null || true
        echo "FAIL: logs kept in $OUT"
        exit 1
    fi
    rm -rf -- "$OUT"
    echo 'basic_forward.sh: OK (unknown=2 recipients, broadcast=2, known=1, FIFO no-echo/isolation checks, clean shutdown)'
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Current switch CLI takes two arguments; it has no config-path argument.
"$SWITCH" "$PORT" 3 > "$OUT/switch.log" 2>&1 &
SW_PID=$!

# Current host is a send-only demo. Python supplies persistent binary-protocol hosts.
python3 - "$PORT" "$SW_PID" <<'PY'
import os
import select
import socket
import struct
import sys
import time
from contextlib import ExitStack

port, pid = map(int, sys.argv[1:])
A = bytes.fromhex("02000000000a")
B = bytes.fromhex("02000000000b")
C = bytes.fromhex("02000000000c")
BROADCAST = b"\xff" * 6


def frame(src, dst, text):
    return struct.pack("!H6s6sHB", len(text), src, dst, 1, 1) + text


def expect(peer, wanted):
    actual = bytearray()
    while len(actual) < len(wanted):
        part = peer.recv(len(wanted) - len(actual))
        assert part, "EOF before complete frame"
        actual.extend(part)
    assert actual == wanted, f"Wrong frame: {bytes(actual)!r}; expected {wanted!r}"


with ExitStack() as stack:
    # Retain this first successful connection as Host A: no readiness probe
    # is discarded, so startup checking does not consume a throwaway port.
    deadline = time.monotonic() + 10
    while not os.path.exists(os.environ["VSWITCH_SOCKET"]):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            raise RuntimeError("Switch exited during startup; see logs below") from None
        if time.monotonic() >= deadline:
            raise TimeoutError("Switch did not create its own admin socket within 10 seconds")
        time.sleep(0.05)
    while True:
        try:
            first = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            break
        except (ConnectionRefusedError, TimeoutError):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                raise RuntimeError("Switch exited during startup; see logs below") from None
            if time.monotonic() >= deadline:
                raise TimeoutError("Switch did not accept Host A within 10 seconds")
            time.sleep(0.05)
    a = stack.enter_context(first)
    b = stack.enter_context(socket.create_connection(("127.0.0.1", port), timeout=5))
    c = stack.enter_context(socket.create_connection(("127.0.0.1", port), timeout=5))
    for peer in (a, b, c):
        peer.settimeout(5)

    ready = frame(C, BROADCAST, b"ready")
    c.sendall(ready)
    expect(a, ready)
    expect(b, ready)

    unknown = frame(A, B, b"unknown hello")
    a.sendall(unknown)
    expect(b, unknown)
    expect(c, unknown)  # Also detects an incorrect earlier echo to C.

    announce = frame(B, BROADCAST, b"learn B")
    b.sendall(announce)
    expect(a, announce)  # Detects an incorrect earlier echo of A's unknown frame.
    expect(c, announce)

    known = frame(A, B, b"known hello")
    a.sendall(known)
    expect(b, known)  # Detects an incorrect earlier echo of B's announcement.

    # All frames use priority 1. Queue FIFO makes this a later marker on A/C:
    # an incorrectly forwarded 'known' frame would arrive before the marker.
    marker = frame(B, BROADCAST, b"after known unicast")
    b.sendall(marker)
    expect(a, marker)
    expect(c, marker)
    assert not select.select([a, b, c], [], [], 0.3)[0], "Unexpected extra frame or EOF"
PY
TEST_STATUS=$?
if [ "$TEST_STATUS" -ne 0 ]; then
    FAIL=1
fi
exit "$FAIL"
