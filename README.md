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

**Evaluating 64 worlds makes TPC-H at most 2.1× slower than the ordinary single-world query
(Q1); all other queries are within 1.5×, most within 5%.**

| Query | ordinary | 64 worlds | 64 worlds / ordinary |
|---|---:|---:|---:|
| Q1 | 63992 | 136868 | 2.14 |
| Q2 | 9044 | 8905 | 0.98 |
| Q3 | 51515 | 50467 | 0.98 |
| Q4 | 33309 | 33806 | 1.01 |
| Q5 | 51916 | 50628 | 0.98 |
| Q6 | 36262 | 52088 | 1.44 |
| Q7 | 51622 | 50819 | 0.98 |
| Q8 | 54354 | 54185 | 1.00 |
| Q9 | 679419 | 647517 | 0.95 |
| Q10 | 50228 | 50277 | 1.00 |
| Q11 | 12451 | 12654 | 1.02 |
| Q12 | 50871 | 53573 | 1.05 |
| Q13 | 35460 | 42782 | 1.21 |
| Q14 | 38041 | 50930 | 1.34 |
| Q15 | 37393 | 55217 | 1.48 |
| Q16 | 10642 | 10514 | 0.99 |
| Q17 | 308806 | 11145 | 0.04 |
| Q18 | 269826 | 268325 | 0.99 |
| Q19 | 9532 | 9058 | 0.95 |
| Q20 | 190121 | 243652 | 1.28 |
| Q21 | 48849 | 49044 | 1.00 |
| Q22 | 14862 | 16982 | 1.14 |

As in DuckDB, the overhead comes mostly from the joins that find each row's entity where the
query has none (Q1, Q6, Q14, Q15 and Q20 read `lineitem` without `orders`), not from the
64-world aggregates; aggregation-heavy Q1 is the exception. Q2, Q11 and Q16 read no
entity-dependent table and are not rewritten. Q17 is faster only because PostgreSQL picks a
better plan for the rewritten query. Measured with `bench/tpch_timing.py "psql -d tpch30" 2 4`
(`PGOPTIONS="-c work_mem=256MB"`), with the default release.

## Release

By default the rewrite turns a group's 64 answers into one value (`as_noised_*`), as the DuckDB
extension does:

1. **NULL decision.** A group is NULL with probability popcount(¬mask)/64, `mask` being the OR
   of its rows' masks: a group few entities contribute to is in few worlds and likely
   suppressed.
2. **Value.** The answer of world *J* = query hash mod 64 (the same hidden world for every group
   of a statement), plus Gaussian noise of variance σ² / (2·mi), σ² the variance of the 64
   answers. With posterior tracking (default) the variance is weighted by a posterior over the
   worlds that every release of the statement updates.
3. `avg` releases a noised sum and a noised count and divides them, as DuckDB does. `numeric`
   sums are computed as DuckDB sums `DECIMAL`: integers times 10^scale, the scale derived from
   the argument's type.
4. **Correction.** A row joined from *k* entities is in a world only if all *k* are, with
   probability 2⁻ᵏ instead of ½; counts and sums of such rows are multiplied by DuckDB's
   correction 2^(k−1) (not extremes; averages are unchanged), and the NULL probability divided
   by it.

| Setting | Default | |
|---|---|---|
| `simd_asa.noise` | `on` | `off`: no noise — world 0, NULL only for groups in no world |
| `simd_asa.mi` | 1/128 | noise budget: lower = more noise; 0 = no noise |
| `simd_asa.ptracking` | `on` | posterior tracking across the groups of a statement |
| `simd_asa.sample_diversity_check` | `on` | reject noised aggregates whose groups mostly hold a single entity |
| `simd_asa.seed` | 42 | seed of the worlds and the noise: a statement releases the same values whenever it runs on the same data with the same seed |
| `simd_asa.hash_repair` | `off` | repair every mask to exactly 32 set bits (each entity in exactly half of the worlds) |

