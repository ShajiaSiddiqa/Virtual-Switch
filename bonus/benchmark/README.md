# Bonus: multithreaded lock benchmark

This standalone experiment does not modify the working switch. It compares
one global mutex, 16 striped mutexes and one read-write lock protecting the
same MAC-style table workload. It measures table operations/second, not frames
or network throughput. Reads do not update LRU; allocation, insertion/removal,
aging, security, logging and the switch's ports_lock are excluded. See the
scope section of the generated report before interpreting results.

Build from the repository root:

```sh
mkdir -p build
gcc -std=c11 -Wall -Wextra -Werror -O2 -g -pthread \
    bonus/benchmark/lock_bench.c -o build/lock_bench
python3 bonus/benchmark/check_benchmark.py
```

Correctness under Valgrind (not for speed measurements):

```sh
valgrind --leak-check=full --show-leak-kinds=all \
    --errors-for-leak-kinds=all --error-exitcode=99 \
    ./build/lock_bench striped 4 10000 50 20 42
```

Install graph dependency in a virtual environment if needed:

```sh
python3 -m venv .venv-benchmark
. .venv-benchmark/bin/activate
python3 -m pip install matplotlib
```

Generate target-machine results:

```sh
python3 bonus/benchmark/run_benchmark.py --ops 1000000 --repeats 3
```

Outputs in results/benchmark: raw.csv, summary.csv, metadata.json,
throughput.png, throughput.pdf and report.md. Keep actual measured data and
report with the submission; do not substitute results from another machine.
If trials are short, increase --ops. Keep other CPU-heavy work stopped.

For concentrated contention:

```sh
python3 bonus/benchmark/run_benchmark.py --hot 80 --ops 1000000 \
    --out results/benchmark-hot
```

Each C run accepts:

```
lock_bench MODE THREADS OPS_PER_THREAD READ_PERCENT HOT_PERCENT SEED
```



## Recorded benchmark results

The submitted measurements were generated with:

```bash
python3 bonus/benchmark/run_benchmark.py --ops 5000000 --repeats 3
```

The main thread waits for every worker to be ready before releasing the start
gate. Timing ends at the last worker's finish timestamp, excluding joins.
Workers use private random state. Matching seeds reproduce operation choices
across strategies; read checksums can differ with scheduling. Stored update
counts must match total write operations or the run fails.

The graph shows medians and observed min/max, not statistical confidence
intervals. A different winner on a different machine is a valid result.
Remaining bonuses: two switch processes linked by pipes; supervisor with
crash-surviving shared MAC storage. Neither is implemented by this experiment.
