# Two-switch bonus: standalone pipe transport

This stage adds transport only. It does not yet connect two switch processes.
Two data pipes carry A-to-B and B-to-A traffic. Each endpoint also has a local
wake pipe for cancellation; wake pipes do not carry frames.

## Ownership and operation

- Create the pair before fork and before creating threads. After fork, destroy
  the unused endpoint in each process. Otherwise inherited descriptors can
  prevent EOF and broken-pipe detection.
- Use one sender and one receiver per retained endpoint. Multiple senders could
  interleave frame bytes. One sender and one receiver can run concurrently.
- Ignore SIGPIPE before sending, as the existing switch already does.
- pipe_link_send serializes the existing 17-byte header and sends its payload.
  pipe_link_receive validates and reconstructs that same protocol. VLAN and
  priority are preserved. No shared heap pointers cross the process boundary.
- poll waits for readiness; nonblocking read/write loops preserve progress
  across partial transfers, EINTR and EAGAIN. A full pipe stalls its sender,
  so switch integration must retain the existing bounded queue/sender design.
- EOF between frames returns 0. EOF inside a header or payload returns -1 with
  EPROTO. A closed peer's read end produces EPIPE when SIGPIPE is ignored.
- pipe_link_stop atomically marks this endpoint stopped and makes its wake
  descriptor readable. Waiting operations return ECANCELED. Stop is permanent
  and idempotent. It affects the local endpoint, not an atomic shared across
  processes. Join all users before pipe_link_destroy closes descriptors.

## Functions

pipe_link_pair creates and configures the descriptors with cleanup on failure.
nonblocking preserves existing fcntl flags while adding O_NONBLOCK.
wait_ready polls the data descriptor together with the cancellation descriptor.
read_exact and write_exact retain an offset until transfer completes.
pipe_link_receive and pipe_link_send reuse frame_deserialize/frame_serialize.
pipe_link_stop wakes operations; pipe_link_destroy releases owned descriptors.

## Verification

The test covers two-process bidirectional maximum and zero-length frames,
fragmented header writes, VLAN/priority preservation, clean EOF, incomplete
headers/payloads, EPIPE, repeated stop, and cancellation with a full pipe.
The cancellation tests allow stop to race with entry into the operation;
they do not prove every possible thread schedule.

Normal build and AddressSanitizer/UndefinedBehaviorSanitizer passed during
preparation (LeakSanitizer disabled in the execution environment). Run Valgrind
locally before integration. Integration still needs distinct admin/shared-memory
names per switch, trunk lifecycle handling and end-to-end VLAN/MAC tests.
