"""Measure, retain raw trials, and generate a graph/report; run from project root."""
import argparse
import csv
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
from datetime import datetime, timezone

parser = argparse.ArgumentParser()
parser.add_argument("--binary", default="./build/lock_bench")
parser.add_argument("--threads", nargs="+", type=int, default=[1, 2, 4, 8])
parser.add_argument("--reads", nargs="+", type=int, default=[80, 50])
parser.add_argument("--hot", type=int, default=0)
parser.add_argument("--ops", type=int, default=500000)
parser.add_argument("--repeats", type=int, default=3)
parser.add_argument("--out", type=Path, default=Path("results/benchmark"))
args = parser.parse_args()
if (any(t < 1 or t > 64 for t in args.threads) or
    any(r < 0 or r > 100 for r in args.reads) or
    not 0 <= args.hot <= 100 or not 1 <= args.ops <= 100000000 or args.repeats < 2):
    parser.error("Invalid range: threads 1..64, percentages 0..100, ops 1..100000000, repeats >=2")
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    parser.exit(1, "Install matplotlib in a virtual environment before running this script.\n")

args.out.mkdir(parents=True, exist_ok=True)
metadata = {
    "created_utc": datetime.now(timezone.utc).isoformat(),
    "platform": platform.platform(),
    "machine": platform.machine(),
    "python": sys.version,
    "logical_cpus": os.cpu_count(),
    "affinity_cpus": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
    "parameters": {key: str(value) if isinstance(value, Path) else value
                   for key, value in vars(args).items()},
    "compile_command": "gcc -std=c11 -Wall -Wextra -Werror -O2 -g -pthread bonus/benchmark/lock_bench.c -o build/lock_bench",
}
try:
    metadata["compiler"] = subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0]
except (OSError, subprocess.SubprocessError):
    metadata["compiler"] = "unknown"
try:
    metadata["cpu_quota"] = Path("/sys/fs/cgroup/cpu.max").read_text().strip()
except OSError:
    pass
