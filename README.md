# Virtual Switch

A multithreaded user-space Layer 2 switch written in C11 for an Operating Systems project. TCP clients represent hosts; a separate `swctl` process provides management through a UNIX socket and shared-memory statistics.

The implementation includes the core switch, advanced scheduling and administration, two switches linked by anonymous pipes, crash recovery under a supervisor, and a reproducible lock benchmark.

## Completion checklist

- [x] Fixed wire protocol, partial I/O, malformed/truncated-frame handling and SIGPIPE protection.
- [x] Bounded output queues, independent sender threads and counted queue rejections.
- [x] Reference-counted shared frames for flooding and mirroring.
- [x] VLAN-aware striped MAC table, aging, host moves, LRU eviction and port security.
- [x] Semaphore-based admission, timed waiting, rejection and port reuse.
- [x] Three priority queues, weighted round robin and queue aging.
- [x] Timer-refilled token buckets and condition-variable token waits.
- [x] Access VLAN isolation and one trunk port.
- [x] Dedicated ingress sniffer and nonblocking mirror enqueue.
- [x] Live port shutdown/up, VLAN changes, mirror changes and MAC clear.
- [x] `swctl`, process-shared statistics and one-second statistics watch.
- [x] Dedicated signal thread: shutdown, MAC-age reload and MAC dump.
- [x] Bounded asynchronous timestamped logger.
- [x] Two switch processes connected directly by two data pipes.
- [x] Supervisor restart with committed MAC state retained in shared memory.
- [x] Lock benchmark, target-machine measurements, graph and generated analysis.
- [x] Functional, memory and race checks for the scenarios listed in the verification record.
- [ ] Final review of course-specific submission format, tracked files and final commit.

Tests establish behavior for their scenarios; they do not prove that every possible schedule or hidden test is bug-free. See [verification](docs/VERIFICATION.md) and the [technical report](docs/REPORT.md).

## Requirements

Linux, GCC, Bash and Python 3 are required. The C programs use libc/POSIX APIs, `-pthread` and `-lrt`. Valgrind is used for memory checks and GCC ThreadSanitizer for race checks. Matplotlib is required only when regenerating benchmark graphs.

All shell commands below run from the repository root.

## Build

```bash
bash bonus/two_switches/build.sh
bash bonus/supervisor/build_recovery.sh
```

These build the ordinary `switch` and `swctl`, the pipe launcher, supervisor/probe and focused checks. Output binaries under `build/` are generated files. The build scripts explicitly list source files and use C11, warnings-as-errors, debug symbols and pthreads. There is no required Makefile target in this version.

Optional demonstration host:

```bash
gcc -std=c11 -Wall -Wextra -Werror -g -O0 -Iinc -pthread \
    src/host.c src/frame_io.c src/frame.c src/io.c -o host
```

## Quick start

In terminal 1:

```bash
./switch 9000 3
```

Arguments are `<tcp_port> <num_ports>`: TCP port 1–65535 and 1–64 logical ports. A quiet terminal is normal; runtime events go to `switch.log`. Stop with Ctrl+C.

In terminal 2:

```bash
./swctl show ports
./swctl show mac
./swctl stats
./swctl stats --watch
```

The optional host sends one VLAN-1 broadcast containing `hello`, then disconnects:

```bash
./host 127.0.0.1 9000 02:00:00:00:00:01
```

It is not an interactive send/receive CLI. The integration tests use persistent Python socket hosts and validate complete binary frames.

For an automated example that starts/stops its own switch:

```bash
bash tests/basic_forward.sh
```

## Administration

User-facing port numbers are one-based. The internal table uses zero-based slots.

| Command | Behavior |
| --- | --- |
| `./swctl show ports` | State, VLAN, access/trunk/sniffer mode and mirror destination |
| `./swctl show mac` | MAC, logical port and VLAN |
| `./swctl clear mac` | Clear the table; supervised mode commits before success reply |
| `./swctl shutdown 2` | Disable slot 2, cancel I/O, discard queued references and join its session |
| `./swctl up 2` | Enable admission to slot 2; the old TCP client must reconnect |
| `./swctl vlan 2 20` | Make slot 2 an access port in VLAN 20 |
| `./swctl trunk 4` | Select slot 4 as the sole trunk |
| `./swctl trunk 0` | Disable the configurable trunk |
| `./swctl mirror 1 3` | Mirror accepted ingress from slot 1 to dedicated sniffer slot 3 |
| `./swctl mirror 1 0` | Disable mirroring from slot 1 |
| `./swctl stats` | Snapshot counters directly from shared memory |
| `./swctl stats --watch` | Refresh the shared-memory counters every second; Ctrl+C stops watch |

