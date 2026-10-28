# simd_asa — SIMD-accelerated stochastic aggregation for PostgreSQL

A PostgreSQL extension that ports the aggregate engine of
[SIMD-ASA](https://github.com/cwida/privacy) (DuckDB) to PostgreSQL. One aggregate
call maintains **64 world-specific aggregates**: each input row carries a 64-bit
membership mask, bit *j* set means "this row belongs to world *j*", and the result is
a `real[]` holding one answer per world. Scans, joins, filters and grouping are shared
across the 64 worlds; only the world-specific aggregate state is widened.

No noise and no privacy mechanism: this is the stochastic-aggregation layer. Queries either
use the `as_*` aggregates directly, or are ordinary SQL rewritten automatically from
declared ownership metadata ([Automatic rewrite](#automatic-rewrite)).

## Functions

| Function | Returns | |
|---|---|---|
| `as_count(mask bigint)` | `real[64]` | rows per world |
| `as_count(mask bigint, value "any")` | `real[64]` | non-NULL values per world |
| `as_sum(mask bigint, value)` | `real[64]` | sum per world; `smallint`, `integer`, `bigint`, `real`, `double precision`, `numeric` |
| `as_avg(mask bigint, value)` | `real[64]` | average per world (NULL for empty worlds); same types |
| `as_min(mask bigint, value)` / `as_max(...)` | `real[64]` | extreme per world; same types |
| `as_sum / as_count / as_min / as_max(counters real[])` | `real[64]` | re-aggregate counter arrays (nested aggregation) |
| `priv_div(real[], real[])` | `real[]` | element-wise division, NULL where the denominator is 0 or NULL |
| `priv_hash(key_hash bigint)` | `bigint` | membership mask of a sampling entity |
| `asa_hash(bigint)`, `asa_hash(integer)` | `bigint` | DuckDB's `hash()` on integers |

Settings: `simd_asa.seed` (default 42) re-draws the assignment of entities to worlds;
`simd_asa.hash_repair` (default off) repairs every mask to exactly 32 set bits.

Semantics are those of the DuckDB extension, so results can be compared across systems:

- `as_count` / `as_sum` report `2 × aggregate` per world: a uniform 64-bit mask includes
  each entity with probability ½, so the factor 2 makes every world an unbiased estimate
  of the full-data aggregate (this is also the BRR replicate rule, *2 × flag × weight*).
  `as_min` / `as_max` / `as_avg` are not scaled.
- A world that received no row is `0` (`as_count`, `as_sum`, `as_min`, `as_max`) or
  `NULL` (`as_avg`, re-aggregation overloads).
- `priv_hash(asa_hash(k))` returns the same mask as DuckDB's `priv_hash(hash(k))` for
  `BIGINT`/`INTEGER` keys (DuckDB: `SET pac_mi = 0`, `privacy_seed` = `simd_asa.seed`).
  Any other 64-bit value can be used as a mask, e.g. packed replicate flags.

## Example

TPC-H Q1 over 64 worlds, with `ORDERS` as the sampling entity (every line item inherits
its order's membership):

```sql
CREATE EXTENSION simd_asa;

SELECT l_returnflag, l_linestatus,
       as_sum(m, l_extendedprice * (1 - l_discount)) AS revenue,  -- 64 sums
       as_avg(m, l_quantity)                          AS avg_qty,  -- 64 averages
       as_count(m)                                    AS orders    -- 64 counts
FROM (SELECT *, priv_hash(asa_hash(l_orderkey)) AS m FROM lineitem
      WHERE l_shipdate <= date '1998-12-01' - interval '90' day) s
GROUP BY 1, 2;
```

Survey replicate weights (e.g. MEPS 128 BRR replicates = two 64-bit blocks): pack the
replicate flags into `mask_lo`/`mask_hi` and call `as_sum(mask_lo, weight * x)`,
`as_sum(mask_hi, weight * x)`. Arbitrary *m* uses ⌈m/64⌉ masks.

Nested aggregation keeps the world dimension: inner per-group counter arrays are
re-aggregated world by world.

```sql
SELECT as_max(per_nation) FROM (
    SELECT as_sum(priv_hash(asa_hash(c_custkey)), c_acctbal) AS per_nation
    FROM customer GROUP BY c_nationkey) t;
```

## Automatic rewrite

Declare the candidate sampling entities and the ownership links once. PostgreSQL extensions
cannot add DDL syntax, so the DuckDB statements become function calls:

| DuckDB | PostgreSQL |
|---|---|
| `ALTER TABLE customer ADD PRIVACY_KEY (c_custkey); ALTER TABLE customer SET PU;` | `SELECT asa_add_entity('customer', 'c_custkey');` |
| `ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer (c_custkey);` | `SELECT asa_add_link('orders', 'o_custkey', 'customer', 'c_custkey');` |

(`asa_drop_entity`, `asa_drop_link`; the metadata lives in the tables `asa_entity` and
`asa_link` and is included in `pg_dump`.) Links need not be enforced foreign keys. Then
ordinary aggregate SQL returns one result per world:

```sql
SELECT asa_add_entity('customer', 'c_custkey');
SELECT asa_add_link('orders', 'o_custkey', 'customer', 'c_custkey');
SELECT asa_add_link('lineitem', 'l_orderkey', 'orders', 'o_orderkey');

SET simd_asa.rewrite = on;
SELECT l_returnflag, sum(l_quantity), avg(l_discount), count(*)   -- each column: real[64]
FROM lineitem GROUP BY 1;
```

For each aggregate query over relations owned by the selected entity, the rewrite

1. finds the entity-dependent relations in `FROM`: the entity and every relation with a
   unique link path to it (the other relations, e.g. `nation`, are shared by all worlds);
2. derives each one's entity key by following its links, through joins the query already
   has (`l_orderkey = o_orderkey`), joining the linked relation in only when it is missing
   (`lineitem` alone gets a join to `orders`), and with no join at all when the link points
   at the entity (`o_custkey` is the customer key);
3. computes the mask `priv_hash(asa_hash(key))`, AND-ing the masks of different entities
   (a tuple joined from two customers belongs to the worlds containing both);
4. replaces `sum`/`count`/`avg`/`min`/`max` by `as_sum`/`as_count`/`as_avg`/`as_min`/`as_max`.

Several candidate entities can be declared; `SET simd_asa.entity = 'orders'` selects the one a
query samples (with a single declared entity it is used by default). `EXPLAIN` shows the
rewritten plan. Queries over sample-independent relations, queries without aggregates and
queries that already use `as_*` aggregates are left unchanged.

The rewrite runs at parse analysis (`post_parse_analyze_hook`), so the new result types are
consistent for prepared statements and the extended protocol. Consequently the result arrays
cannot be post-processed in the same statement (it was type-checked before the rewrite);
store them with `CREATE TABLE AS` or write that query with `as_*` directly. Shapes whose world
semantics are not implemented are rejected with an error rather than answered with ordinary
one-world results: aggregates in `HAVING` or inside expressions (`sum(a) / sum(b)`),
`DISTINCT` aggregates, entity-dependent relations in subqueries, CTEs or views, on the
nullable side of an outer join, window functions and grouping sets.

The hook must be loaded in the session (`LOAD 'simd_asa'`, or `session_preload_libraries` /
`shared_preload_libraries`); the rewrite requires PostgreSQL ≥ 16.

## Build, install, test

Requires PostgreSQL ≥ 14 development files (`pg_config`) — ≥ 16 for the automatic
rewrite — a C compiler, and a little-endian CPU.

```sh
make && make install                 # PGXS; builds with -O3
make installcheck                    # pg_regress suite against a running server (PGHOST/PGPORT/PGUSER)
```

The regression suite (`test/sql`) checks every world of every aggregate and input type
against the naive definition — 64 conditional `AGG(...) FILTER (WHERE bit j)` per group —
plus DuckDB hash compatibility, NULL/empty/extreme inputs, counter re-aggregation and
parallel (partial → serialize → combine) aggregation. `test/sql/rewrite.sql` checks the
automatic rewrite: every rewritten query must return exactly the hand-written `as_*` query's
result (entity table, direct link, injected join, reused joins, two entities, `FILTER`,
`ORDER BY`, prepared statements, entity selection), and every unsupported shape is rejected.

`test/crosscheck_duckdb.sh` cross-validates against the DuckDB extension on generated
data (2M rows × 100 groups × 64 worlds: identical masks; identical counts, min/max,
small-integer sums and averages; ≤ 1.2·10⁻⁷ relative difference for large `bigint` sums,
whose levels are summed in `double` here and in `float` in DuckDB; ≤ 1.2·10⁻⁶ for
floating-point sums, whose `float` accumulation is order-dependent):

```sh
DUCKDB=../privacy/build/release/duckdb \
PRIVACY_EXT=../privacy/build/release/extension/privacy/privacy.duckdb_extension \
PSQL="psql -h localhost -U postgres" test/crosscheck_duckdb.sh 2000000
```

`bench/tpch_q1.py` times TPC-H Q1 (1 world), Q1 with SIMD-ASA (64 worlds) and Q1 as
conditional SQL (64 worlds, 512 `FILTER` aggregates), after validating the SIMD-ASA worlds
against the conditional ones.

TPC-H SF1 (6M line items, `numeric(15,2)` columns as in the standard schema), PostgreSQL
16.15, Apple M1, JIT off, median of 3 runs after a warm-up:

| Variant | Worlds | Aggregates | Serial | 4 workers |
|---|---:|---:|---:|---:|
| Q1 | 1 | 8 | 3.58 s | 0.92 s |
| Q1, SIMD-ASA | 64 | 8 | 6.08 s | 1.52 s |
| Q1, conditional SQL | 64 | 512 | 131.5 s | 32.4 s |

SIMD-ASA computes the 64 worlds 21× faster than the single-scan conditional-SQL
formulation (21.7× serial, 21.3× parallel) and at 1.7× the cost of the single-world
query; it scales with parallel workers like the ordinary aggregates. All SIMD-ASA worlds match the
conditional ones (counts exact, sums ≤ 8.8·10⁻⁶ relative, averages ≤ 1.1·10⁻⁷). Part of the
remaining overhead is the per-row `numeric` → `double` conversion of the inputs.

`bench/tpch_rewrite.py` runs TPC-H Q1, Q3, Q5, Q6, Q10 and Q12 as ordinary SQL through the
automatic rewrite (`CUSTOMER` as the entity, `ORDERS` and `LINEITEM` linked). Every rewritten
query returns exactly the hand-written `as_*` query's result, and the rewrite adds no runtime
(SF1, 4 workers, median of 3):

| Query | 1 world (ms) | rewritten, 64 worlds (ms) | hand-written `as_*` (ms) |
|---|---:|---:|---:|
| Q1 | 876.8 | 2160.6 | 2147.5 |
| Q3 | 270.4 | 274.7 | 273.9 |
| Q5 | 273.5 | 272.4 | 273.1 |
| Q6 | 184.8 | 241.1 | 240.8 |
| Q10 | 391.6 | 392.7 | 394.0 |
| Q12 | 266.7 | 266.3 | 267.3 |

Q1 and Q6 read only `LINEITEM`, so the rewrite joins `ORDERS` in to reach the customer; the
others already contain the joins and cost the same as the one-world query.

## Design

The aggregate kernels are ported from the DuckDB extension, keeping their physical state
layouts:

| Aggregate | State (per group) | DuckDB source |
|---|---|---|
| `as_count` | SWAR: 8 × `uint64` = 64 packed `uint8` counters; cascaded into lazily allocated `uint64[64]` every 255 updates | `as_count.hpp` (`PacCountState`) |
| `as_sum` (integers) | up to 25 lazily allocated levels of 64 × `uint16` counters (4 per `uint64`), staggered by 4 bits; first level stored inline; lossless cascading; separate state for negated negative values | `as_sum.hpp` (`PacSumIntState`, approximate mode) |
| `as_sum` (floating point, `numeric`) | `float[64]`, predicated add (`v × bit`) | `as_sum.hpp` (`PacSumDoubleState`) |
| `as_min` / `as_max` | `T[64]` in SWAR-interleaved order, branch-free bit-select update, global-bound pruning refreshed every 2048 updates | `as_min_max.hpp` (`PacMinMaxState`) |
| `as_avg` | sum state + count state (DuckDB rewrites `as_avg` into `priv_div(as_sum, as_count)`) | `pac_avg_rewriter.cpp` |
| counter re-aggregation | `float[64]` + `uint64[64]` counts + seen-mask | `pac_categorical.cpp` (`PacListAggregateState`) |

What changes for PostgreSQL is the integration, not the kernels:

- **Aggregate API.** Each kernel is wrapped as an `internal`-state aggregate with
  transition, final, combine, serialize and deserialize functions, so it participates in
  hash/sort aggregation, spilling and parallel partial aggregation unchanged. State is
  allocated lazily in the aggregate memory context (DuckDB: arena allocator).
- **Row-at-a-time transitions.** PostgreSQL calls the transition function once per row,
  so the DuckDB buffering wrappers (which delay state allocation during vectorized
  scatter updates) are unnecessary and are not ported. The 64-world update itself is
  still a short, branch-free, auto-vectorized loop.
- **Metadata and rewriting.** Ownership metadata is kept in extension tables instead of
  DDL, and the rewrite is a parse-analysis hook instead of DuckDB's optimizer extension
  (FK join injection + aggregate transformation, `src/asa_rewrite.c`). Enforced foreign
  keys are not required. Not yet ported: the categorical rewrite of predicates over
  worlds (`HAVING`, correlated subqueries such as Q17) and lane-wise expressions over
  aggregates.

Deviations from the DuckDB code:

- A world that only received negative integers is reported, and integer magnitudes
  ≥ 2⁶³ (`INT64_MIN`) are routed by unsigned arithmetic. Both were bugs in the DuckDB code,
  fixed there in [cwida/privacy#29](https://github.com/cwida/privacy/pull/29).
- `numeric` uses the floating-point path (DuckDB `DECIMAL` uses the integer path).
- PAC/DP-specific parts are omitted: noise terminals (`as_noised_*`), the `correction`
  argument, the sample-diversity guard, clipping (`as_clip_*`) and sample-and-aggregate
  (`as_sample_*`) variants. Only *m* = 64 per call.

## License

AGPL-3.0, as the DuckDB extension it is derived from.
