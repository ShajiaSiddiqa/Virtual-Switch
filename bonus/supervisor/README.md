# Supervisor bonus — stage 2: restart engine

The shared-memory store is implemented and separately tested. This stage adds a
single-threaded parent that owns that store, starts a worker with fork/exec, reaps
it with waitpid and restarts abnormal exits. Tests use a dedicated recovery probe.

**Live switch integration is now supplied.** See [LIVE_RECOVERY.md](LIVE_RECOVERY.md)
for synchronous MAC publication, restart restoration, stale-slot safety and the
actual-switch crash/reconnect test. The actual-switch test passed normally and under TSan on the development machine;
the recovery unit also passed Memcheck. See the final
[verification record](../../docs/VERIFICATION.md) for exact coverage.

## Build and test

Run from the repository root after applying the MAC-store update:

```bash
bash bonus/supervisor/build_supervisor.sh
python3 tests/supervisor_check.py
```

Expect eight PASS messages. No TCP port is needed. The tests create their own
unique shared-memory names and temporary reports. SIGKILL is deliberately used
against two probe processes; clean replacement workers must attach to the same
object and verify the previously committed record, including timestamp/LRU data.

For parent-process Valgrind checking:

```bash
SUPERVISOR_COMMAND="valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=99 ./build/switch_supervisor" \
    python3 tests/supervisor_check.py
```

Expect all eight PASS messages. The suite includes deliberate failure cases;
those supervisors correctly return failure and the test asserts that behavior.
The Python runner reports unexpected exit codes or assertions and prints the
corresponding supervisor log. By default Valgrind checks the supervisor parent
and its pre-exec code, not the replacement executable after exec. The MAC-store
module's separate fork/exec tests remain part of validation.

## Files

- `switch_supervisor.c`: the restart engine, intended to supervise one executable
  directly, without a shell or wrapper that leaves an extra child running.
- `build_supervisor.sh`: builds the parent and the test-only worker probe.
- `../../tests/supervisor_probe.c`: attaches to the shared store, publishes a
  known record once, then verifies that it survives subsequent worker restarts.
- `../../tests/supervisor_check.py`: crash/restart, signal and lifecycle tests.
- `MAC_STORE.md`: the storage implementation and stage-1 verification procedure.

## Logic and function walkthrough

### main

1. Validate the executable arguments and optional bounded restart settings.
2. Block SIGINT, SIGTERM, SIGCHLD, SIGHUP and SIGUSR1. Reset their inherited
   dispositions to default; in particular an inherited ignored SIGCHLD could
   otherwise cause automatic child reaping and break waitpid.
3. Create a PID-specific shared MAC store once. Keep it mapped throughout all
   worker replacements. Never recreate or unlink it between worker crashes.
4. Read/validate the committed recovery image before each launch. This also
   repairs an abandoned robust mutex if necessary.
5. Fork. The child closes its inherited store handle, configures its environment,
   restores the original signal mask and calls execvp. A fresh executable must
   attach using the supplied name; it cannot rely on inherited heap addresses.
6. Wait for that exact worker PID. Reap it before cleaning its disposable admin
   and statistics names or starting a replacement.
7. A successful exit finishes normally. An abnormal exit restarts after a bounded
   delay, unless stopping or the restart limit has been reached. Exit 127 is
   reserved for exec/startup failure and is not retried.
8. After the final worker is reaped, remove disposable IPC, close the parent's
   mapping and explicitly unlink its MAC store.

### wait_worker

Uses waitpid(WNOHANG) to inspect the child and sigtimedwait to wait between checks.
SIGCHLD wakes the parent; it does not use an asynchronous signal handler.
SIGINT/SIGTERM requests graceful worker SIGINT once. If the deadline expires,
SIGKILL forces termination, the parent still reaps the worker, and the supervisor
returns failure rather than calling the forced stop clean.

SIGHUP and SIGUSR1 are forwarded to the currently unreaped worker PID. These
signals are not sent to a stale PID during restart backoff. Their actual switch
reload/dump behavior remains implemented by the switch's dedicated signal thread.

