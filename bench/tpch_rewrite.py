#!/usr/bin/env python3
"""TPC-H through the automatic SIMD-ASA rewrite (CUSTOMER as the sampling entity).

For each query: the rewritten query (ordinary SQL, simd_asa.rewrite = on) must return exactly
the result of the hand-written as_* query (checked single-threaded), and both are timed against
the ordinary one-world query. Q14 (expressions over aggregates) and Q17 (correlated AVG as a
predicate over worlds) are additionally checked against their definition: the original query
evaluated separately in each of the 64 worlds.

usage: bench/tpch_rewrite.py "psql -h localhost -p 5432 -U postgres" [runs]
Requires TPC-H customer/orders/lineitem/part/supplier/nation/region, an index on
lineitem(l_partkey) (Q17's correlated subquery) and the simd_asa extension.
"""
import re
import statistics
import subprocess
import sys

PSQL = sys.argv[1] if len(sys.argv) > 1 else "psql"
RUNS = int(sys.argv[2]) if len(sys.argv) > 2 else 3

SETUP = """
SELECT asa_add_entity('customer', 'c_custkey');
SELECT asa_add_link('orders', 'o_custkey', 'customer', 'c_custkey');
SELECT asa_add_link('lineitem', 'l_orderkey', 'orders', 'o_orderkey');
"""

