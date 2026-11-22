# Supervisor bonus — stage 1: recoverable shared-memory MAC image

This stage is standalone. It does not yet restart the switch or persist its live
MAC mutations. It adds the recovery storage that the supervisor will own.
Existing switch, MAC table, queues and two-switch launcher are unchanged.

## Why shared memory alone is insufficient

Heap-linked MacEntry objects cannot be recovered by a fresh executable: their
pointers refer to the old process's address space. A normal process-shared mutex
can also remain locked forever if its owner dies. Finally, even a robust mutex
cannot undo an interrupted in-place table copy.

The store therefore uses plain-value records, a process-shared robust mutex, and
two complete image slots. Records preserve (MAC, VLAN), zero-based port, monotonic
last-seen timestamp and LRU recency. The image also preserves the LRU sequence.
It contains no heap pointers, sockets, thread IDs or linked-list addresses.

## Publication logic

1. Validate the caller's complete image, including duplicate (VLAN, MAC) keys,
   bounded capacity, port/VLAN ranges, timestamps and LRU sequence bounds.
2. Acquire the robust process-shared mutex.
3. Copy the new image into the inactive slot. The committed slot is untouched.
4. Assign the next generation and atomically publish the new slot index.
5. Unlock.

If the writer dies before step 4, the old committed image remains intact. If it
dies after step 4 but before unlocking, the new complete image remains committed.
The next locker receives EOWNERDEAD, validates the committed image and calls
pthread_mutex_consistent. It then continues normally. Invalid committed data
fails closed rather than silently resetting a table.

Readers use the same mutex. A release store publishes the slot after its contents
are written; acquire loads observe the published index. Lock-free integer atomics
are required so no process-local fallback atomic lock can be used for that index.

## Files and functions

- `inc/mac_store.h`: pointer-free record/image types and public API. MacStore is
  a process-local mapping handle; its pointer is never stored inside shared memory.
- `src/mac_store.c`: private SharedMacStore layout, object creation/attachment,
  robust locking, validation, double-buffer publication and cleanup.
- `tests/mac_store_check.c`: persistence across a fresh exec, rejected writes,
  deterministic owner-death faults and multiple publisher processes.
- `bonus/supervisor/build_store.sh`: strict standalone test build.

### Public API

`mac_store_create(store, name)` uses shm_open with O_CREAT|O_EXCL so it cannot
replace an existing object's contents. ftruncate reserves the required size and
mmap(MAP_SHARED) maps the same pages into every attached process. Creation sets
PTHREAD_PROCESS_SHARED and PTHREAD_MUTEX_ROBUST before initializing the mutex.
The object is marked ready only after initialization finishes.

`mac_store_attach(store, name)` opens the initialized object and verifies its size,
version and image layout before using its mutex. It can recover an abandoned
mutex during attachment. Call only after the creator reports successful creation;
this module does not implement a separate startup handshake.

`mac_store_publish(store, entries, count, use_sequence)` commits one whole image.
The input array must remain unchanged during the call. A zero-entry image commits
a clear. Invalid input leaves the committed image unchanged. Generation exhaustion
returns EOVERFLOW instead of wrapping.

`mac_store_read(store, image)` returns a complete, consistent image including its
generation. Each generation describes a publication, not the chronological
freshness of independently captured snapshots. Integration must serialize/order
live table mutations and their publication; publishing an old snapshot later
would still overwrite a newer one. That ordering is not provided by this module.

`mac_store_close(store)` unmaps and closes only this process's handle. It deliberately
does not unlink or destroy the shared mutex: another process may still use it.

`mac_store_unlink(name)` removes the shared-memory name. The future supervisor
will call it only after all workers are reaped and attached users have stopped.
Unlinking does not immediately invalidate already mapped pages.

### Internal helpers

`valid_name` enforces a single POSIX shared-memory name. `valid_entries` checks
bounds before accessing the array, rejects duplicate keys and validates metadata.
Its duplicate detection is O(K^2), bounded by 256 records; this initial version
prioritizes straightforward validation. It is not a fast-path lookup benchmark.

`valid_committed` examines only the selected slot, because the inactive slot may
contain an interrupted update. `lock_store` handles EOWNERDEAD and recovery;
`unlock_store` converts pthread error numbers to the module's -1/errno convention.
`get_shared` rejects invalid handles and `map_store` centralizes mmap setup.

The test-only `mac_store_test_hold` is compiled solely with MAC_STORE_TESTING.
It damages an inactive copy or commits a new valid copy, writes a notification,
then holds the mutex. The parent kills that child only after the notification,
so fault timing is controlled. Production compilation excludes this function.

## Tests

```bash
bash bonus/supervisor/build_store.sh
./build/mac_store_check

valgrind --leak-check=full --show-leak-kinds=all \
    --errors-for-leak-kinds=all --error-exitcode=99 \
    ./build/mac_store_check
```

Expect six PASS lines and zero parent-process memory errors/leaks. Two child
processes are deliberately SIGKILLed to test abandoned locks. Their forced death
is the injected fault, not a failure. Normal test children close their handles.
The parent explicitly unlinks the unique test object when finished.

Locally passed: strict production/test builds, all six checks and AddressSanitizer/
UndefinedBehaviorSanitizer checks (LeakSanitizer disabled in the execution environment).
Valgrind must be run on the Ubuntu machine; it is unavailable in this environment.

## Tradeoffs and scope

Two slots use twice the image memory, but require no journal replay or dynamic
allocation recovery. A robust mutex alone would leave partial in-place data.
A write-ahead journal is another approach but adds replay, truncation and ordering
rules. A disk file can survive machine restarts, which this POSIX shared-memory
stage does not promise. These guarantees concern process crashes on the same boot.

The store uses one lock for recovery-image operations. It does not replace the
existing striped live MAC hash table. No existing fast-path lock was changed.

## Integrated worker

The supervisor engine and live-switch integration are now implemented. See
[LIVE_RECOVERY.md](LIVE_RECOVERY.md) for synchronous publication of learning,
move, lookup LRU, aging, eviction, removal and clear, and restoration with
unvalidated recovery hints after fresh exec. TCP sessions and administrative
policy are rebuilt separately; recovered slots are not treated as live identity.

The actual switch passed three-crash/reconnect scenarios normally and under TSan.
See the root [verification record](../../docs/VERIFICATION.md). The stage-1 store
checks above remain useful independent fault-injection tests. A periodic checkpoint
alone is not the implementation's guarantee; committed live mutations are
published synchronously in the enabled supervised path.
