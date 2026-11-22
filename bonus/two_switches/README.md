# Two switches linked by two anonymous pipes

This bonus uses two independent switch processes and exactly two inter-process
data pipes: A-to-B and B-to-A. Local cancellation pipes still wake each process's
own workers; they are not additional links between switches.

## Build and verify

Run from the repository root:

```bash
bash bonus/two_switches/build.sh
./build/pipe_link_check
./build/stats_instance_check
./build/two_switch_pipe_check
python3 tests/two_switch_launcher_check.py
TWO_SWITCH_TEST_PORT=9200 python3 tests/two_switch_integration.py
```

The final command needs free TCP ports 9200 and 9201. Override the base port if
needed. It starts and stops the launcher itself; do not start a manual pair first.
It uses PID-specific admin/statistics names and does not remove another instance's
socket. The test intentionally leaves a partial frame during shutdown, so a
protocol-error diagnostic in a log is expected.

For memory checking:

```bash
valgrind --leak-check=full --show-leak-kinds=all \
    --errors-for-leak-kinds=all --error-exitcode=99 \
    ./build/two_switch_pipe_check

valgrind --leak-check=full --show-leak-kinds=all \
    --errors-for-leak-kinds=all --error-exitcode=99 \
    ./build/stats_instance_check
```

These check the forked real port sessions and statistics ownership. They do not
replace a full launcher/admin integration run.

## Manual operation

```bash
./build/two_switches 9200 9201 3
```

Each switch has three total ports: two TCP host slots and a reserved pipe trunk
at port 3. The launcher prints both child PIDs, UNIX socket paths and shared-memory
names. Both switches initially put access ports in VLAN 1.

Use the actual paths printed by your launcher, for example:

```bash
VSWITCH_SOCKET=/tmp/vswitch-12345-a.sock ./swctl show ports
VSWITCH_SOCKET=/tmp/vswitch-12345-a.sock ./swctl vlan 1 10
VSWITCH_SOCKET=/tmp/vswitch-12345-b.sock ./swctl vlan 1 10
VSWITCH_SOCKET=/tmp/vswitch-12345-a.sock ./swctl show mac
VSWITCH_STATS=/vswitch_12345_a_stats ./swctl stats --watch
```

12345 above is an example launcher PID, not a fixed identifier. SIGINT to the
launcher stops and reaps both children. Do not use SIGKILL for normal shutdown.
Logs are `build/two-<launcherPID>-a.log` and the corresponding `b` file. MAC dumps
and reload configurations also use per-instance paths printed by this pattern:
`build/two-<launcherPID>-a-mac.txt` and `build/two-<launcherPID>-a.conf`.
Send SIGUSR1/SIGHUP to the relevant child PID, not the launcher. To reload its
aging setting, write the desired seconds to that child's configuration file first.

## Implementation and function walkthrough

### Launcher: bonus/two_switches/two_switches.c

`number` uses strtol plus end/range checks to reject invalid arguments. `main`
blocks SIGINT, SIGTERM and SIGCHLD before fork. `pipe(ab)` and `pipe(ba)` create
both directions. Fork occurs before any worker threads exist.

`child` keeps exactly one data read end and one data write end, closes the other
two, sets the per-instance environment, restores the signal mask, and execvp runs
the ordinary switch executable. exec creates fresh switch state; heap pointers
are never passed to another process. The parent closes all four data descriptors
after forking. Leaving any unnecessary write end open would prevent EOF detection.

The parent uses waitpid(WNOHANG) and sigtimedwait to coordinate termination.
SIGINT/SIGTERM forwards graceful SIGINT to both children. An unexpected child exit
stops the other switch and produces failure status. After a ten-second grace
period it escalates to SIGKILL and reports failure. Only after both children are
reaped does it remove any IPC names left by this launch. It does not restart
crashed switches; that is the separate supervisor bonus.

### Descriptor adoption: inc/pipe_link.h and src/pipe_link.c

`pipe_link_adopt` checks that the inherited descriptors are distinct FIFO ends,
with a read-only input and write-only output belonging to different pipes.
fstat inspects object type/identity; fcntl checks access flags and sets nonblocking
mode. A new local wake pipe allows cancellation. Ownership transfers on success.

`pipe_link_send` serializes the existing wire header, then sends its payload.
`pipe_link_receive` reconstructs and validates the same header/payload. The helper
loops retain offsets across partial operations and EAGAIN; poll waits for data,
space or cancellation. EINTR retries. SIGPIPE remains ignored by switch main,
so a closed peer produces an EPIPE error instead of process termination.

`pipe_link_stop` sets an atomic flag and writes a wake byte. Both receiver and
sender can wake because that byte stays readable. Destroy runs only after all
users have joined, preventing close/reused-descriptor races.