(args.out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

modes = ["global", "striped", "rwlock"]
rows = []
order = random.Random(42)
# Warm each policy/thread/workload combination outside reported measurements.
for reads in args.reads:
    for threads in args.threads:
        for mode in modes:
            subprocess.run([args.binary, mode, str(threads), "10000", str(reads), str(args.hot), "0"],
                           check=True, capture_output=True, text=True, timeout=120)

with (args.out / "raw.csv").open("w", newline="") as raw:
    writer = None
    for reads in args.reads:
        for threads in args.threads:
            for repeat in range(args.repeats):
                policies = modes.copy()
                order.shuffle(policies)
                seed = 1000 + repeat
                for mode in policies:
                    command = [args.binary, mode, str(threads), str(args.ops),
                               str(reads), str(args.hot), str(seed)]
                    result = subprocess.run(command, check=True, capture_output=True,
                                            text=True, timeout=600)
                    row = json.loads(result.stdout)
                    row["repeat"] = repeat + 1
                    if row["writes"] != row["updates"]:
                        raise RuntimeError("Lost updates; timing result rejected")
                    if writer is None:
                        writer = csv.DictWriter(raw, fieldnames=list(row))
                        writer.writeheader()
                    writer.writerow(row)
                    raw.flush()
                    rows.append(row)
                    print(f"reads={reads}% threads={threads} {mode:7s} "
                          f"trial={repeat+1}: {row['ops_per_second']/1e6:.3f} Mops/s", flush=True)

summary = []
for reads in args.reads:
    for threads in args.threads:
        for mode in modes:
            values = [r["ops_per_second"] for r in rows if
                      (r["read_percent"], r["threads"], r["mode"]) == (reads, threads, mode)]
            summary.append(dict(read_percent=reads, threads=threads, mode=mode,
                                median_ops_per_second=statistics.median(values),
                                min_ops_per_second=min(values), max_ops_per_second=max(values)))
with (args.out / "summary.csv").open("w", newline="") as target:
    writer = csv.DictWriter(target, fieldnames=list(summary[0]))
    writer.writeheader()
    writer.writerows(summary)

fig, axes = plt.subplots(1, len(args.reads), figsize=(6*len(args.reads), 4.7), squeeze=False)
for axis, reads in zip(axes[0], args.reads):
    for mode in modes:
        selected = sorted((r for r in summary if r["read_percent"] == reads and r["mode"] == mode),
                          key=lambda r: r["threads"])
        med = [r["median_ops_per_second"]/1e6 for r in selected]
        lower = [(r["median_ops_per_second"]-r["min_ops_per_second"])/1e6 for r in selected]
        upper = [(r["max_ops_per_second"]-r["median_ops_per_second"])/1e6 for r in selected]
        axis.errorbar([r["threads"] for r in selected], med, yerr=[lower, upper],
                      marker="o", capsize=3, label=mode)
    axis.set(title=f"{reads}% lookup / {100-reads}% update", xlabel="Worker threads",
             ylabel="Table operations per second (millions)", xticks=sorted(set(args.threads)))
    axis.set_ylim(bottom=0)
    axis.grid(alpha=.25)
    axis.legend()
fig.suptitle("MAC-style table lock comparison: median and observed min–max")
fig.tight_layout()
fig.savefig(args.out / "throughput.png", dpi=180)
fig.savefig(args.out / "throughput.pdf")
plt.close(fig)

short = sum(r["seconds"] < .1 for r in rows)
report = ["# Lock strategy benchmark", "", "## Scope and method", "",
    "This is a multithreaded MAC-style table microbenchmark, not a packet-forwarding benchmark. "
    "All modes use the same preloaded 256 VLAN/MAC keys, 16 bucket chains, hash function, "
    "per-worker random sequences and lookup/update bodies. Only synchronization differs: "
    "one global mutex, 16 striped mutexes, or one process-private read-write lock.", "",
    "Lookup reads a port. Update increments a protected port value and update counter. "
    "All keys are hits. Entries are preallocated and stable; the timed region excludes "
    "allocation, expiration, eviction, logging, networking and production ports_lock. "
    "Lookup does not update LRU or aging; the production switch lookup updates LRU, so "
    "this read-write-lock result is not a drop-in performance claim for the current MAC table.", "",
    f"Each worker executes {args.ops:,} operations. There are {args.repeats} measured repeats "
    "per combination, with separate warmup runs and randomized policy order. Thread creation "
    "is excluded; coordinated wakeup and workload execution are included. Total work increases "
    "with thread count, so the graph compares throughput rather than fixed-job completion time.", "",
    f"Hot-key selection: {args.hot}% of requests are forced to key zero; remaining selections "
    "are uniform. Every run verifies total stored updates equals total worker write operations. "
    "Checksums need not match between modes because concurrent read interleavings differ.", "",
    "## Results", "", "![Throughput comparison](throughput.png)", "",
    "Error bars show observed minimum–maximum, not confidence intervals.", "",
    "| Lookup share | Threads | Global Mops/s | Striped Mops/s | RW-lock Mops/s |",
    "|---|---:|---:|---:|---:|"]
for reads in args.reads:
    for threads in args.threads:
        group = {r["mode"]: r["median_ops_per_second"]/1e6 for r in summary
                 if r["read_percent"] == reads and r["threads"] == threads}
        report.append(f"| {reads}% | {threads} | {group['global']:.3f} | "
                      f"{group['striped']:.3f} | {group['rwlock']:.3f} |")
report += ["", "## Interpretation", ""]
for reads in args.reads:
    group = [r for r in summary if r["read_percent"] == reads and r["threads"] == max(args.threads)]
    winner = max(group, key=lambda r: r["median_ops_per_second"])
    report.append(f"At {max(args.threads)} threads and {reads}% lookups, the largest measured "
                  f"median was **{winner['mode']}**, {winner['median_ops_per_second']/1e6:.3f} Mops/s. "
                  "This observation applies to this machine and workload.")
report += ["", "Striping permits operations in different buckets to proceed concurrently. "
    "A global mutex serializes all operations. A read-write lock permits concurrent read-only "
    "lookups but serializes writers; bookkeeping, reader/writer fairness and write frequency "
    "can make it slower than a mutex. There is no assumed universal winner.", "",
    "## Limitations and reproducibility", "",
    f"{short} of {len(rows)} timed runs lasted under 0.1 seconds. Increase --ops if many runs "
    "are short. Repeat on an otherwise idle target machine. CPU quotas, affinity, oversubscription, "
    "frequency changes, cache effects and false sharing can affect results. No CPU pinning or "
    "real-time scheduling is imposed. Report raw variability rather than hiding outliers.", "",
    "Run optimized binaries for timing. Use Valgrind/ThreadSanitizer separately for correctness; "
    "instrumented throughput is not comparable to normal optimized performance.", "",
    "Raw trials: raw.csv. Aggregates: summary.csv. Platform/compiler/settings: metadata.json. "
    "The compile command in metadata is the recommended command, not proof of binary provenance.", ""]
(args.out / "report.md").write_text("\n".join(report))
print(f"Saved results, graph and report to {args.out}", flush=True)
if short:
    print(f"NOTE: {short} short trials; increase --ops for final measurements.", flush=True)