C = "priv_hash(asa_hash(c_custkey))"
O = "priv_hash(asa_hash(o_custkey))"
REV = "l_extendedprice * (1 - l_discount)"
# (name, ordinary SQL, hand-written SIMD-ASA SQL)
QUERIES = [
    ("Q1",
     "SELECT l_returnflag, l_linestatus, sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
     f"sum({REV}) AS sum_disc_price, sum({REV} * (1 + l_tax)) AS sum_charge, avg(l_quantity) AS avg_qty, "
     "avg(l_extendedprice) AS avg_price, avg(l_discount) AS avg_disc, count(*) AS count_order "
     "FROM lineitem WHERE l_shipdate <= date '1998-12-01' - interval '90' day GROUP BY 1, 2 ORDER BY 1, 2",
     f"SELECT l_returnflag, l_linestatus, as_sum({O}, l_quantity) AS sum_qty, as_sum({O}, l_extendedprice) AS "
     f"sum_base_price, as_sum({O}, {REV}) AS sum_disc_price, as_sum({O}, {REV} * (1 + l_tax)) AS sum_charge, "
     f"as_avg({O}, l_quantity) AS avg_qty, as_avg({O}, l_extendedprice) AS avg_price, "
     f"as_avg({O}, l_discount) AS avg_disc, as_count({O}) AS count_order "
     "FROM lineitem JOIN orders ON l_orderkey = o_orderkey "
     "WHERE l_shipdate <= date '1998-12-01' - interval '90' day GROUP BY 1, 2 ORDER BY 1, 2"),
    ("Q3",
     f"SELECT l_orderkey, sum({REV}) AS revenue, o_orderdate, o_shippriority FROM customer, orders, lineitem "
     "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND l_orderkey = o_orderkey "
     "AND o_orderdate < date '1995-03-15' AND l_shipdate > date '1995-03-15' "
     "GROUP BY l_orderkey, o_orderdate, o_shippriority ORDER BY revenue DESC, o_orderdate LIMIT 10",
     f"SELECT l_orderkey, as_sum({C}, {REV}) AS revenue, o_orderdate, o_shippriority FROM customer, orders, "
     "lineitem WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND l_orderkey = o_orderkey "
     "AND o_orderdate < date '1995-03-15' AND l_shipdate > date '1995-03-15' "
     "GROUP BY l_orderkey, o_orderdate, o_shippriority ORDER BY revenue DESC, o_orderdate LIMIT 10"),
    ("Q5",
     f"SELECT n_name, sum({REV}) AS revenue FROM customer, orders, lineitem, supplier, nation, region "
     "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey "
     "AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
     "AND r_name = 'ASIA' AND o_orderdate >= date '1994-01-01' AND o_orderdate < date '1995-01-01' "
     "GROUP BY n_name ORDER BY revenue DESC",
     f"SELECT n_name, as_sum({C}, {REV}) AS revenue FROM customer, orders, lineitem, supplier, nation, region "
     "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey "
     "AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
     "AND r_name = 'ASIA' AND o_orderdate >= date '1994-01-01' AND o_orderdate < date '1995-01-01' "
     "GROUP BY n_name ORDER BY revenue DESC"),
    ("Q6",
     "SELECT sum(l_extendedprice * l_discount) AS revenue FROM lineitem WHERE l_shipdate >= date '1994-01-01' "
     "AND l_shipdate < date '1995-01-01' AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity < 24",
     f"SELECT as_sum({O}, l_extendedprice * l_discount) AS revenue FROM lineitem JOIN orders "
     "ON l_orderkey = o_orderkey WHERE l_shipdate >= date '1994-01-01' "
     "AND l_shipdate < date '1995-01-01' AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity < 24"),
    ("Q10",
     f"SELECT c_custkey, c_name, sum({REV}) AS revenue, c_acctbal, n_name, c_address, c_phone, c_comment "
     "FROM customer, orders, lineitem, nation WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey "
     "AND o_orderdate >= date '1993-10-01' AND o_orderdate < date '1994-01-01' AND l_returnflag = 'R' "
     "AND c_nationkey = n_nationkey GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment "
     "ORDER BY revenue DESC LIMIT 20",
     f"SELECT c_custkey, c_name, as_sum({C}, {REV}) AS revenue, c_acctbal, n_name, c_address, c_phone, c_comment "
     "FROM customer, orders, lineitem, nation WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey "
     "AND o_orderdate >= date '1993-10-01' AND o_orderdate < date '1994-01-01' AND l_returnflag = 'R' "
     "AND c_nationkey = n_nationkey GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment "
     "ORDER BY revenue DESC LIMIT 20"),
    ("Q12",
     "SELECT l_shipmode, sum(CASE WHEN o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END) AS high, "
     "sum(CASE WHEN o_orderpriority NOT IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END) AS low "
     "FROM orders, lineitem WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL', 'SHIP') "
     "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate AND l_receiptdate >= date '1994-01-01' "
     "AND l_receiptdate < date '1995-01-01' GROUP BY l_shipmode ORDER BY l_shipmode",
     f"SELECT l_shipmode, as_sum({O}, CASE WHEN o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END) AS "
     f"high, as_sum({O}, CASE WHEN o_orderpriority NOT IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END) AS low "
     "FROM orders, lineitem WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL', 'SHIP') "
     "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate AND l_receiptdate >= date '1994-01-01' "
     "AND l_receiptdate < date '1995-01-01' GROUP BY l_shipmode ORDER BY l_shipmode"),
    ("Q14",
     f"SELECT 100.00 * sum(CASE WHEN p_type LIKE 'PROMO%' THEN {REV} ELSE 0 END) / sum({REV}) AS promo_revenue "
     "FROM lineitem, part WHERE l_partkey = p_partkey AND l_shipdate >= date '1995-09-01' "
     "AND l_shipdate < date '1995-10-01'",
     f"SELECT asa_lane_div(asa_lane_mul(100.00, as_sum({O}, CASE WHEN p_type LIKE 'PROMO%' THEN {REV} ELSE 0 END)), "
     f"as_sum({O}, {REV})) AS promo_revenue FROM lineitem JOIN orders ON l_orderkey = o_orderkey, part "
     "WHERE l_partkey = p_partkey AND l_shipdate >= date '1995-09-01' AND l_shipdate < date '1995-10-01'"),
    ("Q17",
     "SELECT sum(l_extendedprice) / 7.0 AS avg_yearly FROM lineitem, part WHERE p_partkey = l_partkey "
     "AND p_brand = 'Brand#23' AND p_container = 'MED BOX' "
     "AND l_quantity < (SELECT 0.2 * avg(l_quantity) FROM lineitem WHERE l_partkey = p_partkey)",
     "SELECT asa_lane_div(as_sum(priv_hash(asa_hash(o.o_custkey)) & asa_lane_lt(l.l_quantity::float8, "
     "(SELECT asa_lane_mul(0.2, as_avg(priv_hash(asa_hash(o2.o_custkey)), l2.l_quantity)) FROM lineitem l2 "
     "JOIN orders o2 ON l2.l_orderkey = o2.o_orderkey WHERE l2.l_partkey = p_partkey)), l.l_extendedprice), 7.0) "
     "AS avg_yearly FROM lineitem l JOIN orders o ON l.l_orderkey = o.o_orderkey, part "
     "WHERE p_partkey = l.l_partkey AND p_brand = 'Brand#23' AND p_container = 'MED BOX'"),
]

