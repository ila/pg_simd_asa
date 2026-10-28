#!/usr/bin/env python3
"""TPC-H Q1 over 64 stochastic worlds in PostgreSQL.

Variants (ORDERS is the sampling entity: mask = priv_hash(asa_hash(l_orderkey))):
  q1           ordinary Q1, one world (cost reference, not equivalent work)
  simd_asa     Q1 with as_sum / as_avg / as_count: 64 worlds per aggregate call
  conditional  Q1 with 64 x AGG(...) FILTER (WHERE bit j of mask): 64 worlds, 512 aggregates

usage: bench/tpch_q1.py "psql -h localhost -p 5432 -U postgres" [runs]
Requires a loaded TPC-H lineitem table and the simd_asa extension.
"""
import re
import statistics
import subprocess
import sys

PSQL = sys.argv[1] if len(sys.argv) > 1 else "psql"
RUNS = int(sys.argv[2]) if len(sys.argv) > 2 else 3

WHERE = "l_shipdate <= date '1998-12-01' - interval '90' day"
MASKED = f"(SELECT *, priv_hash(asa_hash(l_orderkey)) AS m FROM lineitem WHERE {WHERE}) s"
EXPRS = ["l_quantity", "l_extendedprice", "l_extendedprice * (1 - l_discount)",
         "l_extendedprice * (1 - l_discount) * (1 + l_tax)"]
AVGS = ["l_quantity", "l_extendedprice", "l_discount"]

Q1 = (f"SELECT l_returnflag, l_linestatus, {', '.join(f'sum({e})' for e in EXPRS)}, "
      f"{', '.join(f'avg({e})' for e in AVGS)}, count(*) "
      f"FROM lineitem WHERE {WHERE} GROUP BY 1, 2 ORDER BY 1, 2")
SIMD_ASA = (f"SELECT l_returnflag, l_linestatus, {', '.join(f'as_sum(m, {e})' for e in EXPRS)}, "
            f"{', '.join(f'as_avg(m, {e})' for e in AVGS)}, as_count(m) "
            f"FROM {MASKED} GROUP BY 1, 2 ORDER BY 1, 2")


def conditional_aggs(j):
    f = f"FILTER (WHERE (m >> {j}) & 1 = 1)"
    return ([f"sum({e}) {f}" for e in EXPRS] + [f"avg({e}) {f}" for e in AVGS] + [f"count(*) {f}"])


CONDITIONAL = (f"SELECT l_returnflag, l_linestatus, "
               f"{', '.join(a for j in range(64) for a in conditional_aggs(j))} "
               f"FROM {MASKED} GROUP BY 1, 2 ORDER BY 1, 2")


def psql(sql, settings=""):
    out = subprocess.run(PSQL.split() + ["-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=1"],
                         input=settings + sql, capture_output=True, text=True, check=True)
    return out.stdout


def exec_ms(sql, settings):
    out = psql(f"EXPLAIN (ANALYZE, TIMING OFF) {sql};", settings)
    return float(re.search(r"Execution Time: ([\d.]+) ms", out).group(1))


# Correctness: every SIMD-ASA world equals the conditional aggregate (x2 for SUM/COUNT).
check = f"""
WITH a AS (SELECT l_returnflag, l_linestatus, as_sum(m, l_extendedprice) s, as_avg(m, l_quantity) q, as_count(m) c
           FROM {MASKED} GROUP BY 1, 2),
     n AS (SELECT l_returnflag, l_linestatus, j, 2 * sum(l_extendedprice) FILTER (WHERE (m >> j) & 1 = 1) s,
                  avg(l_quantity) FILTER (WHERE (m >> j) & 1 = 1) q, 2 * count(*) FILTER (WHERE (m >> j) & 1 = 1) c
           FROM {MASKED}, generate_series(0, 63) j GROUP BY 1, 2, 3)
SELECT count(*), max(abs(a.s[j + 1] - n.s) / n.s), max(abs(a.q[j + 1] - n.q) / n.q), max(abs(a.c[j + 1] - n.c))
FROM a JOIN n USING (l_returnflag, l_linestatus);
"""
cells, rel_sum, rel_avg, abs_cnt = psql(check).strip().split("|")
print(f"validation: {cells} cells, max rel err as_sum {float(rel_sum):.2e}, as_avg {float(rel_avg):.2e}, "
      f"max abs err as_count {abs_cnt}")

print(f"{'variant':<12} {'workers':>7} {'median ms':>10}   runs")
for workers in (0, 4):
    settings = f"SET max_parallel_workers_per_gather = {workers}; SET jit = off;\n"
    for name, sql in (("q1", Q1), ("simd_asa", SIMD_ASA), ("conditional", CONDITIONAL)):
        exec_ms(sql, settings)  # warm-up
        times = [exec_ms(sql, settings) for _ in range(RUNS)]
        print(f"{name:<12} {workers:>7} {statistics.median(times):>10.1f}   {', '.join(f'{t:.0f}' for t in times)}",
              flush=True)
