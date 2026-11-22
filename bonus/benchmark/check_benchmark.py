import json
import subprocess
import sys

binary = sys.argv[1] if len(sys.argv) > 1 else "./build/lock_bench"
for reads in (0, 50, 100):
    expected_writes = None
    for mode in ("global", "striped", "rwlock"):
        result = subprocess.run([binary, mode, "4", "2000", str(reads), "30", "42"],
                                capture_output=True, text=True, timeout=20, check=True)
        row = json.loads(result.stdout)
        assert row["operations"] == 8000
        assert row["writes"] == row["updates"]
        assert row["seconds"] > 0 and row["ops_per_second"] > 0
        if reads == 0:
            assert row["writes"] == 8000
        if reads == 100:
            assert row["writes"] == 0
        if expected_writes is None:
            expected_writes = row["writes"]
        assert expected_writes == row["writes"], "Modes used different operation sequences"
print("PASS: all policies preserve updates and execute identical read/write workloads")
for mode, threads in (("invalid", "4"), ("global", "0"), ("rwlock", "65")):
    result = subprocess.run([binary, mode, threads, "2000", "50", "0", "42"],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode != 0
print("PASS: invalid modes and thread counts rejected")