### backoff

Waits with sigtimedwait until a monotonic deadline. It is interruptible by shutdown,
so stopping during the delay prevents another worker from starting. This avoids
an unbounded rapid crash loop and avoids sleeping through an operator stop.

### child_environment

Sets VSWITCH_MAC_STORE, VSWITCH_SOCKET, VSWITCH_STATS, SWITCH_LOG_PATH,
SWITCH_DUMP_PATH, SWITCH_CONFIG_PATH and VSWITCH_RESTART_COUNT. IPC names remain
stable across this parent's worker restarts and differ from other supervisors.
It removes inherited pipe-link descriptor settings: this engine manages one
standalone switch, not the separate two-switch pipe launcher.

The parent is single-threaded, so fork is not performed while other parent threads
might hold allocator or application locks. Each fresh switch can start its own
threads after exec.

### remove_worker_ipc, setting, pending and now

`remove_worker_ipc` unlinks only this supervisor's disposable socket and statistics
names, after the old worker has exited. It does not unlink the MAC store.

`setting` uses strtol plus complete-input/range checks. `pending` wraps
sigtimedwait with a relative timeout. `now` reads CLOCK_MONOTONIC so wall-clock
adjustments cannot extend the shutdown deadline or change backoff duration.

## Settings and ownership

| Environment setting | Default | Allowed range |
| --- | ---: | ---: |
| VSWITCH_RESTART_LIMIT | 5 replacements | 0–100 |
| VSWITCH_RESTART_DELAY_MS | 250 ms | 10–60000 |
| VSWITCH_SUPERVISOR_GRACE_MS | 5000 ms | 100–60000 |

The restart limit applies over this supervisor's entire lifetime. It resets only
when a new supervisor starts. A clean worker exit does not consume a restart.

The printed SUPERVISOR line identifies its MAC store, admin socket and statistics
name. A WORKER line identifies each PID, restart count and committed image found
before that worker starts. Logs/config/dumps use `build/super-<parentPID>.*`.
The build directory must exist; the supplied build script creates it.

The parent owns the recovery object's lifetime. Worker close, exec and crash do
not unlink it. A supervisor shutdown removes it after the worker exits; surviving
a supervisor or machine crash is outside this stage's guarantee.

## Tests and their boundaries

1. Kill two real probe processes; verify distinct replacement PIDs, the same store
   name and inode, and the same committed generation/record after fresh exec.
2. Forward SIGHUP and SIGUSR1 to a live worker without restarting it.
3. Stop the parent normally; verify that the worker is reaped and IPC is removed.
4. Let a worker exit successfully; verify that no replacement starts.
5. Repeatedly exit abnormally; verify that the configured restart limit is enforced.
6. Fail exec; verify that no restart loop occurs.
7. Ignore graceful stop; verify deadline escalation, reaping and failure status.
8. Stop during restart backoff; verify no further worker is launched.

The probe uses a regular placeholder file for the admin path and a POSIX shared
memory placeholder for statistics. These verify ownership and crash cleanup;
they are not a substitute for testing a live UNIX socket and switch counters.

Passed locally: strict builds, all eight scenarios, and AddressSanitizer/
UndefinedBehaviorSanitizer checks on the supervisor with LeakSanitizer disabled.
Valgrind is unavailable in the delivery environment; run the supplied command on
Ubuntu. No claim is made yet about crash recovery of the live switch MAC table.

## Live switch integration

Follow [LIVE_RECOVERY.md](LIVE_RECOVERY.md) to build and test the integrated worker.
The eight engine/probe tests above remain useful independent lifecycle checks.

## Assignment-compliance update

Use the root Makefile for the course build, clean and tool targets. The host is
now a send/receive client. Mirroring includes accepted ingress and queued egress,
with one copy per sniffer. Checked failure-path helpers and updated tests require
fresh development-machine `make check`, `make valgrind-check` and `make tsan-check`.
Earlier results in these notes describe the preceding tested revision. The required
three-page report is `docs/SUBMISSION_REPORT.pdf`; longer notes are supplementary.