### Port integration: src/switch.c

`Port.pipe_link` is NULL for existing TCP ports. A non-NULL pointer identifies the
reserved physical trunk and stays fixed throughout the launch.

`main` validates SWITCH_PIPE_READ_FD/SWITCH_PIPE_WRITE_FD and adopts the inherited
ends before starting workers. `serve_hosts` reserves the last port, sets its trunk
policy and starts its normal port session. Its semaphore initially counts only
TCP slots. `admit_client` explicitly skips the reserved slot, even after link loss.

`receiver_thread` chooses pipe_link_receive for the pipe port and frame_read for
TCP. Both feed the same SharedFrame creation and forward_frame path.
`forward_frame_locked` therefore learns remote source MACs against the pipe trunk,
using the existing (VLAN, MAC) key. Flooding excludes its ingress port, so a frame
arriving from the other switch is not reflected back down the trunk.

`sender_thread` retains the same QoS queue and token bucket. It chooses
pipe_link_send for trunk output. A frozen peer may stall this sender, but receiving
threads still use bounded, nonblocking enqueue and count rejected deliveries.
`stop_port_io` cancels a PipeLink instead of calling socket-only shutdown on a
pipe. `port_session` joins its sender and releases queued references as before.
The reserved slot never contributes a free-host semaphore permit. Main closes
its adopted data/wake descriptors after sessions finish.

The pipe trunk is fixed for one launch: VLAN reassignment, shutdown/up, disabling
or moving the trunk, and mirroring the pipe ingress are rejected. Existing TCP
port management remains available. A failed pipe link requires restarting the
pair; hot reconnection and spanning-tree topologies are outside this bonus.
Existing per-port MAC-security limits also apply to the trunk; increase the
compile-time limit deliberately if a larger lab topology needs more remote MACs.

### Instance isolation: control, shared statistics and swctl

`control_socket_path` reads VSWITCH_SOCKET with the old path as the default.
ControlServer copies the chosen path before starting its worker and unlinks that
stored path on cleanup. `swctl` uses the same override to select an instance.

StatsHandle stores the chosen VSWITCH_STATS name. Create/attach validate and open
it; owner cleanup unlinks that stored name, even if the environment later changes.
The shared structure/counter layout and process-shared mutex are unchanged.
All environment options must be configured before creating worker threads.

No additional fast-path global lock was introduced. The existing ports, queue,
rate-limiter, transmit and MAC lock ordering remains applicable. Pipe cancellation
requires no transmit lock; shutdown can wake a sender stalled in poll.

## Verification status

Passed on the Ubuntu development machine:

- Standalone pipe transport and inherited-descriptor adoption.
- Independent shared-memory statistics and owner-specific cleanup.
- Two-process forwarding, VLAN isolation, and VLAN-specific MAC learning.
- Cross-switch unicast with preserved VLAN, priority, and payload.
- Launcher fork/exec, signal forwarding, child reaping, and failure handling.
- Full TCP/admin integration and IPC resource cleanup.
- Valgrind: zero errors and zero remaining allocations in the parent
  and both child processes of two_switch_pipe_check.
- ThreadSanitizer: full two-switch integration passed without race warnings.

The partial-frame shutdown test intentionally produces a protocol-error
diagnostic. The asynchronous logger may drop messages under contention
according to its nonblocking policy.

These results cover the tested scenarios; they do not prove the absence
of every possible concurrency defect.

The supervisor and persistent-MAC integration are separate bonus wor

A socketpair would provide a bidirectional descriptor and socket shutdown, but the
assignment explicitly requires two pipes. A TCP relay could reuse frame_io but
would add adapters and obscure the requested direct pipe connection. This design
adds transport selection at the port boundary and keeps forwarding policy common.

Reference counts remain process-local. Frames crossing the link are serialized
bytes and reconstructed on receipt; sharing heap addresses between the processes
would be invalid. Broadcast copies within either switch still share one allocation.

## Verification status at delivery

Passed locally: strict compilation; pipe transport/adoption; forked two-switch
real-port-session forwarding and shutdown; independent shared-memory instances;
launcher fork/exec, pipe ownership, signal forwarding and reaping; existing QoS,
VLAN, mirror, shared statistics and port lifecycle checks. AddressSanitizer and
UndefinedBehaviorSanitizer passed the forked session test with LeakSanitizer disabled.

The delivery environment forbids network listeners and has no Valgrind installed.
The full TCP/admin integration test and local Valgrind runs are supplied for the
Ubuntu development machine. No claim of complete validation is made until those
pass. Keep the prior Tier 1/Tier 2 regression tests in the repository.
