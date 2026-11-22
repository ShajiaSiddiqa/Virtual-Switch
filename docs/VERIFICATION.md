# Verification record

Recorded from development-machine outputs supplied on 22 November 2026, plus
focused source/unit checks during implementation. A PASS refers to the named
scenario/tool combination, not proof of absence of all defects.

## Final-code regression results

| Check | Evidence reported | Result |
| --- | --- | --- |
| `tests/basic_forward.sh` | Complete binary comparisons, unknown/broadcast/known forwarding, FIFO negative markers, cleanup | PASS normal and TSan |
| `tests/tier1_verify.py`, ports 9400/9401 | Forwarding/reuse; 6000 frames to fast C while B never reads | PASS under Memcheck; zero errors and no allocations remaining in both runs |
| Slow-port accounting, latest Memcheck run | B: 3920 rejected deliveries; C: 0 | Expected isolation behavior |
| `tests/vlan_integration.py`, port 9410 | VLAN isolation, access spoof rejection, VLAN-aware MAC key, live changes, cleanup | PASS normal |
| `tests/mirror_integration.py`, port 9420 | Cross-VLAN ingress capture, dedicated sniffer restrictions, 6000 fast-host deliveries | PASS normal; sniffer 4809 drops, normal ports zero |
| `tests/port_admin_integration.py`, port 9430 | Shutdown/up/reconnect, permits, unrelated traffic and cleanup | PASS normal |
| `tests/logger_integration.py`, port 9440 | 19 MAC moves, timestamped records, drain/join and cleanup | PASS normal |
| `tests/two_switch_integration.py`, ports 9450/9451 | Bidirectional VLAN forwarding, remote learning, stats, clear and cleanup | PASS normal |
| Current ordinary `build/switch_tsan`, ports 9460/9461 | Tier 1 forwarding/reuse and 6000-frame isolation | PASS; no TSan diagnostics |
| Current ordinary TSan, port 9470 | Port lifecycle integration | PASS; no TSan diagnostics |
| Current ordinary TSan, ports 9480/9481 | Actual two-switch integration | PASS; no TSan diagnostics |

For TSan slow-host testing, B recorded 4018 rejected deliveries and C zero. Exact
slow-port counts vary with scheduling and socket buffering; the required relation
is positive slow rejection with successful fast delivery. Small logger-drop counts
of one appeared in some TSan runs; submission errors remained zero. These are
allowed by the trylock/full-drop logging policy.

## Recovery results

| Check | Coverage | Result |
| --- | --- | --- |
| `mac_store_check` | Close/reattach/fresh exec, invalid images, owner killed before/after commit, 200 publications, explicit unlink | Six PASS checks; reported Memcheck zero errors/leaks |
| `supervisor_check.py` | Two forced probe crashes, signal forwarding, clean exit, crash limit, exec failure, forced stop, backoff stop | Eight PASS checks normal and with supervisor Memcheck wrapper |
| `mac_recovery_check` | Live learn/move/LRU/evict, restore metadata/hints, expire/remove/clear, concurrent publication, failure ownership | Five PASS checks; 18 allocations/frees, zero remaining bytes, Memcheck zero errors |
| `mac_recovery_check_tsan` | Same recovery unit scenarios | Five PASS checks; no TSan diagnostics |
| Actual switch recovery, port 9300 | Three worker SIGKILL events, retained rows, reordered reconnect, move, clear, final forwarding/cleanup | Six PASS checks; supervisor status 0, three restarts |
| Actual switch recovery under TSan, port 9320 | Same live crash/reconnect scenarios | Six PASS checks; no TSan diagnostics; supervisor status 0, three restarts |

The reported recovery generations were 0 initially, 5 with three entries after
the first crash, 9 with three entries after the move/crash, and 10 with zero after
clear/crash. These are observations from that test, not fixed API generation values
for arbitrary workloads. The sixty-second age/one-active-MAC recovery fixture is
separate from the ordinary switch build.

Forced SIGKILL tests cannot produce a normal memory-leak shutdown report for the
killed worker itself. The recovery unit Memcheck result and live TSan result must
not be represented as a full graceful worker Memcheck run of every killed process.
The supervisor wrapper checks the parent/pre-exec path by default; it does not
instrument the replacement executable after exec without additional configuration.

## Earlier focused checks

Reported passing checks include queue capacity/FIFO/wrap/close; QoS weighting,
overdue promotion and ownership; limiter bounds/close; shared counter increments
across two processes; fragmented/oversized control commands; signal callbacks;
MAC snapshot/clear/VLAN behavior; asynchronous logger accounting/drain; pipe
framing/EOF/EPIPE/cancellation; and forked real-port-session shutdown. Relevant
Memcheck runs reported zero errors/leaks. These records supplement the final-code
regressions; they are not a claim that every focused test was rerun after every
subsequent edit.

## Benchmark evidence

`lock_bench` correctness checks passed across policies and invalid arguments.
The reported striped four-thread Memcheck sample had matching 20028 writes/updates
and zero errors/leaks. Native final measurements used:

```bash
python3 bonus/benchmark/run_benchmark.py --ops 5000000 --repeats 3
```

No short-trial warning was reported in that final run. Preserve all six files in
results/benchmark. Do not replace these native results with sanitizer measurements
or implementation-environment smoke results.

## Reproduction and interpretation

Use the root README for ordinary build/test commands and the bonus READMEs for
specialized builds. Run default-instance integrations sequentially, with no manual
switch holding their socket/statistics names. Use distinct free TCP ports. Run
one diagnostic tool per executable; never run a TSan binary under Valgrind.

A protocol-error log from an intentionally interrupted header/payload, a signal-9
restart from deliberate fault injection, or a counted slow/sniffer/logger drop is
not automatically a failed test. Unexpected exit status, mismatched frames,
missing expected traffic, stalled fast hosts, leaked resources, Memcheck errors or
TSan diagnostics are failures.

No new C source was changed by the final documentation package. If source changes
after these results, rerun the affected unit and integration checks before using
this record as final evidence. Course-specific public/hidden tests and submission
format review remain separate final checks.
