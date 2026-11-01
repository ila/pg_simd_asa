# simd_asa — SIMD-accelerated stochastic aggregation for PostgreSQL

`simd_asa` evaluates an aggregate query over 64 sampled versions of a database in a single
execution, instead of running it once per sample. It is a PostgreSQL port of SIMD-ASA from the
DuckDB `privacy` extension: the 64-world aggregates, a rewrite that applies them to ordinary
SQL, and that extension's noised release of the 64 answers. On all 22 TPC-H queries it returns
what the DuckDB extension returns.

## The problem: sampled execution is slow

Many analyses evaluate the same query over many sampled versions of a database and combine the
answers into an error bar, a confidence bound or a noised release: survey replicate weights
(MEPS publishes 128 balanced repeated replicates), jackknife and bootstrap estimates, Monte
Carlo and probabilistic databases, sampling-based approximate query processing, Monte Carlo
Shapley values and sample-and-aggregate differential privacy.

The samples are usually drawn over an *entity* rather than over rows: a customer, a patient, a
household. Each sample — a *world* — is a complete database instance: the entity table and every
table that reaches it through foreign keys (the customers' `orders` and their `lineitem`s) keep
only the sampled entities' rows, while tables outside that path (`nation`, `part`) stay whole.

The direct implementation builds each world and runs the query on it, *m* times. Its cost grows
linearly with *m*: *m* times the same scans, joins, sub-queries and aggregations, for answers
that differ only in which entities they include.

## The idea

Run the query once and carry the worlds along as bits. Each entity gets an *m*-bit *membership
mask*, bit *j* set when it is in world *j*. For random halves (each entity in each world with
probability ½) the mask is just a 64-bit hash of the entity's key, so no world is materialized.
Every row inherits the mask of the entity it belongs to — an `orders` row its customer's, a
`lineitem` row its order's customer's — and a row joined from two entities is in the worlds
that contain both (the AND of their masks).

Scans, joins, filters and grouping then run once for all worlds. Only the aggregates are
widened: each keeps 64 results per group instead of one and adds a row into result *j* only
when bit *j* of the row's mask is set. With 64 worlds the mask is one machine word and the
update is a short branch-free loop that compiles to SIMD instructions:

```c
for (int j = 0; j < 64; j++)      /* count[]: one group's 64 results */
    count[j] += (mask >> j) & 1;  /* +1 in the worlds this row belongs to */
```

This vectorizes across worlds rather than across rows: one input value updates 64 results of
the same group, stored next to each other, so it stays SIMD-friendly in `GROUP BY` queries,
where row-wise SIMD would scatter values to different groups.

Each group gets 64 answers, exactly the ones 64 separate executions would return. Predicates
over aggregates — TPC-H Q17's `l_quantity < (SELECT 0.2 * avg(l_quantity) ...)` — hold in some
worlds only: the sub-query returns its 64 values, the comparison becomes a mask of the worlds
where it holds, and that mask is AND-ed into the rows' masks, so nested queries run once too.

What to do with the 64 answers depends on the application. By default the rewrite releases one
noised value per group ([Release](#release)); the 64 answers themselves are available as well
([Using the aggregates directly](#using-the-aggregates-directly)).

## Quick start

Say we want to know how much the quantities, discounts and line counts per return flag in
TPC-H depend on which customers are in the data: how different would they be on another random
half of the customers? Directly, that means drawing 64 random halves of `customer`, keeping
each half's `orders` and `lineitem` rows, and running the query 64 times. With `simd_asa` we
declare the customer as the sampling entity once and run the query as usual:

```sql
CREATE EXTENSION simd_asa;

-- the sampling entity, and how the other tables reach it
SELECT asa_add_entity('customer', 'c_custkey');
SELECT asa_add_link('orders', 'o_custkey', 'customer', 'c_custkey');
SELECT asa_add_link('lineitem', 'l_orderkey', 'orders', 'o_orderkey');

LOAD 'simd_asa';
SET simd_asa.rewrite = on;

-- ordinary SQL
SELECT l_returnflag, sum(l_quantity), avg(l_discount), count(*) FROM lineitem GROUP BY 1;
```

The query is rewritten before it is planned (`EXPLAIN` shows the result). `lineitem` has no
customer column, so the rewrite joins `orders` to find each line item's customer, and every
aggregate becomes its 64-world version over that customer's mask:

```
original                                   rewritten

HashAggregate  GROUP BY l_returnflag       HashAggregate  GROUP BY l_returnflag
│ sum(l_quantity)                          │ asa_noised_sum(m, l_quantity)
│ avg(l_discount)                          │ asa_noised_avg(m, l_discount)
│ count(*)                                 │ asa_noised_count(m)
│                                          │   m = priv_hash(asa_hash(o_custkey))
└─ Seq Scan lineitem                       │       the worlds of the line item's customer
                                           └─ Hash Join  l_orderkey = o_orderkey
                                              ├─ Seq Scan lineitem
                                              └─ Hash
                                                 └─ Seq Scan orders  (o_orderkey, o_custkey)
```

TPC-H SF1, the original query:

| l_returnflag | sum | avg | count |
|---|---:|---:|---:|
| A | 37734107.00 | 0.04998529583839761162 | 1478493 |
| N | 77624935.00 | 0.05000144882208464801 | 3043852 |
| R | 37719753.00 | 0.05000940583012705647 | 1478870 |

The 64 answers of `count(*)`, one per world, before the release. Each world holds about
half of the customers; counts and sums are doubled, so every world estimates the full answer:

| l_returnflag | world 1 | world 2 | world 3 | … | world 64 |
|---|---:|---:|---:|---|---:|
| A | 1480358 | 1485016 | 1470636 | … | 1480442 |
| N | 3048814 | 3050502 | 3021656 | … | 3048802 |
| R | 1481902 | 1485608 | 1471898 | … | 1483296 |

The rewritten query's result, one noised value per group (the default):

| l_returnflag | sum | avg | count |
|---|---:|---:|---:|
| A | 39340120 | 0.0500151657965117 | 1510236 |
| N | 78612688 | 0.0504914382172441 | 2989416 |
| R | 38213316 | 0.0501655618661092 | 1463237 |

The result has the column types of the original query (`count` → `bigint`, `sum(numeric)` →
`numeric`, ...). Queries over tables outside the entity's path (`nation`, `part`) run unchanged.

## Performance

TPC-H SF30 (44 GB) on a MacBook Pro (Apple M1, 16 GB), PostgreSQL 16.15, 4 parallel workers,
`work_mem = 256MB`, JIT off; median of 2 runs after a warm-up, in ms. The database does not fit
in memory, so the runs read from disk.

**Evaluating 64 worlds makes TPC-H at most 2.2× slower than the ordinary single-world query
(Q1); all other queries are within 1.5×.**

| Query | ordinary | 64 worlds | 64 worlds / ordinary |
|---|---:|---:|---:|
| Q1 | 58689 | 130769 | 2.23 |
| Q2 | 11149 | 8612 | 0.77 |
| Q3 | 61770 | 52903 | 0.86 |
| Q4 | 36940 | 37587 | 1.02 |
| Q5 | 55417 | 59942 | 1.08 |
| Q6 | 38745 | 54579 | 1.41 |
| Q7 | 60070 | 60499 | 1.01 |
| Q8 | 64077 | 55470 | 0.87 |
| Q9 | 531423 | 490601 | 0.92 |
| Q10 | 53819 | 50909 | 0.95 |
| Q11 | 12302 | 12025 | 0.98 |
| Q12 | 53794 | 54705 | 1.02 |
| Q13 | 35687 | 37611 | 1.05 |
| Q14 | 45888 | 56047 | 1.22 |
| Q15 | 39555 | 56356 | 1.42 |
| Q16 | 11139 | 10913 | 0.98 |
| Q17 | 234388 | 11046 | 0.05 |
| Q18 | 290547 | 296334 | 1.02 |
| Q19 | 9209 | 9084 | 0.99 |
| Q20 | 166532 | 218401 | 1.31 |
| Q21 | 47772 | 52597 | 1.10 |
| Q22 | 15094 | 13694 | 0.91 |

The overhead comes mostly from the joins that find each row's entity where the query has none
(Q1, Q6, Q14, Q15 and Q20 read `lineitem` without `orders`), not from the 64-world aggregates;
aggregation-heavy Q1 is the exception. Q2, Q11 and Q16 read no entity-dependent table and are
not rewritten, so their ratios show the run-to-run variance of the disk-bound setup (Q9's runs
alone range from 447 s to 545 s). Q17 is faster only because PostgreSQL picks a better plan for
the rewritten query. Measured with `bench/tpch_timing.py "psql -d tpch30" 2 4`
(`PGOPTIONS="-c work_mem=256MB"`), with noise off (`simd_asa.noise = off`); the noise is added
once per output cell, after aggregation.

## Release

By default the rewrite turns a group's 64 answers into one value:

1. **NULL.** A group is NULL with probability popcount(¬mask)/64, `mask` being the OR of its
   rows' masks: a group that few entities contribute to is likely suppressed.
2. **Value.** The answer of one hidden world (the same for every group of a statement), plus
   Gaussian noise of variance σ²/(2·mi), σ² being the variance of the 64 answers.
3. **Correction.** A row joined from *k* entities is in a world only if all *k* are (probability
   2⁻ᵏ instead of ½), so its counts and sums are scaled by 2^(k−1).

| Setting | Default | |
|---|---|---|
| `simd_asa.noise` | `on` | `off`: no noise, the answer of world 0 |
| `simd_asa.mi` | 1/128 | noise budget: lower = more noise |
| `simd_asa.seed` | 42 | seed of the worlds and the noise |
| `simd_asa.hash_repair` | `off` | put every entity in exactly half of the worlds |

With the same seed, data and plan, a statement releases the same values every time (use
`max_parallel_workers_per_gather = 0` for repeatable runs). Since every statement then gets the
same noise, change the seed between statements whose results must not be compared.

## Automatic rewrite

```sql
SELECT asa_add_entity('customer', 'c_custkey');                       -- the sampling entity
SELECT asa_add_link('orders', 'o_custkey', 'customer', 'c_custkey');  -- how a table reaches it
```

Links need not be foreign keys (`asa_drop_entity` / `asa_drop_link` remove them). With several
entities declared, `SET simd_asa.entity` picks one. The rewrite runs before planning, level by
level (statement, subquery, CTE, sub-select):

- **Masks.** A row's mask is the AND of the masks of the entities it comes from; a join needed
  to reach the entity is added when the query lacks it (`lineitem` gets `orders`).
- **Aggregates.** `sum`, `count`, `avg`, `min` and `max`, also with `DISTINCT`, become 64-world
  aggregates. Expressions over several aggregates (Q8, Q14) are evaluated per world, then
  released.
- **Subqueries.** A subquery without aggregates passes its mask up; one with aggregates is
  released in place (Q15); one grouped per entity stays exact (Q13, Q18).
- **Predicates over aggregates.** A comparison with an aggregating sub-select (Q17, Q20, Q22) or
  a `HAVING` holds in some worlds only and becomes a mask.
- **Top-k.** `ORDER BY ... LIMIT` over aggregates releases only the selected groups (Q3, Q10,
  Q21).

Unsupported shapes are rejected with an error instead of being answered over a single world,
among them window functions, grouping sets, set operations, views, user-defined functions,
`ORDER BY` inside aggregates and entity-dependent tables on the nullable side of an outer join.
The hook must be
loaded in the session (`LOAD 'simd_asa'` or `session_preload_libraries`).

## Using the aggregates directly

`as_` stands for *accelerated stochastic*. A 64-world answer is a `real[]` of 64 values.

| Function | Returns |
|---|---|
| `as_count(mask [, value])`, `as_sum`, `as_avg`, `as_min`, `as_max(mask, value)` | `real[64]`: the aggregate in each world |
| `as_noised_count`, `as_noised_sum`, ... `(mask [, value])` | the release, of the ordinary aggregate's type |
| `priv_hash(asa_hash(key))` | `bigint`: the mask of an entity |
| `asa_lane_add/sub/mul/div(a, b)`, `asa_lane_lt/le/gt/ge/eq/ne(a, b)` | per-world arithmetic; mask of the worlds where a comparison holds |

`as_count` and `as_sum` double each world's answer, so every world estimates the full-data
aggregate. Any 64-bit value works as a mask, e.g. packed survey replicate flags. TPC-H Q1 by
hand, each line item in the worlds of its order's customer:

```sql
SELECT l_returnflag, l_linestatus,
       as_sum(m, l_extendedprice * (1 - l_discount)) AS revenue,  -- 64 sums
       as_avg(m, l_quantity)                          AS avg_qty,  -- 64 averages
       as_count(m)                                    AS items     -- 64 counts
FROM (SELECT l.*, priv_hash(asa_hash(o_custkey)) AS m
      FROM lineitem l JOIN orders ON l_orderkey = o_orderkey
      WHERE l_shipdate <= date '1998-12-01' - interval '90' day) s
GROUP BY 1, 2;
```

## Build and test

Requires PostgreSQL ≥ 16 (`pg_config`) and a little-endian CPU.

```sh
make && make install
make installcheck    # against a running server (PGHOST/PGPORT/PGUSER)
```

The tests check every world of every aggregate against 64 `FILTER (WHERE bit j)` aggregates,
and every rewritten query against its hand-written version.

## License

AGPL-3.0.