Every statement's query hash is the seed's. Randomness comes from a per-statement generator
seeded by it; the generator and the posterior restart when a top-level statement starts. So a
statement releases the same values every time it runs on the same data with the same plan. A
different plan can differ: groups draw in the order they are finalized, and parallel workers
combine floating-point partial sums in the order they finish (use
`max_parallel_workers_per_gather = 0` for repeatable runs).

A fixed seed also means every statement uses the same worlds and the same noise draws: two
queries that differ in one entity (say, one with `WHERE c_custkey <> 42`) get the same noise, so
their difference is that entity's contribution in the released world. Change `simd_asa.seed`
between statements when that matters.

## Automatic rewrite

PostgreSQL extensions cannot add DDL syntax, so the DuckDB statements that declare the sampling
entity and its links become function calls:

| DuckDB | PostgreSQL |
|---|---|
| `ALTER TABLE customer ADD PRIVACY_KEY (c_custkey); ALTER TABLE customer SET PU;` | `SELECT asa_add_entity('customer', 'c_custkey');` |
| `ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer (c_custkey);` | `SELECT asa_add_link('orders', 'o_custkey', 'customer', 'c_custkey');` |

(`asa_drop_entity`, `asa_drop_link`; the metadata lives in the tables `asa_entity` and
`asa_link` and is included in `pg_dump`.) Links need not be enforced foreign keys.

The rewrite is the PostgreSQL counterpart of the DuckDB extension's query rewriter. It works
level by level (statement, `FROM`-clause subquery, CTE, sub-select):

