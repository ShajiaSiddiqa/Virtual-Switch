# Host shutdown and recovery-test readiness fixes

- `src/host.c`: SIGINT/SIGTERM set the shutdown flag and write a byte to a nonblocking self-pipe. The input/network poll includes that pipe, preserving wakeups delivered immediately before the wait. The handler preserves errno; cleanup blocks these signals before closing the pipe descriptors.
- `tests/supervisor_integration.py`: wait until the admin listener reports all expected clients ACTIVE before sending test traffic. A successful TCP connect alone does not guarantee that the switch has admitted that client. The readiness check sends no data-plane frames, so restored MAC hints remain unvalidated for the recovery assertion.

Local checks: strict normal and TSan builds; 20 complete normal host suites and five complete TSan host suites passed; Python compilation and return-value audit passed. Valgrind is unavailable here; full switch integration cannot run here because UNIX listener creation is restricted. Run the targeted commands supplied with this patch on the project machine before claiming those checks passed.

Extract this archive from the project root. It replaces only the host source and recovery integration test and adds this note. It does not change the Makefile, benchmark results, switch forwarding, or persistent MAC store.
