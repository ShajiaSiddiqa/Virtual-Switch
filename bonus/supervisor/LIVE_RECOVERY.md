# Live switch recovery integration

The switch now attaches to the supervisor-owned VSWITCH_MAC_STORE when configured.
Ordinary runs without that environment variable keep the existing behavior.
This stage wires live state into the already tested store and restart engine.
The development-machine crash/reconnect checks passed normally and under TSan.
See the final [verification record](../../docs/VERIFICATION.md) for coverage.

## Build and normal checks

```bash
bash bonus/supervisor/build_recovery.sh
./build/mac_recovery_check
PORT=9300 bash tests/supervisor_recovery.sh
```

The unit check prints five PASS lines. The actual switch integration prints six
PASS lines and the supervisor output. It deliberately kills three worker processes
with SIGKILL; the supervisor must survive and replace each one. It also leaves
partial frames during crash/shutdown. Protocol-error diagnostics in those cases
are expected.

The production `./switch` retains the default five-second age and four-MAC limit.
`build/switch_recovery` is a test fixture: sixty-second aging avoids expiration
confounding the crash test, and an active-MAC limit of one exercises stale-hint
handling. Both binaries use the same source and persistence implementation.

## Memory and race checks

```bash
valgrind --leak-check=full --show-leak-kinds=all \
    --errors-for-leak-kinds=all --error-exitcode=99 ./build/mac_recovery_check

bash bonus/supervisor/build_recovery.sh tsan
setarch "$(uname -m)" -R \
    env TSAN_OPTIONS=halt_on_error=1 ./build/mac_recovery_check_tsan
setarch "$(uname -m)" -R \
    env TSAN_OPTIONS=halt_on_error=1 \
        SWITCH="$PWD/build/switch_recovery_tsan" PORT=9320 \
    bash tests/supervisor_recovery.sh
```

Do not run Valgrind around a TSan executable. Existing unit/function tests and
basic_forward.sh remain regression checks. Compile ordinary switch commands with
src/mac_store.c added; the supplied build scripts include it automatically.

## Real use after validation

```bash
./build/switch_supervisor ./switch 9300 3
```

Use the socket/statistics names printed by this supervisor with swctl. SIGINT to
the supervisor stops and reaps its worker before removing owned IPC resources.
SIGUSR1/SIGHUP to the supervisor are forwarded to its active worker.

## How live publication works

`MacTable` has an optional MacCheckpoint callback/context. Main attaches to the
shared object, reads its committed image, restores the empty process-local table,
then installs `persist_mac_image`. Configuration happens before worker/aging users.
The callback invokes mac_store_publish and does not call back into the hash table.

`mac_table_insert_vlan` updates source learning, timestamps, recency and moves. It
publishes before a successful return, including after capacity eviction. Forwarding
therefore does not enqueue a frame before its successful source-learning commit.

`mac_table_lookup_vlan` still uses the selected bucket mutex for lookup and LRU
update. It releases that bucket before requesting a checkpoint. A successful
lookup returns only after a coherent image containing its recency update (or a
newer concurrent state) has committed. Missing or unvalidated recovered entries
do not update LRU or select a forwarding destination.

`mac_table_expire`, `mac_table_remove_port` and `mac_table_clear` commit their
changes before returning success. Administrative clear therefore commits before
its successful UNIX-socket reply. Lookup never refreshes last_seen, so ordinary
aging semantics remain intact.

`checkpoint_locked` assumes the existing capacity mutex is held and no bucket is
held. It locks every bucket in ascending order, builds the complete pointer-free
image and publishes while those locks remain held. Capture and publication cannot
be delayed/reordered behind another mutation's newer image. It then releases the
buckets in reverse order. Public `mac_table_checkpoint` supplies the capacity lock
when called from lookup.

Without a callback, checkpoint helpers return immediately; no all-bucket checkpoint
locking or shared-memory publication occurs in the ordinary switch mode.

## Restore and stale connection safety

`mac_table_restore` validates capacity, keys, ports, timestamp conversion and LRU
bounds, allocates replacement nodes privately, then installs them into an empty
initialized table. Failure before installation frees private nodes and leaves the
table empty. It restores timestamps/sequence/recency rather than resetting every
entry's age to now. A restarted executable creates new process-local mutexes.

Each restored node is marked `recovered`. Such nodes remain visible to show mac,
aging and capacity eviction, but lookup treats them as unknown until source learning
validates that (VLAN, MAC) again. Unknown forwarding still uses normal VLAN-scoped
flooding; this is not a claim that an unknown destination gets confidential delivery.

This matters because old TCP connections are gone. If a different host reuses an
old slot, the stale mapping must not select only that unrelated connection as its
known destination. When the real MAC transmits again, learning updates its current
slot and clears recovered. Port-security counting excludes unvalidated hints.
Disconnect removal deletes validated mappings, leaving old unvalidated hints for
aging/eviction/clear rather than deleting them merely because a new connection
reused their historical slot.

VLAN, trunk, mirror, rate and administrative port policy are not restored by this
MAC image. They retain the existing startup defaults; reapply policy as needed.
The supervisor manages one standalone switch. Combining crash restart with the
separate two-switch pipe launcher is outside this integration.

## Lock order and performance tradeoff

Existing switch paths retain their order: admission where applicable; affected
transmit locks ascending; ports lock; MAC capacity lock; bucket locks ascending;
shared store mutex. Lookup releases its stripe before acquiring capacity. The
checkpoint callback must never reenter MacTable or acquire ports/transmit locks.
The parent never holds child heap-table locks and releases its store lock before
fork. Main keeps the attached mapping valid until aging and port users have joined.

Supervised mode adds synchronous whole-image capture/publication and serializes
that checkpoint phase. This is an intentional durability/performance tradeoff;
it is not the same throughput as ordinary striped lookup. The existing benchmark
measures its stated in-memory lock workload, not this supervised persistence path.
An indexed shared table or journal could reduce copy cost, at greater recovery
and transaction complexity. This implementation chooses bounded full images.

A crash can discard an in-progress operation that has not committed/returned.
Committed images survive worker crashes while the supervisor stays alive. This
is process-crash recovery, not power-loss/disk durability. A failed publication
returns failure; it must never be reported as a successful committed operation.

## Verification status

The development machine reported five recovery unit PASS checks, zero Memcheck
errors with all 18 allocations freed, and no TSan diagnostics for that unit.
Normal and TSan actual-switch tests each passed all six scenarios: three crashes,
retained mappings, reordered reconnect, safe unknown forwarding, revalidated host
move, persistent clear, new forwarding and final shutdown/IPC removal. The
supervisor exited with status 0 and three restarts.

The core store and supervisor engine tests also passed as recorded in their
separate stages. Latest ordinary-switch regressions passed for Tier 1 under
Memcheck, VLAN, mirror, port lifecycle, logging and two-switch forwarding; current
ordinary TSan checks passed forwarding/isolation, lifecycle and the pipe pair.

The killed workers do not have graceful Memcheck exit reports. Do not describe
the recovery unit Memcheck result as a full Memcheck shutdown of every killed
network worker. Tests establish their stated scenarios, not every crash timing.
The [technical report](../../docs/REPORT.md) documents synchronization costs and
known limitations, and [verification](../../docs/VERIFICATION.md) lists evidence.
