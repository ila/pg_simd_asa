# simd_asa — SIMD-accelerated stochastic aggregation for PostgreSQL

A PostgreSQL extension that ports the aggregate engine of
[SIMD-ASA](https://github.com/cwida/privacy) (DuckDB) to PostgreSQL. One aggregate
call maintains **64 world-specific aggregates**: each input row carries a 64-bit
membership mask, bit *j* set means "this row belongs to world *j*", and the result is
a `real[]` holding one answer per world. Scans, joins, filters and grouping are shared
across the 64 worlds; only the world-specific aggregate state is widened.

On top, the PAC noise release of the DuckDB extension turns the 64 worlds of a cell into one
noised value (`as_noised_*`, [Noise](#noise)); the DP mechanisms are not ported. Queries
either use these aggregates directly, or are ordinary SQL rewritten automatically from
declared ownership metadata ([Automatic rewrite](#automatic-rewrite)): all 22 TPC-H queries
run unchanged and return what the DuckDB extension returns.

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
| `asa_lane_add/sub/mul/div(a, b)`, `asa_lane_neg(a)` | `real[]` | per-world arithmetic; `a`, `b`: `real[]` or `double precision` (broadcast); `/ 0` is NULL |
| `asa_lane_lt/le/gt/ge/eq/ne(a, b)` | `bigint` | mask of the worlds where the comparison holds |
| `priv_hash(key_hash bigint [, query_hash bigint])` | `bigint` | membership mask of a sampling entity |
| `asa_hash(bigint)`, `asa_hash(integer)` | `bigint` | DuckDB's `hash()` on integers |
| `as_noised_count/sum/avg/min/max(mask, [value])` | type of `count`/`sum`/... | PAC release of the 64 worlds ([Noise](#noise)) |
| `as_noised(counters real[])` | `double precision` | PAC release of a counter array (e.g. an expression evaluated per world) |
| `priv_filter(mask bigint)` | `boolean` | whether a row passes a predicate that holds in the worlds of `mask` |
| `priv_mean(counters real[])` | `double precision` | mean of the non-NULL worlds |
| `asa_query_hash()` | `bigint` | the query hash of `priv_hash(bigint)` and `as_noised_*` |

Settings: `simd_asa.seed` (default 42) re-draws the assignment of entities to worlds;
`simd_asa.hash_repair` (default off) repairs every mask to exactly 32 set bits. The noise
settings are listed under [Noise](#noise).

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

## Noise

`as_noised_count/sum/avg/min/max` release one value per group, of the type the PostgreSQL
aggregate returns (`count` → `bigint`, `sum(integer)` → `bigint`, `sum(numeric)` → `numeric`,
...), as DuckDB's PAC terminals do (`PacNoiseInNull`, `PacNoisySampleFrom64Counters`):

1. **NULL decision.** A cell is NULL with probability popcount(¬mask)/64, `mask` being the OR
   of its rows' masks: cells few entities contribute to are likely suppressed.
2. **Value.** World *J* = query hash mod 64 (the same secret world for all cells of a
   statement), plus Gaussian noise of variance σ² / (2·mi), σ² the variance of the 64
   world values. With p-tracking (default) the variance is weighted by a Bayesian posterior
   over the worlds that every release of the statement updates, so the noise accounts for
   what earlier cells revealed.
3. `as_noised_avg` releases a noised sum and a noised count and divides them (DuckDB's AVG
   rewrite). `numeric` sums are computed as DuckDB sums `DECIMAL`: integers times
   10^scale on the integer kernel, the scale derived from the argument's type.

| Setting | Default | |
|---|---|---|
| `simd_asa.noise` | `on` | `off`: deterministic releases — world 0, NULL only for cells in no world (DuckDB's `pac_mi = 0`) |
| `simd_asa.mi` | 1/128 | mutual-information bound (`pac_mi`); lower = more noise; 0 = no noise |
| `simd_asa.ptracking` | `on` | p-tracking (`pac_ptracking`) |
| `simd_asa.sample_diversity_check` | `on` | reject noised aggregates whose groups mostly hold a single entity (`pac_sample_diversity_check`) |
| `simd_asa.seed` | 42 | with noise, the rewrite mixes a per-statement nonce into the seed: a counter if the seed was set (reproducible), else a random value |

Randomness comes from a per-statement generator seeded by the query hash; it and the
p-tracking posterior restart when a top-level statement starts, so re-executing a prepared
statement releases the same values instead of fresh noise that could be averaged out.

## Automatic rewrite

Declare the candidate sampling entities and the ownership links once. PostgreSQL extensions
cannot add DDL syntax, so the DuckDB statements become function calls:

| DuckDB | PostgreSQL |
|---|---|
| `ALTER TABLE customer ADD PRIVACY_KEY (c_custkey); ALTER TABLE customer SET PU;` | `SELECT asa_add_entity('customer', 'c_custkey');` |
| `ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer (c_custkey);` | `SELECT asa_add_link('orders', 'o_custkey', 'customer', 'c_custkey');` |

(`asa_drop_entity`, `asa_drop_link`; the metadata lives in the tables `asa_entity` and
`asa_link` and is included in `pg_dump`.) Links need not be enforced foreign keys. Then
ordinary SQL returns PAC-noised results of the usual types:

```sql
SELECT asa_add_entity('customer', 'c_custkey');
SELECT asa_add_link('orders', 'o_custkey', 'customer', 'c_custkey');
SELECT asa_add_link('lineitem', 'l_orderkey', 'orders', 'o_orderkey');

LOAD 'simd_asa';
SET simd_asa.rewrite = on;
SELECT l_returnflag, sum(l_quantity), avg(l_discount), count(*) FROM lineitem GROUP BY 1;
```

The rewrite is the PostgreSQL counterpart of DuckDB's PAC compiler. It works level by level
(statement, `FROM`-clause subquery, CTE, sub-select):

1. **Masks.** The entity-dependent relations are the entity and every relation with a unique
   link path to it (others, e.g. `nation`, are shared by all worlds). Each one's entity key
   is found through a join the level already has (`l_orderkey = o_orderkey`), through an
   injected join when it is missing (`lineitem` alone gets `orders`), or directly when the
   link points at the entity (`o_custkey`). The row mask is the AND over the distinct keys of
   `priv_hash(asa_hash(key), query_hash)`: a row joined from two customers belongs to the
   worlds containing both.
2. **Releases.** `sum`/`count`/`avg`/`min`/`max` become noised aggregates of the same result
   type. An arithmetic expression over several aggregates is evaluated per world and released
   once (`asa_lane_*`, then `as_noised`: TPC-H Q8, Q14); other expressions — `round(...)`,
   `sum(x) > 10`, `/ 7.0` — apply to the released values.
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
   released through their counters (DuckDB's categorical rewrite); on rows that are not
   aggregated it decides whether the row passes (`priv_filter`: by majority without noise,
   with probability popcount/64 with noise; Q20). `HAVING` over aggregates is decided the
   same way per group.
5. **Top-k.** `ORDER BY ... LIMIT` over aggregates ranks the groups by the mean of their
   worlds and releases only the selected ones (DuckDB's top-k pushdown: Q3, Q10, Q21;
   `simd_asa.pushdown_topk`).

`EXPLAIN` shows the rewritten plan. Several candidate entities can be declared;
`SET simd_asa.entity = 'orders'` selects one (a single declared entity is the default). With
`simd_asa.counters = on` the rewritten aggregates return their 64 world counters (`real[]`)
instead of a release; queries that need a release (HAVING, filters over worlds, aggregating
subqueries) are then rejected. Queries over sample-independent relations, queries without
aggregates over the entity and queries that already use this extension's functions are left
unchanged.

The rewrite runs at parse analysis (`post_parse_analyze_hook`), so result types are consistent
for prepared statements and the extended protocol. Shapes it does not implement are rejected
with an error rather than answered with ordinary one-world results: user-defined functions
or aggregates in entity-dependent queries, `DISTINCT` aggregates,
window functions and grouping sets over entity-dependent rows, set operations, views and
non-aggregating CTEs over entity-dependent relations (use a subquery), entity-dependent
relations on the nullable side of an outer join (outside a subquery grouped by the entity),
and `IN` / `EXISTS` sub-selects that aggregate other than per entity. Per-world arithmetic is
floating point (integer division does not truncate).

Unlike DuckDB's compiler, the rewrite has no protected-column policy: DuckDB rejects TPC-H
Q10, Q18 and Q22 (customer columns outside aggregates) and, with `priv_check`, Q3 (grouping
by a key); here they run as DuckDB's hand-written versions of them do.

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
parallel (partial → serialize → combine) aggregation. `test/sql/noise.sql` checks the
releases: world 0 without noise, result types, NULL cells, reproducibility, noise within its
expected deviation, the sample-diversity check and the filter probabilities.
`test/sql/rewrite.sql` checks the rewrite in counters mode — every rewritten query must return
exactly the hand-written `as_*` query's result (entity table, direct link, injected join,
reused joins, two entities, `FILTER`, `ORDER BY`, prepared statements, entity selection) —
and the rejected shapes. `test/sql/release.sql` checks the default releases the same way
against hand-written `as_noised_*` queries: expressions, derived tables, per-entity subqueries,
CTEs, predicates over worlds as masks and as filters, `HAVING`, top-k, parallel query and
prepared statements.

`bench/tpch_crosscheck.py` runs the 22 TPC-H queries (the texts DuckDB ships,
`bench/tpch_queries`) through the rewrite without noise and compares them with the DuckDB
extension's results (`bench/duckdb_reference.sh`: its automatic PAC rewrite with
`pac_mi = 0`, or its hand-written AS queries where its compiler rejects the query). All 22
agree (values within 10⁻⁵ relative). Q15 is checked differently: DuckDB's rewrite of its CTE
releases different values depending on how the CTE is read (per supplier, under `max()`,
under `ORDER BY ... LIMIT`), so DuckDB's answer is not its own largest released revenue;
PostgreSQL's is.

```sh
bench/duckdb_reference.sh ../privacy/build/release/duckdb tpch1.duckdb ../privacy /tmp/ref
bench/tpch_crosscheck.py "psql -h localhost -U postgres" /tmp/ref
```

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

`bench/tpch_timing.py` times the 22 queries as ordinary SQL (one world), through the rewrite
with noise (the default) and without (SF1 with `partsupp` and an index on `lineitem(l_partkey)`
and `lineitem(l_orderkey)`, PostgreSQL 16.15, Apple M1, 4 workers, JIT off, median of 3 runs
after a warm-up; times in ms):

| Query | plain | noised | deterministic | noised / plain |
|---|---:|---:|---:|---:|
| Q1 | 988.5 | 2642.2 | 2626.2 | 2.67 |
| Q2 | 881.1 | 877.0 | 878.6 | 1.00 |
| Q3 | 278.1 | 284.3 | 280.5 | 1.02 |
| Q4 | 138.1 | 142.7 | 150.8 | 1.03 |
| Q5 | 153.0 | 165.3 | 160.7 | 1.08 |
| Q6 | 217.0 | 293.3 | 289.4 | 1.35 |
| Q7 | 243.6 | 243.2 | 245.0 | 1.00 |
| Q8 | 197.2 | 161.8 | 164.3 | 0.82 |
| Q9 | 957.8 | 935.6 | 935.9 | 0.98 |
| Q10 | 216.6 | 218.0 | 217.8 | 1.01 |
| Q11 | 58.1 | 58.5 | 60.4 | 1.01 |
| Q12 | 262.5 | 264.3 | 265.2 | 1.01 |
| Q13 | 297.0 | 297.9 | 297.4 | 1.00 |
| Q14 | 183.2 | 234.6 | 234.3 | 1.28 |
| Q15 | 201.3 | 344.7 | 337.9 | 1.71 |
| Q16 | 105.5 | 107.1 | 106.5 | 1.02 |
| Q17 | 557.1 | 215.8 | 216.0 | 0.39 |
| Q18 | 1478.7 | 1520.6 | 1514.5 | 1.03 |
| Q19 | 19.1 | 19.2 | 19.3 | 1.00 |
| Q20 | 235.2 | 297.4 | 294.7 | 1.26 |
| Q21 | 282.2 | 283.3 | 281.1 | 1.00 |
| Q22 | 74.7 | 116.2 | 117.2 | 1.56 |

The noise costs nothing measurable (noised vs. deterministic). Most queries run at the cost of
the one-world query; the overhead is where the rewrite joins `ORDERS` in to reach the customer
(Q1, Q6, Q14, Q15, Q20 read `LINEITEM` alone) and in Q22, whose mask (with the sub-select) is
evaluated both in the aggregate and in the filter that drops rows in no world. Q2, Q11 and
Q16 are not rewritten. Q8 and Q17 are faster only because PostgreSQL picks other plans for
the rewritten queries.

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

`bench/tpch_rewrite.py` runs TPC-H Q1, Q3, Q5, Q6, Q10, Q12, Q14 and Q17 as ordinary SQL
through the automatic rewrite in counters mode (`CUSTOMER` as the entity, `ORDERS` and `LINEITEM` linked).
Every rewritten query returns exactly the hand-written `as_*` query's result. Q14 and Q17 are
also checked against their definition — the original query evaluated separately on each of
the 64 worlds (for Q17 including the per-world correlated `AVG`): all 64 worlds agree within
float rounding (max relative difference 6.3·10⁻⁶ and 6.6·10⁻⁷). The rewrite adds no runtime
(SF1, 4 workers, median of 3, index on `lineitem(l_partkey)`):

| Query | 1 world (ms) | rewritten, 64 worlds (ms) | hand-written `as_*` (ms) |
|---|---:|---:|---:|
| Q1 | 899.0 | 2383.8 | 2275.0 |
| Q3 | 276.9 | 299.1 | 302.6 |
| Q5 | 291.6 | 285.9 | 287.4 |
| Q6 | 191.0 | 256.3 | 264.6 |
| Q10 | 405.3 | 406.0 | 401.2 |
| Q12 | 276.3 | 275.4 | 277.7 |
| Q14 | 225.0 | 291.7 | 289.8 |
| Q17 | 613.6 | 229.3 | 229.2 |

Q1, Q6 and Q14 read `LINEITEM` without `ORDERS`, so the rewrite joins `ORDERS` in to reach
the customer; the others already contain the joins and cost about the same as the one-world
query. (Rewritten Q17 is faster than the one-world query only because PostgreSQL picks a
different plan once the correlated sub-select moves from `WHERE` into the aggregate.)

## Design

The aggregate kernels are ported from the DuckDB extension, keeping their physical state
layouts:

| Aggregate | State (per group) | DuckDB source |
|---|---|---|
| `as_count` | SWAR: 8 × `uint64` = 64 packed `uint8` counters; cascaded into lazily allocated `uint64[64]` every 255 updates | `as_count.hpp` (`PacCountState`) |
| `as_sum` (integers) | up to 25 lazily allocated levels of 64 × `uint16` counters (4 per `uint64`), staggered by 4 bits; first level stored inline; lossless cascading; separate state for negated negative values | `as_sum.hpp` (`PacSumIntState`, approximate mode) |
| `as_sum` (floating point, `numeric`) | `float[64]`, predicated add (`v × bit`) | `as_sum.hpp` (`PacSumDoubleState`) |
| `as_noised_sum` (`numeric`) | the integer state on value × 10^scale | `as_sum.cpp` (`BindDecimalPacSum`) |
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
  (FK join injection, aggregate transformation, and the categorical rewrite of
  expressions and predicates over worlds — DuckDB's `list_transform` lambdas become
  `asa_lane_*` calls; `src/asa_rewrite.c`). Enforced foreign keys are not required. DuckDB
  passes the hash column through its logical plan; here a subquery passes its mask up as
  an output column, and the top-k pushdown wraps the query in a subquery that is limited
  before the release.
- **Query hash.** DuckDB's bind data carries the query hash to every aggregate; here the
  rewrite embeds it as a constant argument, which the transition functions of the noised
  forms store in their state (it survives partial aggregation and serialization).

Deviations from the DuckDB code:

- A world that only received negative integers is reported, and integer magnitudes
  ≥ 2⁶³ (`INT64_MIN`) are routed by unsigned arithmetic. Both were bugs in the DuckDB code,
  fixed there in [cwida/privacy#29](https://github.com/cwida/privacy/pull/29).
- Counter arrays of `numeric` use the floating-point path, as DuckDB's `as_sum` lists of
  `DECIMAL`; the noised forms use the integer path, as DuckDB's fused `as_noised_sum`.
- A correlated sub-select over no rows returns NULL counters (`asa_counters_*`), as DuckDB's
  decorrelated sub-selects do; `as_sum` itself returns 64 zeros there, as DuckDB's.
- The noise generator is PostgreSQL's (`pg_prng`), not `std::mt19937_64`: noised values are
  distributed as in DuckDB, not identical to them. Every cell of a statement draws from one
  generator (DuckDB reseeds per vector of groups), and without an explicit seed the
  per-statement nonce is random (DuckDB: the query id).
- DP mechanisms and the remaining PAC options are not ported: the `correction` factor,
  utility NULLing (`privacy_min_group_count`), clipping (`as_clip_*`), sample-and-aggregate
  (`as_sample_*`) and *m* ≠ 64.

## License

AGPL-3.0, as the DuckDB extension it is derived from.
