#!/usr/bin/env python3
"""Times the 22 TPC-H queries: ordinary SQL vs the automatic SIMD-ASA rewrite.

Configurations: "plain" (simd_asa.rewrite = off: the one-world query), "noised" (the rewrite
with PAC noise, the default) and "deterministic" (simd_asa.noise = off). Each query runs once
to warm up, then RUNS times per configuration in one session; the median of psql's \\timing
is reported in ms. Q2, Q11 and Q16 read no entity-dependent relation and are not rewritten.

usage: bench/tpch_timing.py "<psql command>" [runs] [workers]
"""
import os
import re
import statistics
import subprocess
import sys

PSQL = sys.argv[1]
RUNS = int(sys.argv[2]) if len(sys.argv) > 2 else 3
WORKERS = int(sys.argv[3]) if len(sys.argv) > 3 else 4
HERE = os.path.dirname(os.path.abspath(__file__))
CONFIGS = {
    "plain": "SET simd_asa.rewrite = off;",
    "noised": "SET simd_asa.rewrite = on;",
    "deterministic": "SET simd_asa.rewrite = on;\nSET simd_asa.noise = off;",
}


def time_query(query, settings):
    script = (f"LOAD 'simd_asa';\nSET jit = off;\nSET max_parallel_workers_per_gather = {WORKERS};\n"
              f"{settings}\n\\o /dev/null\n{query}\n\\timing on\n" + f"{query}\n" * RUNS)
    out = subprocess.run(PSQL.split() + ["-X", "-q"], input=script, capture_output=True, text=True, check=True)
    times = [float(t) for t in re.findall(r"Time: ([0-9.]+) ms", out.stdout)]
    if len(times) != RUNS:
        raise RuntimeError(out.stdout + out.stderr)
    return statistics.median(times)


print(f"| Query | {' | '.join(CONFIGS)} | noised / plain |")
print("|---|" + "---:|" * (len(CONFIGS) + 1))
for n in range(1, 23):
    with open(os.path.join(HERE, "tpch_queries", f"q{n:02d}.sql")) as f:
        query = f.read().strip()
    t = {name: time_query(query, settings) for name, settings in CONFIGS.items()}
    print(f"| Q{n} | " + " | ".join(f"{t[c]:.1f}" for c in CONFIGS) + f" | {t['noised'] / t['plain']:.2f} |",
          flush=True)
