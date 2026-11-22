#!/usr/bin/env bash
# supervisor_recovery.sh - tests actual switch restart and persistent MAC recovery.
# Plan: learn, kill, inspect; reconnect in a different order, relearn, kill again;
# clear, crash again, verify empty, then forward and stop with complete cleanup.
set -u
cd "$(dirname "$0")/.." || exit 1
export RECOVERY_SWITCH="${SWITCH:-./build/switch_recovery}"
export RECOVERY_TEST_PORT="${PORT:-$((20000 + RANDOM % 20000))}"
# The Python driver owns the supervisor PID and performs bounded cleanup in
# finally, including on KeyboardInterrupt. exec avoids another owning shell.
exec python3 tests/supervisor_integration.py