An access port must send frames with its assigned VLAN. The trunk preserves the VLAN field. A sniffer can intentionally observe mirrored traffic across VLANs, is excluded from ordinary forwarding, and cannot inject ordinary traffic. Mirroring covers accepted ingress, not a second independent egress capture.

## Configuration and signals

The reload configuration is **one integer**, not `key=value` settings. `SIGHUP` changes MAC aging only. Startup uses the compiled default until reload.

```bash
printf '30\n' > vswitch.conf
kill -HUP <actual-switch-pid>
kill -USR1 <actual-switch-pid>
kill -INT <actual-switch-pid>
```

Replace the angle-bracket placeholder with one actual PID. SIGUSR1 writes `mac_dump.txt`; SIGINT requests coordinated shutdown. Do not target every process named `switch` when running multiple instances.

| Default | Value |
| --- | ---: |
| MAC age / sweep interval | 5 s / 1 s |
| MAC buckets / table capacity | 16 / 256 |
| Validated MAC/VLAN keys per port | 4 |
| Admission timeout | 2 s |
| Priority values | 0 high, 1 normal, 2 low |
| Queue capacity | 8 per priority, 24 per port |
| Weighted schedule / overdue threshold | 5:3:1 / 100 ms |
| Rate / initial burst | 1000 frames/s / 10 tokens |
| Refill cadence | approximately 10 ms |
| Logger capacity / message storage | 64 records / 256 bytes per message |

Queue-aging fairness requires the port's sender to make progress. The token bucket permits a burst; it is not a strict fixed-window counter.

Instance environment options must be set before workers start:

| Environment | Default/purpose |
| --- | --- |
| `VSWITCH_SOCKET` | `/tmp/vswitch.sock`; also used by `swctl` |
| `VSWITCH_STATS` | `/vswitch_stats`; also used by statistics commands |
| `SWITCH_LOG_PATH` | `switch.log` |
| `SWITCH_DUMP_PATH` | `mac_dump.txt` |
| `SWITCH_CONFIG_PATH` | `vswitch.conf` |
| `VSWITCH_MAC_STORE` | Optional supervisor-owned recovery object; normally set by supervisor |

`VSWITCH_NAME` alone does not configure the switch; shell tests may derive the two actual IPC settings from their own test identifier.

## Wire protocol

The header is 17 bytes followed by `length` payload bytes.

| Byte offset | Field | Bytes | Valid values |
| ---: | --- | ---: | --- |
| 0 | length | 2 | 0–1500; payload length only |
| 2 | source MAC | 6 | Six address bytes |
| 8 | destination MAC | 6 | Six address bytes |
| 14 | VLAN | 2 | 1–4094 |
| 16 | priority | 1 | 0–2 |

Length and VLAN are network byte order. Python tests use `struct.pack("!H6s6sHB", ...)`. TCP and pipes are byte streams; one read/write is not assumed to transfer a complete frame. See `frame.c`, `io.c`, `frame_io.c` and `pipe_link.c`.

## Bonus: two switches

```bash
./build/two_switches 9200 9201 3
```

Each process has two host slots and a reserved pipe trunk at port 3. Exactly two inter-process data pipes carry both directions; each process also has local wake pipes for cancellation. Fork/exec occurs before switch workers start.

The launcher prints distinct admin and statistics names. Use those actual values, for example:

```bash
VSWITCH_SOCKET=/tmp/vswitch-12345-a.sock ./swctl show mac
VSWITCH_STATS=/vswitch_12345_a_stats ./swctl stats
```

`12345` is an example launcher PID. The physical pipe trunk is fixed for that launch and cannot be disabled, reassigned or used as a mirror source. Ctrl+C to the launcher stops/reaps both switches. See [two-switch details](bonus/two_switches/README.md).

## Bonus: supervisor and MAC recovery

```bash
./build/switch_supervisor ./switch 9300 3
```

The parent retains a shared MAC recovery image, restarts abnormal worker exits and gives replacements the same object name. The default limit is five replacements, with 250 ms backoff and a five-second graceful-stop deadline. The supervisor prints the per-instance socket/statistics names. SIGINT/SIGTERM stops it; SIGHUP/SIGUSR1 forwards to its active worker.

Committed MAC/VLAN, slot, aging timestamp and LRU data survive a worker crash. TCP sessions and administrative policy do not. Recovered mappings are visible but treated as unknown destinations until source relearning validates them; this prevents stale known-unicast selection after slot reuse. Shared memory survives worker crashes while the parent lives, not machine power loss.

The supervisor manages a standalone switch. It is not combined with the two-switch pipe launcher. See [live recovery](bonus/supervisor/LIVE_RECOVERY.md).

## Bonus: benchmark

