# Assignment-compliance update

This package addresses the gaps found when the original assignment PDF was read.
It replaces the send-only host, supplies the required Makefile and a three-page
report, extends capture to queued egress, and adds checked failure-path helpers.

## Apply safely

From the repository root, back up the working tree (excluding generated data and
Git history) before extracting. Preserve results/benchmark: this package does not
contain or replace the development machine's measured results.

```bash
tar --exclude='./build' --exclude='./.git' --exclude='*.tar.gz' \
    -czf ../virtual-switch-before-compliance.tar.gz .
tar -xzf virtual-switch-compliance-update.tar.gz
make clean
make -j4
make check
make valgrind-check
setarch "$(uname -m)" -R make tsan-check
```

The ASLR workaround is only needed on machines with the known TSan mapping issue.
Never run Valgrind on a TSan executable. Do not leave another default-instance
switch running while these integration targets execute. Diagnostic logs remain
in build; make clean intentionally removes that generated directory. Benchmark
measurements in results/benchmark survive clean.

## What changed

- Makefile: course-flags default build of programs/checks/demos/fixtures, clean,
  unit and integration tests, Valgrind and separate TSan builds, native benchmark
  target and return-value audit.
- host.c: same required positional CLI, persistent send/receive with poll, send,
  sendp, recv-total-count, help and quit; complete binary-frame reconstruction,
  escaped output, bounded commands and clean stdin/signal shutdown.
- switch.c: mirror copies for accepted ingress and accepted ordinary egress queues;
  one copy per sniffer per frame, no recursive mirroring. Existing tests updated.
- checked.h and call sites: checked pthread/POSIX/formatting/cleanup results.
  Recoverable errors retain explicit module handling; unsafe cleanup failures
  fail fast. The lexical audit is an additional regression guard, not a semantic
  proof about all possible C code.
- Tests: real host protocol peer, automated signals, full-table aging accounting,
  updated mirror coverage, consistent executable overrides and a retained Tier 1
  driver. Manual demos are compiled but not run as unattended tests.
- Documentation: corrected README, actual file tree, GitHub Mermaid diagrams,
  editable DOT/SVG/PNG thread diagram and required three-page submission PDF.
  Longer implementation notes remain supplementary.

## Verification at package creation

Passed in the preparation environment: clean make all/tests; 17 C unit/function
executables with 56 PASS records; automatic signal checks; five host-peer cases
normally and under TSan; updated mirror-unit cases under TSan; supervisor engine,
pipe-launcher and benchmark correctness checks; return audit; Python/Bash syntax;
three-page PDF link/layout review. The earlier aging count omission was reproduced
in an isolated generated copy and the fixed regression passes.

The environment blocks creating the UNIX listener, and Valgrind is not installed.
No new full switch-network or Memcheck PASS is claimed for this revision. Earlier
network/tool results in VERIFICATION.md precede these source changes. Run all three
Makefile suites above on Ubuntu and record their outcomes. Official public/hidden
course tests remain separate from our own test suite.

## Submission

Use docs/SUBMISSION_REPORT.pdf as the required report. It contains the thread
layout/roles, lock table/order, real aging-count bug and completion summary. Review
student/team identification requirements separately; the assignment supplied no
required cover-page identity fields. Keep tests, source, Makefile, docs, bonus and
measured benchmark artifacts tracked. Ignore build output, binaries and delivery
archives. README and maintained bonus docs supersede old standalone delivery notes.