# The definition: each query evaluated on world j alone (rows whose customer is in world j),
# with SUM scaled by 2 as as_sum reports it. Returns (j, value) rows.
WORLD = "generate_series(0, 63) j"
DEFINITIONS = {
    "Q14": f"""SELECT j, 100.00 * sum(CASE WHEN p_type LIKE 'PROMO%' THEN {REV} ELSE 0 END) / sum({REV})
FROM lineitem JOIN orders ON l_orderkey = o_orderkey JOIN part ON l_partkey = p_partkey, {WORLD}
WHERE l_shipdate >= date '1995-09-01' AND l_shipdate < date '1995-10-01' AND ({O} >> j) & 1 = 1 GROUP BY j""",
    "Q17": f"""WITH li AS (SELECT l_partkey, l_quantity, l_extendedprice, {O} AS m
                FROM lineitem JOIN orders ON l_orderkey = o_orderkey JOIN part ON p_partkey = l_partkey
                WHERE p_brand = 'Brand#23' AND p_container = 'MED BOX'),
     w AS (SELECT j, li.* FROM li, {WORLD} WHERE (m >> j) & 1 = 1),
     a AS (SELECT j, l_partkey, avg(l_quantity) AS avg_q FROM w GROUP BY 1, 2)
SELECT j, 2 * sum(l_extendedprice) / 7.0 FROM w JOIN a USING (j, l_partkey) WHERE l_quantity < 0.2 * avg_q GROUP BY j""",
}


def psql(sql):
    out = subprocess.run(PSQL.split() + ["-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=1"],
                         input="LOAD 'simd_asa';\nSET jit = off;\n" + sql, capture_output=True, text=True, check=True)
    return out.stdout


def exec_ms(sql, settings):
    out = psql(f"{settings}\nEXPLAIN (ANALYZE, TIMING OFF) {sql};")
    return float(re.search(r"Execution Time: ([\d.]+) ms", out).group(1))


psql(SETUP)
ON, OFF = "SET simd_asa.rewrite = on;", "SET simd_asa.rewrite = off;"
print("validation (single-threaded): rewritten query == hand-written as_* query")
for name, sql, hand in QUERIES:
    res = psql(f"""SET max_parallel_workers_per_gather = 0;
{ON} CREATE TEMP TABLE rw AS {sql};
{OFF} CREATE TEMP TABLE hw AS {hand};
SELECT (SELECT count(*) FROM rw), (SELECT count(*) FROM (SELECT * FROM rw EXCEPT ALL SELECT * FROM hw) d)
     + (SELECT count(*) FROM (SELECT * FROM hw EXCEPT ALL SELECT * FROM rw) d);""").split()
    rows, mismatches = res[-1].split("|")
    print(f"  {name:<4} rows {rows:>3}  mismatching rows {mismatches}  {'IDENTICAL' if mismatches == '0' else 'DIFF'}")

print("\nper-world definition: rewritten world j == the query evaluated on world j alone")
for name, definition in DEFINITIONS.items():
    sql = next(q for n, q, _ in QUERIES if n == name)
    res = psql(f"""SET max_parallel_workers_per_gather = 0;
{ON} CREATE TEMP TABLE rw AS {sql};
{OFF} CREATE TEMP TABLE def AS {definition};
SELECT count(*), max(abs(r.v - d.v) / abs(d.v)), count(*) FILTER (WHERE abs(r.v - d.v) / abs(d.v) > 1e-5)
FROM (SELECT ordinality - 1 AS j, v FROM rw, unnest(rw.{'promo_revenue' if name == 'Q14' else 'avg_yearly'})
      WITH ORDINALITY u(v, ordinality)) r JOIN def d(j, v) USING (j);""").split()
    worlds, rel, off = res[-1].split("|")
    print(f"  {name:<4} worlds {worlds}  max relative difference {float(rel):.2e}  worlds off by > 1e-5: {off}")

print(f"\ntimings, 4 workers, median of {RUNS} (ms): ordinary 1-world SQL | rewritten | hand-written as_*")
for name, sql, hand in QUERIES:
    row = []
    for settings, q in ((OFF, sql), (ON, sql), (OFF, hand)):
        settings += " SET max_parallel_workers_per_gather = 4;"
        exec_ms(q, settings)  # warm-up
        row.append(statistics.median(exec_ms(q, settings) for _ in range(RUNS)))
    print(f"  {name:<4} {row[0]:>9.1f} | {row[1]:>9.1f} | {row[2]:>9.1f}   rewritten/hand-written {row[1] / row[2]:.3f}",
          flush=True)
