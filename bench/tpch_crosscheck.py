#!/usr/bin/env python3
"""Cross-checks the 22 TPC-H queries through the PostgreSQL rewrite against DuckDB.

Without noise (simd_asa.noise = off / pac_mi = 0) a release is deterministic: world 0 of the
counters, NULL for cells in no world, and filters over worlds decide by majority. Both systems
compute the same masks (seed 42, no hash repair), so the rewritten PostgreSQL queries must
return what DuckDB's privacy extension returns:

  - the automatic PAC rewrite of DuckDB (privacy_mode = 'pac', pac_mi = 0) for the queries it
    compiles (Q3 with priv_check = false: it groups by a protected key column);
  - DuckDB's hand-written AS queries (benchmark/tpch/tpch_as_queries) for Q10, Q18 and Q22,
    which its compiler rejects (columns of the privacy unit outside aggregates). Q22 has a
    predicate over worlds, for which DuckDB's compiler releases through counter lists and
    as_noised (as its plan for Q17 shows): its reference is the hand-written query in that
    form (hand_q22_categorical.sql, out_q22_categorical.csv).

Q2, Q11 and Q16 read no entity-dependent relation: both systems run them unchanged.

D1-D4 (bench/tpch_distinct) are count/sum/avg(DISTINCT ...) over TPC-H, checked against
DuckDB's automatic rewrite (its bit_or pre-aggregation of the distinct values).

C1 checks the correction for rows of two customers, which has no DuckDB reference (DuckDB
derives one hash per privacy-unit table, so its self-joins mask by one of the two customers):
pairs of orders of two different customers, whose masks are AND-ed, are in world 0 with
probability 1/4, and the corrected counts must be within 2% of the exact ones.

Q15 has no DuckDB reference: DuckDB's rewrite of its CTE releases different values
depending on how the CTE is consumed (per supplier, under max(), under ORDER BY ... LIMIT),
so its answer is not the supplier with the largest released revenue. PostgreSQL's answer is
checked to be that supplier instead.

usage: bench/tpch_crosscheck.py "<psql command>" <directory of DuckDB CSV outputs out_qNN.csv>
"""
import csv
import io
import os
import subprocess
import sys

PSQL = sys.argv[1]
ORACLE = sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
REL_TOL = 1e-5

SETTINGS = """LOAD 'simd_asa';
SET simd_asa.rewrite = on;
SET simd_asa.noise = off;
SET simd_asa.sample_diversity_check = off;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
"""


def run_pg(query, extra=""):
    out = subprocess.run(PSQL.split() + ["-X", "-q", "--csv"], input=SETTINGS + extra + query,
                         capture_output=True, text=True, check=True).stdout
    return list(csv.reader(io.StringIO(out)))


ORACLE_FILES = {22: "out_q22_categorical.csv"}

Q15_REVENUE = """WITH revenue AS (SELECT l_suppkey AS supplier_no, sum(l_extendedprice * (1 - l_discount)) AS total_revenue
FROM lineitem WHERE l_shipdate >= CAST('1996-01-01' AS date) AND l_shipdate < CAST('1996-04-01' AS date)
GROUP BY supplier_no) SELECT supplier_no, total_revenue FROM revenue ORDER BY total_revenue DESC, supplier_no"""


def read_oracle(n):
    with open(os.path.join(ORACLE, ORACLE_FILES.get(n, f"out_q{n:02d}.csv"))) as f:
        return list(csv.reader(f))


def check_q15(pg):
    """The answer is the supplier(s) with the largest released revenue."""
    ranked = run_pg(Q15_REVENUE, "SET simd_asa.pushdown_topk = off;\n")
    best = [r for r in ranked[1:] if r[1] == ranked[1][1]]
    answer = [(r[0], r[4]) for r in pg[1:]]
    if answer != [(r[0], r[1]) for r in best]:
        return f"{answer} vs released maximum {best}"
    return None


def same(a, b):
    a, b = a.strip(), b.strip()
    if a == b:
        return True
    try:
        x, y = float(a), float(b)
    except ValueError:
        return False
    return abs(x - y) <= REL_TOL * max(abs(x), abs(y), 1.0)


def compare(pg, duck):
    """Rows in order; rows tied on the ORDER BY keys may come in any order, so unmatched rows
    are retried as a multiset."""
    if len(pg) != len(duck):
        return f"{len(pg) - 1} rows vs {len(duck) - 1}"
    bad = [i for i in range(1, len(pg)) if not all(same(x, y) for x, y in zip(pg[i], duck[i]))]
    if not bad:
        return None
    rest = [duck[i] for i in bad]
    for i in bad:
        match = next((r for r in rest if all(same(x, y) for x, y in zip(pg[i], r))), None)
        if match is None:
            return f"row {i}: {pg[i]} vs {duck[i]}"
        rest.remove(match)
    return None


SELF_JOIN = """SELECT a.o_orderpriority, count(*) FROM orders a JOIN orders b
ON a.o_clerk = b.o_clerk AND a.o_orderdate = b.o_orderdate AND a.o_custkey < b.o_custkey GROUP BY 1 ORDER BY 1"""


def check_correction():
    released = run_pg(SELF_JOIN)
    exact = run_pg(SELF_JOIN, "SET simd_asa.rewrite = off;\n")
    for r, e in zip(released[1:], exact[1:]):
        if r[0] != e[0] or abs(float(r[1]) - float(e[1])) > 0.02 * float(e[1]):
            return f"{r} vs exact {e}"
    return None if len(released) == len(exact) else f"{len(released) - 1} rows vs {len(exact) - 1}"


failures = 0
for n in range(1, 23):
    with open(os.path.join(HERE, "tpch_queries", f"q{n:02d}.sql")) as f:
        query = f.read()
    pg = run_pg(query)
    result = check_q15(pg) if n == 15 else compare(pg, read_oracle(n))
    failures += result is not None
    print(f"Q{n:02d}: {'ok' if result is None else 'MISMATCH ' + result}")
for n in range(1, 5):
    with open(os.path.join(HERE, "tpch_distinct", f"d{n}.sql")) as f:
        query = f.read()
    with open(os.path.join(ORACLE, f"out_d{n}.csv")) as f:
        result = compare(run_pg(query), list(csv.reader(f)))
    failures += result is not None
    print(f"D{n}: {'ok' if result is None else 'MISMATCH ' + result}")
result = check_correction()
failures += result is not None
print(f"C1: {'ok' if result is None else 'MISMATCH ' + result}")
sys.exit(1 if failures else 0)