1. **Masks.** The entity-dependent relations are the entity and every relation with a unique
   link path to it (others, e.g. `nation`, are shared by all worlds). Each one's entity key
   is found through a join the level already has (`l_orderkey = o_orderkey`), through an
   injected join when it is missing (`lineitem` alone gets `orders`), or directly when the
   link points at the entity (`o_custkey`). The row mask is the AND over the distinct keys of
   `priv_hash(asa_hash(key), query_hash)`: a row joined from two customers belongs to the
   worlds containing both, hence the [correction](#release). Keys equal by a join condition
   are one customer (`a.o_custkey = b.o_custkey`), as is a key a subquery exposes along with
   its mask.
2. **Aggregates.** `sum`/`count`/`avg`/`min`/`max` become 64-world aggregates of the same result
   type. An arithmetic expression over several aggregates is evaluated per world and released
   once (`asa_lane_*`, then `as_noised`: TPC-H Q8, Q14); other expressions — `round(...)`,
   `sum(x) > 10`, `/ 7.0` — apply to the released values. `count/sum/avg(DISTINCT v)` count
   each distinct value in the worlds of any of its rows (`asa_*_distinct`, DuckDB's `bit_or`
   pre-aggregation); `min/max(DISTINCT v)` are `min/max(v)`.
3. **Subqueries.** A `FROM`-clause subquery that does not aggregate passes its row mask up as
   an extra column (Q7, Q8, Q9). One that aggregates is released in place, e.g. a CTE (Q15).
   One grouped by the entity key, or by a column that determines the entity (`l_orderkey`),
   aggregates single entities: it stays exact, and its key is the mask of the aggregate above
   (Q13; Q18's `IN` sub-select). `EXISTS` / `IN` sub-selects without such aggregation are
   ordinary filters (Q4, Q21, Q22), as in DuckDB.
4. **Predicates over worlds.** A `WHERE` comparison with a scalar sub-select that aggregates
   over entity-dependent rows (Q17's `l_quantity < (SELECT 0.2 * avg(l_quantity) ...)`) holds
   in some worlds only: the sub-select returns its 64 world values and the comparison becomes
   the mask of the worlds where it holds (`asa_lane_lt`). Below an aggregate the mask is AND-ed
   into the row mask (DuckDB's `priv_select`: Q17, Q22) and the level's aggregates are
   released through their 64 answers (DuckDB's categorical rewrite); on rows that are not
   aggregated it decides whether the row passes (`priv_filter`: by majority without noise,
   with probability popcount/64 with noise; Q20). `HAVING` over aggregates is decided the
   same way per group.
5. **Top-k.** `ORDER BY ... LIMIT` over aggregates ranks the groups by the mean of their
   worlds and releases only the selected ones (DuckDB's top-k pushdown: Q3, Q10, Q21;
   `simd_asa.pushdown_topk`).

Several candidate entities can be declared; `SET simd_asa.entity = 'orders'` selects one (a
single declared entity is the default). Queries over entity-independent
relations, queries without aggregates over the entity and queries that already use this
extension's functions are left unchanged.

The rewrite runs at parse analysis (`post_parse_analyze_hook`), so result types are consistent
for prepared statements and the extended protocol. Shapes it does not implement are rejected
with an error rather than answered with ordinary one-world results: user-defined functions
or aggregates in entity-dependent queries, `ORDER BY` inside aggregates, window functions
and grouping sets over entity-dependent rows, set operations, views and
non-aggregating CTEs over entity-dependent relations (use a subquery), entity-dependent
relations on the nullable side of an outer join (outside a subquery grouped by the entity),
and `IN` / `EXISTS` sub-selects that aggregate other than per entity. Per-world arithmetic is
floating point (integer division does not truncate).

Unlike the DuckDB rewriter, this one does not restrict which columns may be returned: DuckDB
rejects TPC-H Q10, Q18 and Q22 (customer columns outside aggregates) and, with `priv_check`, Q3
(grouping by a key); here they run as DuckDB's hand-written versions of them do.

The hook must be loaded in the session (`LOAD 'simd_asa'`, or `session_preload_libraries` /
`shared_preload_libraries`).

## Using the aggregates directly

The rewrite only generates calls to the functions below, which can also be called by hand.
`as_` stands for *accelerated stochastic*: the 64-world aggregates, named as in the DuckDB
extension, as are the `priv_` functions; `asa_` are the helpers specific to this port. A
64-world answer is a `real[]` of 64 values, one per world.

| Function | Returns | |
|---|---|---|
| `as_count(mask bigint)` | `real[64]` | rows per world |
| `as_count(mask bigint, value "any")` | `real[64]` | non-NULL values per world |
| `as_sum(mask bigint, value)` | `real[64]` | sum per world; `smallint`, `integer`, `bigint`, `real`, `double precision`, `numeric` |
| `as_avg(mask bigint, value)` | `real[64]` | average per world (NULL for empty worlds); same types |
| `as_min(mask bigint, value)` / `as_max(...)` | `real[64]` | extreme per world; same types |
| `as_sum / as_count / as_min / as_max(answers real[])` | `real[64]` | re-aggregate 64-world answers (nested aggregation) |
| `as_noised_count/sum/avg/min/max(mask, [value])` | type of `count`/`sum`/... | the [release](#release) of the 64 worlds |
| `as_noised(answers real[])` | `double precision` | release of a 64-world answer (e.g. an expression evaluated per world) |
| `priv_hash(key_hash bigint [, query_hash bigint])` | `bigint` | mask of an entity: the worlds it belongs to |
| `asa_hash(bigint)`, `asa_hash(integer)` | `bigint` | DuckDB's `hash()` on integers |
| `priv_div(real[], real[])` | `real[]` | element-wise division, NULL where the denominator is 0 or NULL |
| `asa_lane_add/sub/mul/div(a, b)`, `asa_lane_neg(a)` | `real[]` | per-world arithmetic; `a`, `b`: `real[]` or `double precision` (broadcast); `/ 0` is NULL |
| `asa_lane_lt/le/gt/ge/eq/ne(a, b)` | `bigint` | mask of the worlds where the comparison holds |
| `priv_filter(mask bigint)` | `boolean` | whether a row passes a predicate that holds in the worlds of `mask` |
| `priv_mean(answers real[])` | `double precision` | mean of the non-NULL worlds |
| `asa_query_hash()` | `bigint` | the query hash of `priv_hash(bigint)` and `as_noised_*` |
| `asa_noised_*` / `asa_counters_*(mask, [value], query_hash, correction)` | release / `real[64]` | the forms the rewrite uses: explicit query hash and [correction](#release) |
| `asa_noised/counters_count/sum/avg_distinct(mask, value, query_hash, correction ORDER BY value)` | release / `real[64]` | `count/sum/avg(DISTINCT value)`: a distinct value is in the worlds of any row carrying it |

Semantics are those of the DuckDB extension, so results can be compared across systems:

- `as_count` / `as_sum` report `2 × aggregate` per world: each entity is in a world with
  probability ½, so the factor 2 makes every world an unbiased estimate of the full-data
  aggregate (this is also the BRR replicate rule, *2 × flag × weight*).
  `as_min` / `as_max` / `as_avg` are not scaled.
- A world that received no row is `0` (`as_count`, `as_sum`, `as_min`, `as_max`) or
  `NULL` (`as_avg`, re-aggregation overloads).
- `priv_hash(asa_hash(k))` returns the same mask as DuckDB's `priv_hash(hash(k))` for
  `BIGINT`/`INTEGER` keys, with DuckDB's seed set to `simd_asa.seed` and its noise off.
  Any other 64-bit value can be used as a mask, e.g. packed replicate flags.

TPC-H Q1 by hand, 64 answers per aggregate, each line item in the worlds of its order's
customer:

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

Replace `as_sum` by `as_noised_sum` (and so on) for the released value. Survey replicate
weights (e.g. MEPS's 128 BRR replicates = two 64-bit blocks) work the same way: pack the
replicate flags into `mask_lo`/`mask_hi` and call `as_sum(mask_lo, weight * x)`,
`as_sum(mask_hi, weight * x)`.

Nested aggregation keeps the world dimension: inner per-group answers are re-aggregated world
by world.

```sql
SELECT as_max(per_nation) FROM (
    SELECT as_sum(priv_hash(asa_hash(c_custkey)), c_acctbal) AS per_nation
    FROM customer GROUP BY c_nationkey) t;
```

## Build, install, test

Requires PostgreSQL ≥ 16 development files (`pg_config`; tested on 16.15), a C compiler, and
a little-endian CPU.

```sh
make && make install                 # PGXS; builds with -O3
make installcheck                    # pg_regress suite against a running server (PGHOST/PGPORT/PGUSER)
```

The regression suite (`test/sql`) checks every world of every aggregate and input type
against the naive definition — 64 conditional `AGG(...) FILTER (WHERE bit j)` per group —
plus DuckDB hash compatibility, NULL/empty/extreme inputs, re-aggregation and parallel
(partial → serialize → combine) aggregation. `test/sql/noise.sql` checks the releases: world 0
without noise, result types, NULL groups, reproducibility, noise within its expected deviation,
the sample-diversity check and the filter probabilities. `test/sql/rewrite.sql` checks the
rewrite before the release — every rewritten query must return exactly the
hand-written `as_*` query's result (entity table, direct link, injected join, reused joins, two
entities and their correction, `FILTER`, `ORDER BY`, prepared statements, entity selection) —
and the rejected shapes. `test/sql/release.sql` checks the default releases the same way
against hand-written `as_noised_*` queries: expressions, derived tables, per-entity subqueries,
CTEs, predicates over worlds as masks and as filters, `HAVING`, top-k, parallel query and
prepared statements. `test/sql/distinct.sql` checks every world of the `DISTINCT` aggregates
against the definition (the aggregate over the distinct values of the rows in world *j*) and
their rewrite, `HAVING` and top-k included.

`bench/tpch_crosscheck.py` runs the 22 TPC-H queries (the texts DuckDB ships,
`bench/tpch_queries`) through the rewrite without noise and compares them with the DuckDB
extension's results (`bench/duckdb_reference.sh`: its automatic rewrite without noise, or its
hand-written queries where its rewriter rejects the query). All 22 agree (values within 10⁻⁵
relative). Q15 is checked differently: DuckDB's rewrite of its CTE releases different values
depending on how the CTE is read (per supplier, under `max()`, under `ORDER BY ... LIMIT`), so
DuckDB's answer is not its own largest released revenue; PostgreSQL's is. It also runs four
`DISTINCT` queries over TPC-H (`bench/tpch_distinct`, against DuckDB's rewrite: all agree) and
checks the correction on a self-join of orders of two different customers, which DuckDB cannot
answer the same way (it masks a self-join by one of the two customers): the corrected counts
are within 2% of the exact ones (0.8% at SF1; uncorrected they would be half).

```sh
bench/duckdb_reference.sh <duckdb binary> tpch1.duckdb <duckdb extension repo> /tmp/ref
bench/tpch_crosscheck.py "psql -h localhost -U postgres" /tmp/ref
```

`test/crosscheck_duckdb.sh` cross-validates against the DuckDB extension on generated
data (2M rows × 100 groups × 64 worlds: identical masks; identical counts, min/max,
small-integer sums and averages; ≤ 1.2·10⁻⁷ relative difference for large `bigint` sums,
whose levels are summed in `double` here and in `float` in DuckDB; ≤ 1.2·10⁻⁶ for
floating-point sums, whose `float` accumulation is order-dependent):

```sh
DUCKDB=<duckdb binary> PRIVACY_EXT=<extension .duckdb_extension file> \
PSQL="psql -h localhost -U postgres" test/crosscheck_duckdb.sh 2000000
```

`bench/tpch_q1.py` compares one pass over 64 worlds with writing the 64 worlds out in SQL:
TPC-H Q1 (1 world), Q1 with `as_*` (64 worlds) and Q1 as conditional SQL (64 worlds, 512
`FILTER` aggregates), after validating the `as_*` worlds against the conditional ones. TPC-H
SF1 (6M line items, `numeric(15,2)` columns as in the standard schema), PostgreSQL 16.15,
Apple M1, JIT off, median of 3 runs after a warm-up:

| Variant | Worlds | Aggregates | Serial | 4 workers |
|---|---:|---:|---:|---:|
| Q1 | 1 | 8 | 3.58 s | 0.92 s |
| Q1, `as_*` | 64 | 8 | 6.08 s | 1.52 s |
| Q1, conditional SQL | 64 | 512 | 131.5 s | 32.4 s |

The `as_*` aggregates compute the 64 worlds 21× faster than the conditional-SQL formulation
and at 1.7× the cost of the single-world query, and scale with parallel workers like the
ordinary aggregates. All worlds match the conditional ones (counts exact, sums ≤ 8.8·10⁻⁶
relative, averages ≤ 1.1·10⁻⁷). Part of the remaining overhead is the per-row `numeric` →
`double` conversion of the inputs.

## Design

The aggregate kernels are ported from the DuckDB extension, keeping their physical state
layouts:

| Aggregate | State (per group) |
|---|---|
| `as_count` | SWAR: 8 × `uint64` = 64 packed `uint8` counters; cascaded into lazily allocated `uint64[64]` every 255 updates |
| `as_sum` (integers) | up to 25 lazily allocated levels of 64 × `uint16` counters (4 per `uint64`), staggered by 4 bits; first level stored inline; lossless cascading; separate state for negated negative values |
| `as_sum` (floating point, `numeric`) | `float[64]`, predicated add (`v × bit`) |
| `as_noised_sum` (`numeric`) | the integer state on value × 10^scale |
| `as_min` / `as_max` | `T[64]` in SWAR-interleaved order, branch-free bit-select update, global-bound pruning refreshed every 2048 updates |
| `as_avg` | sum state + count state (DuckDB rewrites `as_avg` into `priv_div(as_sum, as_count)`) |
| re-aggregation | `float[64]` + `uint64[64]` counts + seen-mask |

What changes for PostgreSQL is the integration, not the kernels:

- **Aggregate API.** Each kernel is wrapped as an `internal`-state aggregate with
  transition, final, combine, serialize and deserialize functions, so it participates in
  hash/sort aggregation, spilling and parallel partial aggregation unchanged. State is
  allocated lazily in the aggregate memory context (DuckDB: arena allocator).
- **Row-at-a-time transitions.** PostgreSQL calls the transition function once per row,
  so the DuckDB buffering wrappers (which delay state allocation during vectorized
  scatter updates) are unnecessary and are not ported. The 64-world update itself is
  still a short, branch-free, auto-vectorized loop.
- **Metadata and rewriting.** Entity metadata is kept in extension tables instead of DDL, and
  the rewrite is a parse-analysis hook instead of DuckDB's optimizer extension (FK join
  injection, aggregate replacement, and the rewrite of expressions and predicates over
  worlds — DuckDB's `list_transform` lambdas become `asa_lane_*` calls; `src/asa_rewrite.c`).
  Enforced foreign keys are not required. DuckDB passes the hash column through its logical
  plan; here a subquery passes its mask up as an output column, and the top-k pushdown wraps
  the query in a subquery that is limited before the release.
- **Query hash.** DuckDB's bind data carries the query hash to every aggregate; here the
  rewrite embeds it as a constant argument, which the transition functions of the noised
  forms store in their state (it survives partial aggregation and serialization).

Deviations from the DuckDB code:

- A world that only received negative integers is reported, and integer magnitudes
  ≥ 2⁶³ (`INT64_MIN`) are routed by unsigned arithmetic. Both were bugs in the DuckDB code,
  since fixed there.
- 64-world answers of `numeric` use the floating-point path, as DuckDB's `as_sum` lists of
  `DECIMAL`; the noised forms use the integer path, as DuckDB's fused `as_noised_sum`.
- A correlated sub-select over no rows returns NULL answers (`asa_counters_*`), as DuckDB's
  decorrelated sub-selects do; `as_sum` itself returns 64 zeros there, as DuckDB's.
- The noise generator is PostgreSQL's (`pg_prng`), not DuckDB's PCG32 streams: noised values
  are distributed as in DuckDB, not identical to them. Every group of a statement draws from one
  generator, in the order the groups are finalized (DuckDB: a stream per finalized batch). Both
  repeat exactly only serially (`max_parallel_workers_per_gather = 0`, DuckDB `threads = 1`).
  The query hash is always the seed's, as DuckDB's with an explicit seed (without one, DuckDB
  mixes in the query id).
- `DISTINCT` aggregates sort their input by the value (an ordered aggregate, as PostgreSQL's
  own `count(DISTINCT)`) instead of pre-aggregating with `bit_or`; the values each feed the
  base aggregate once with their OR-ed mask, so the releases are DuckDB's. Like
  PostgreSQL's `DISTINCT` aggregates they are not parallelized. As in DuckDB they take no
  correction.
- The correction is static: *k* counts the distinct keys of the level, so in a self-join
  without an equality on the key, pairs that happen to share a customer weigh double (their
  masks are equal: in a world with probability ½, not ¼). DuckDB derives one hash per entity
  table, so its self-joins mask by one of the two customers instead.
- The DuckDB extension's other releases (differential privacy, sample-and-aggregate) and options
  (clipping, minimum group size, *m* ≠ 64) are not ported.

## License

AGPL-3.0, as the DuckDB extension it is derived from.
