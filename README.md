# simd_asa — SIMD-accelerated stochastic aggregation for PostgreSQL

A PostgreSQL extension that ports the aggregate engine of
[SIMD-ASA](https://github.com/cwida/privacy) (DuckDB) to PostgreSQL. One aggregate
call maintains **64 world-specific aggregates**: each input row carries a 64-bit
membership mask, bit *j* set means "this row belongs to world *j*", and the result is
a `real[]` holding one answer per world. Scans, joins, filters and grouping are shared
across the 64 worlds; only the world-specific aggregate state is widened.

This is the engine layer only: no noise, no privacy mechanism, no query rewriter.
Queries are written against the `as_*` aggregates directly (see [Design](#design)).

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

## Build, install, test

Requires PostgreSQL ≥ 14 development files (`pg_config`), a C compiler, little-endian CPU.

```sh
make && make install                 # PGXS; builds with -O3
make installcheck                    # pg_regress suite against a running server (PGHOST/PGPORT/PGUSER)
```

The regression suite (`test/sql`) checks every world of every aggregate and input type
against the naive definition — 64 conditional `AGG(...) FILTER (WHERE bit j)` per group —
plus DuckDB hash compatibility, NULL/empty/extreme inputs, counter re-aggregation and
parallel (partial → serialize → combine) aggregation.

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

| Variant | Worlds | Aggregates | Serial |
|---|---:|---:|---:|
| Q1 | 1 | 8 | 3.58 s |
| Q1, SIMD-ASA | 64 | 8 | 6.08 s |
| Q1, conditional SQL | 64 | 512 | 131.5 s |

SIMD-ASA computes the 64 worlds 21.7× faster than the single-scan conditional-SQL
formulation and at 1.7× the cost of the single-world query. All SIMD-ASA worlds match the
conditional ones (counts exact, sums ≤ 8.8·10⁻⁶ relative, averages ≤ 1.1·10⁻⁷). Part of the
remaining overhead is the per-row `numeric` → `double` conversion of the inputs.

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
- **Metadata and rewriting.** Ownership metadata (`PRIVACY_LINK`) and the automatic
  query rewriter are not ported: masks are computed explicitly (`priv_hash(...)` on the
  sampling entity's key, propagated through joins by joining to it). A rewriter would be a
  `planner_hook` that adds the mask projection and replaces aggregates, as DuckDB's
  optimizer extension does; enforced foreign keys are not required.

Deviations from the DuckDB code:

- Fixed: a world that only received negative integers is reported (the DuckDB counter
  finalize takes the world mask from the positive state only and returns 0), and integer
  magnitudes ≥ 2⁶³ (`INT64_MIN`) are routed by unsigned arithmetic (DuckDB routes them to
  level 0 and loses them).
- `numeric` uses the floating-point path (DuckDB `DECIMAL` uses the integer path).
- PAC/DP-specific parts are omitted: noise terminals (`as_noised_*`), the `correction`
  argument, the sample-diversity guard, clipping (`as_clip_*`) and sample-and-aggregate
  (`as_sample_*`) variants. Only *m* = 64 per call.

## License

AGPL-3.0, as the DuckDB extension it is derived from.