```bash
mkdir -p build
gcc -std=c11 -Wall -Wextra -Werror -O2 -g -pthread \
    bonus/benchmark/lock_bench.c -o build/lock_bench
python3 bonus/benchmark/check_benchmark.py
python3 bonus/benchmark/run_benchmark.py --ops 5000000 --repeats 3
```

Keep `results/benchmark/` tracked: `raw.csv`, `summary.csv`, `metadata.json`, `throughput.png`, `throughput.pdf` and `report.md`. The graph/report contain native target-machine measurements. This is a table-operation microbenchmark, not whole-switch packet throughput. It excludes lookup-side LRU mutation and the supervised checkpoint path. See [benchmark scope](bonus/benchmark/README.md).

## Verification

Do not leave a manual default-instance switch running when executing integration tests that use its default IPC names. Run those tests sequentially. Random TCP ports reduce but do not eliminate collisions.

```bash
bash tests/basic_forward.sh
TIER1_PORT=9400 python3 tests/tier1_verify.py \
    valgrind --leak-check=full --show-leak-kinds=all \
    --errors-for-leak-kinds=all --error-exitcode=99 ./switch
VLAN_TEST_PORT=9410 python3 tests/vlan_integration.py
MIRROR_TEST_PORT=9420 python3 tests/mirror_integration.py
PORT_ADMIN_TEST_PORT=9430 python3 tests/port_admin_integration.py
LOGGER_TEST_PORT=9440 python3 tests/logger_integration.py
TWO_SWITCH_TEST_PORT=9450 python3 tests/two_switch_integration.py
```

Recovery tests use a separate fixture binary with a 60-second MAC age and one-MAC active limit:

```bash
./build/mac_recovery_check
PORT=9300 bash tests/supervisor_recovery.sh
```

Separate ordinary TSan build:

```bash
gcc -std=c11 -Wall -Wextra -Werror -g -O0 -Iinc -pthread \
    -fsanitize=thread src/switch.c src/mac_store.c src/pipe_link.c \
    src/async_logger.c src/rate_limit.c src/signal_control.c src/control.c \
    src/frame_queue.c src/shared_frame.c src/frame_io.c src/frame.c \
    src/io.c src/mac_table.c src/shm_stats.c -lrt -o build/switch_tsan
setarch "$(uname -m)" -R env TSAN_OPTIONS=halt_on_error=1 \
    SWITCH="$PWD/build/switch_tsan" bash tests/basic_forward.sh
```

ASLR disabling is the workaround used for this machine's TSan mapping issue. It is not a requirement of switch operation. Never combine Valgrind with a TSan executable. Recovery-specific TSan commands are in `bonus/supervisor/LIVE_RECOVERY.md`.

## Source map

| Location | Responsibility |
| --- | --- |
| `inc/` | Public headers and data structures |
| `src/frame.c`, `src/io.c`, `src/frame_io.c` | Wire validation and complete transfers |
| `src/shared_frame.c`, `src/frame_queue.c` | Atomic ownership and bounded QoS queues |
| `src/mac_table.c`, `src/mac_store.c` | Live MAC hash table and crash-consistent recovery image |
| `src/rate_limit.c` | Per-session token bucket/refill worker |
| `src/control.c`, `src/swctl.c`, `src/shm_stats.c` | Management and shared telemetry |
| `src/signal_control.c`, `src/async_logger.c` | Synchronous signal worker and asynchronous logging |
| `src/pipe_link.c` | Cancellable two-pipe transport |
| `src/switch.c` | Main accept loop, port sessions, policy and lifecycle |
| `src/host.c` | Send-only demonstration client |
| `tests/` | Unit, shell and Python integration checks; keep tracked |
| `bonus/` | Benchmark, pipe launcher, supervisor and supporting documentation |
| `results/benchmark/` | Measured benchmark artifacts; keep tracked |
| `docs/` | Technical report and verification record |
| `build/` | Generated binaries/logs; ignore |

## Limitations

There is no Ethernet NIC/TAP integration, spanning-tree protocol, authentication for TCP hosts or interactive production host CLI. Reload changes MAC age only. The global ports lock and MAC capacity operations can serialize work; supervised whole-image checkpoints add further synchronization/copy cost. Mirror capture is ingress-only. Logger contention can drop records by design, counted separately from submission errors. Counter values describe their implemented events, not guaranteed end-to-end delivery.

Some administrative operations serialize with admission, which can be waiting up to its timeout. Some VLAN/mirror changes wait for affected senders' transmit locks; TCP writes have a timeout, while the fixed pipe trunk is excluded from incompatible reconfiguration. These bounds and restrictions are part of the implementation, not claims of zero-latency administration.
