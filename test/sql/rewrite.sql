-- Automatic SIMD-ASA rewrite: metadata, mask derivation, aggregate replacement, rejections.
LOAD 'simd_asa';
-- These tests check the per-world counters (as before 0.2's noised releases); release.sql
-- tests the default noised releases.
SET simd_asa.counters = on;
SET extra_float_digits = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE r_customer (c_custkey integer, c_nation integer);
CREATE TABLE r_orders (o_orderkey bigint, o_custkey integer, o_total integer);
CREATE TABLE r_lineitem (l_orderkey bigint, l_qty integer, l_price float8, l_ship date);
CREATE TABLE r_nation (n_key integer, n_name text);
INSERT INTO r_customer SELECT c, c % 5 FROM generate_series(1, 200) c;
INSERT INTO r_orders SELECT o, o % 200 + 1, o % 97 FROM generate_series(1, 1000) o;
INSERT INTO r_lineitem SELECT l % 1000 + 1, l % 50 + 1, (l % 13) * 1.5, date '2026-01-01' + l % 300
FROM generate_series(1, 4000) l;
INSERT INTO r_nation SELECT n, 'nation ' || n FROM generate_series(0, 4) n;

-- result equality of two tables (as multisets)
CREATE FUNCTION pg_temp.same(a regclass, b regclass) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
    r boolean;
BEGIN
    EXECUTE format('SELECT NOT EXISTS (SELECT * FROM %s EXCEPT ALL SELECT * FROM %s) AND '
                   'NOT EXISTS (SELECT * FROM %s EXCEPT ALL SELECT * FROM %s)', a, b, b, a) INTO r;
    RETURN r;
END
$$;

-- ---------------------------------------------------------------------------
-- Metadata
-- ---------------------------------------------------------------------------
SELECT asa_add_entity('r_customer', 'no_such_column');
SELECT asa_add_entity('r_nation', 'n_name');
SELECT asa_add_entity('r_customer', 'c_custkey');
SELECT asa_add_link('r_orders', 'o_custkey', 'r_customer', 'c_custkey');
SELECT asa_add_link('r_lineitem', 'l_orderkey', 'r_orders', 'o_orderkey');
SELECT relid, key_column FROM asa_entity;
SELECT relid, link_column, ref_relid, ref_column FROM asa_link ORDER BY 1;

-- rewrite is opt-in
SELECT pg_typeof(sum(o_total)) FROM r_orders;
SET simd_asa.rewrite = on;
SELECT c_nation, count(*), sum(c_custkey) FROM r_customer GROUP BY 1
\gdesc

-- ---------------------------------------------------------------------------
-- Entity key derivation (each rewritten query equals the hand-written as_* query)
-- ---------------------------------------------------------------------------

-- the entity itself
CREATE TEMP TABLE rw AS SELECT c_nation, count(*) AS n, sum(c_custkey) AS s FROM r_customer GROUP BY 1;
CREATE TEMP TABLE hw AS SELECT c_nation, as_count(priv_hash(asa_hash(c_custkey))) AS n,
                               as_sum(priv_hash(asa_hash(c_custkey)), c_custkey) AS s FROM r_customer GROUP BY 1;
SELECT pg_temp.same('rw', 'hw') AS entity_table;
DROP TABLE rw, hw;

-- a link to the entity: the link column is the entity key, no join
EXPLAIN (COSTS OFF, VERBOSE) SELECT sum(o_total) FROM r_orders;
CREATE TEMP TABLE rw AS SELECT o_total % 3 AS g, sum(o_total) AS s, avg(o_total) AS a, min(o_total) AS mn,
                               max(o_total) AS mx, count(o_total) AS n FROM r_orders GROUP BY 1;
CREATE TEMP TABLE hw AS SELECT o_total % 3 AS g, as_sum(m, o_total) AS s, as_avg(m, o_total) AS a,
                               as_min(m, o_total) AS mn, as_max(m, o_total) AS mx, as_count(m, o_total) AS n
FROM (SELECT *, priv_hash(asa_hash(o_custkey)) AS m FROM r_orders) t GROUP BY 1;
SELECT pg_temp.same('rw', 'hw') AS direct_link;
DROP TABLE rw, hw;

-- two links away: the linked relation is joined in
EXPLAIN (COSTS OFF) SELECT sum(l_qty) FROM r_lineitem;
CREATE TEMP TABLE rw AS SELECT l_qty % 4 AS g, sum(l_qty) AS s, sum(l_price) AS p, count(*) AS n
FROM r_lineitem WHERE l_ship < date '2026-08-01' GROUP BY 1;
CREATE TEMP TABLE hw AS SELECT l_qty % 4 AS g, as_sum(m, l_qty) AS s, as_sum(m, l_price) AS p, as_count(m) AS n
FROM (SELECT l.*, priv_hash(asa_hash(o.o_custkey)) AS m FROM r_lineitem l JOIN r_orders o ON l.l_orderkey = o.o_orderkey
      WHERE l_ship < date '2026-08-01') t GROUP BY 1;
SELECT pg_temp.same('rw', 'hw') AS injected_join;
DROP TABLE rw, hw;

-- an existing join along the link is reused (one join, one mask)
EXPLAIN (COSTS OFF) SELECT sum(l_qty) FROM r_lineitem JOIN r_orders ON l_orderkey = o_orderkey;
EXPLAIN (COSTS OFF, VERBOSE)
SELECT c_nation, sum(l_qty) FROM r_customer, r_orders, r_lineitem
WHERE c_custkey = o_custkey AND o_orderkey = l_orderkey GROUP BY 1;
CREATE TEMP TABLE rw AS SELECT n_name, sum(l_qty) AS s, count(*) AS n
FROM r_customer JOIN r_orders ON c_custkey = o_custkey JOIN r_lineitem ON o_orderkey = l_orderkey
     JOIN r_nation ON n_key = c_nation GROUP BY 1;
CREATE TEMP TABLE hw AS SELECT n_name, as_sum(priv_hash(asa_hash(c_custkey)), l_qty) AS s,
                               as_count(priv_hash(asa_hash(c_custkey))) AS n
FROM r_customer JOIN r_orders ON c_custkey = o_custkey JOIN r_lineitem ON o_orderkey = l_orderkey
     JOIN r_nation ON n_key = c_nation GROUP BY 1;
SELECT pg_temp.same('rw', 'hw') AS reused_joins;
DROP TABLE rw, hw;

-- tuples derived from two different entities belong to the worlds containing both
EXPLAIN (COSTS OFF, VERBOSE)
SELECT count(*) FROM r_orders a JOIN r_orders b ON a.o_total = b.o_total AND a.o_orderkey < b.o_orderkey;
CREATE TEMP TABLE rw AS SELECT count(*) AS n
FROM r_orders a JOIN r_orders b ON a.o_total = b.o_total AND a.o_orderkey < b.o_orderkey;
CREATE TEMP TABLE hw AS SELECT as_count(priv_hash(asa_hash(a.o_custkey)) & priv_hash(asa_hash(b.o_custkey))) AS n
FROM r_orders a JOIN r_orders b ON a.o_total = b.o_total AND a.o_orderkey < b.o_orderkey;
SELECT pg_temp.same('rw', 'hw') AS two_entities;
DROP TABLE rw, hw;

-- FILTER is kept; ORDER BY / DISTINCT on a rewritten column sort the arrays
CREATE TEMP TABLE rw AS SELECT c_nation, count(*) FILTER (WHERE c_custkey > 100) AS n FROM r_customer GROUP BY 1;
CREATE TEMP TABLE hw AS SELECT c_nation, as_count(priv_hash(asa_hash(c_custkey))) FILTER (WHERE c_custkey > 100) AS n
FROM r_customer GROUP BY 1;
SELECT pg_temp.same('rw', 'hw') AS filter_kept;
DROP TABLE rw, hw;
CREATE TEMP TABLE rw AS SELECT c_nation, sum(c_custkey) AS s FROM r_customer GROUP BY 1 ORDER BY 2 DESC;
SELECT c_nation FROM rw;
SELECT c_nation FROM rw ORDER BY s DESC;
DROP TABLE rw;
CREATE TEMP TABLE rw AS SELECT DISTINCT count(*) AS n FROM r_customer GROUP BY c_nation;
SELECT count(*) AS distinct_worlds FROM rw;
DROP TABLE rw;

-- ---------------------------------------------------------------------------
-- Expressions over aggregates are evaluated per world (TPC-H Q14 shape)
-- ---------------------------------------------------------------------------
SELECT asa_lane_add(ARRAY[1, 2, NULL]::real[], ARRAY[10, 20, 30]::real[]) AS add,
       asa_lane_sub(ARRAY[5, 5, 5]::real[], 1) AS sub, asa_lane_mul(2, ARRAY[1, 2, 3]::real[]) AS mul,
       asa_lane_div(ARRAY[1, 2, 3]::real[], ARRAY[2, 0, 1]::real[]) AS div, asa_lane_neg(ARRAY[1, -2]::real[]) AS neg;
SELECT asa_lane_lt(ARRAY[1, 5, 3, NULL]::real[], 4) AS lt, asa_lane_ge(4, ARRAY[1, 5, 3]::real[]) AS ge,
       asa_lane_eq(ARRAY[1, 2, 3]::real[], ARRAY[1, 0, 3]::real[]) AS eq;
EXPLAIN (COSTS OFF, VERBOSE) SELECT 100.0 * sum(c_custkey) / count(*) FROM r_customer;
CREATE TEMP TABLE rw AS SELECT c_nation, 100.0 * sum(c_custkey) / count(*) AS r, -sum(c_custkey) + c_nation AS n,
                               sum(c_custkey)::numeric(12, 2) AS c FROM r_customer GROUP BY 1;
CREATE TEMP TABLE hw AS SELECT c_nation, asa_lane_div(asa_lane_mul(100.0, as_sum(m, c_custkey)), as_count(m)) AS r,
                               asa_lane_add(asa_lane_neg(as_sum(m, c_custkey)), c_nation) AS n, as_sum(m, c_custkey) AS c
FROM (SELECT *, priv_hash(asa_hash(c_custkey)) AS m FROM r_customer) t GROUP BY 1;
SELECT pg_temp.same('rw', 'hw') AS lane_expressions;
DROP TABLE rw, hw;

-- ---------------------------------------------------------------------------
-- A comparison with an aggregating sub-select is a predicate over worlds (TPC-H Q17 shape):
-- the sub-select yields one value per world, and the comparison's per-world outcome is
-- folded into the mask of the outer aggregates.
-- ---------------------------------------------------------------------------
EXPLAIN (COSTS OFF, VERBOSE)
SELECT sum(l_price) FROM r_lineitem l1
WHERE l_qty < (SELECT avg(l_qty) FROM r_lineitem l2 WHERE l2.l_orderkey % 10 = l1.l_orderkey % 10);
CREATE TEMP TABLE rw AS SELECT sum(l_price) AS s FROM r_lineitem l1
WHERE l_qty < (SELECT avg(l_qty) FROM r_lineitem l2 WHERE l2.l_orderkey % 10 = l1.l_orderkey % 10);
-- the definition: the query evaluated on each world alone (as_sum reports 2 x sum)
SET simd_asa.rewrite = off;
CREATE TEMP TABLE def AS
WITH li AS (SELECT l.*, priv_hash(asa_hash(o_custkey)) AS m FROM r_lineitem l JOIN r_orders ON l_orderkey = o_orderkey),
     w AS (SELECT j, li.* FROM li, generate_series(0, 63) j WHERE (m >> j) & 1 = 1),
     a AS (SELECT j, l_orderkey % 10 AS g, avg(l_qty) AS avg_qty FROM w GROUP BY 1, 2)
SELECT w.j, 2 * sum(l_price) AS s FROM w JOIN a ON a.j = w.j AND a.g = w.l_orderkey % 10
WHERE l_qty < avg_qty GROUP BY w.j;
SELECT count(*) AS worlds, count(*) FILTER (WHERE abs(r.v - d.s) > 1e-4 * abs(d.s)) AS worlds_differing
FROM (SELECT ordinality - 1 AS j, v FROM rw, unnest(rw.s) WITH ORDINALITY u(v, ordinality)) r JOIN def d USING (j);
DROP TABLE rw, def;
SET simd_asa.rewrite = on;
-- outer rows that are shared by all worlds: the predicate alone decides the worlds
EXPLAIN (COSTS OFF, VERBOSE) SELECT count(*) FROM r_nation WHERE n_key < (SELECT avg(c_nation) FROM r_customer);
-- a sub-select over sample-independent relations stays an ordinary predicate
EXPLAIN (COSTS OFF) SELECT count(*) FROM r_customer WHERE c_nation < (SELECT max(n_key) FROM r_nation);

-- prepared statements see the rewritten result type
PREPARE p AS SELECT c_nation, count(*) FROM r_customer WHERE c_custkey > $1 GROUP BY 1;
CREATE TEMP TABLE rw AS EXECUTE p(150);
SELECT c_nation, pg_typeof(count), array_length(count, 1) FROM rw ORDER BY 1;
DEALLOCATE p;
DROP TABLE rw;

-- not rewritten: sample-independent relations, non-aggregate queries, explicit as_* queries
SELECT n_name, count(*) FROM r_nation GROUP BY 1 ORDER BY 1 LIMIT 2;
SELECT o_orderkey, o_total FROM r_orders ORDER BY 1 LIMIT 2;
SELECT array_length(as_count(priv_hash(asa_hash(c_custkey))), 1) FROM r_customer;
SELECT o_custkey FROM r_orders WHERE o_orderkey IN (SELECT l_orderkey FROM r_lineitem WHERE l_qty = 1) ORDER BY 1 LIMIT 2;

-- ---------------------------------------------------------------------------
-- Rejected query shapes
-- ---------------------------------------------------------------------------
SELECT c_nation FROM r_customer GROUP BY 1 HAVING sum(c_custkey) > 10;
SELECT sum(o_total) > 10 FROM r_orders;
SELECT round(sum(o_total)) FROM r_orders;
SELECT count(DISTINCT o_custkey) FROM r_orders;
SELECT min(l_ship) FROM r_lineitem;
SELECT string_agg(n_name, ',') FROM r_customer JOIN r_nation ON n_key = c_nation;
SELECT max(s) FROM (SELECT o_custkey, sum(o_total) AS s FROM r_orders GROUP BY 1) t;
WITH t AS (SELECT * FROM r_orders) SELECT count(*) FROM t;
SELECT count(*) FROM r_nation WHERE n_key IN (SELECT c_nation FROM r_customer);
SELECT count(*) FROM r_nation LEFT JOIN r_customer ON n_key = c_nation;
SELECT c_nation, count(*), rank() OVER (ORDER BY c_nation) FROM r_customer GROUP BY 1;
SELECT c_nation, count(*) FROM r_customer GROUP BY ROLLUP (c_nation);
CREATE VIEW r_orders_v AS SELECT * FROM r_orders;
SELECT count(*) FROM r_orders_v;

-- Public UDFs must not masquerade as extension functions and bypass sampling.
CREATE FUNCTION public.r_identity(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1';
SELECT sum(public.r_identity(o_total)) FROM r_orders;
-- The restriction applies only to queries over entity-dependent data.
SELECT sum(public.r_identity(n_key)) AS independent_udf FROM r_nation;
DROP FUNCTION public.r_identity(integer);
-- Actual extension functions remain valid with automatic rewriting enabled.
SELECT array_length(as_count(priv_hash(asa_hash(c_custkey))), 1) AS explicit_extension FROM r_customer;

-- Nested names resolve by lexical declaration, not by the last CTE visited.
WITH x AS (SELECT o_total FROM r_orders)
SELECT sum(x.o_total) FROM (WITH x AS (SELECT 1 AS v) SELECT v FROM x) d CROSS JOIN x;
WITH x AS (SELECT o_total FROM r_orders)
SELECT sum(x.o_total) FROM (WITH y AS (SELECT 1 AS v) SELECT v FROM y) d CROSS JOIN x;
-- An ancestor CTE referenced two levels down is still entity-dependent.
WITH x AS (SELECT o_total FROM r_orders)
SELECT (SELECT sum(o_total) FROM (SELECT o_total FROM x) t);
-- Aggregated CTE releases remain supported even when a nested CTE shadows their name.
SET simd_asa.counters = off;
SET simd_asa.noise = off;
WITH x AS (SELECT sum(o_total) AS s FROM r_orders)
SELECT pg_typeof(x.s) AS released_cte
FROM (WITH x AS (SELECT 1 AS v) SELECT v FROM x) d CROSS JOIN x;
SET simd_asa.counters = on;
RESET simd_asa.noise;
-- A local CTE shadows an outer dependent CTE, so its result stays scalar.
WITH x AS (SELECT o_total FROM r_orders)
SELECT (WITH x AS (SELECT 7 AS v) SELECT sum(v) FROM x) AS independent_cte;

-- ---------------------------------------------------------------------------
-- Several candidate sampling entities: each query selects one
-- ---------------------------------------------------------------------------
SELECT asa_add_entity('r_orders', 'o_orderkey');
SELECT count(*) FROM r_lineitem;
SET simd_asa.entity = 'r_nation';
SELECT count(*) FROM r_lineitem;
SET simd_asa.entity = 'r_orders';
EXPLAIN (COSTS OFF, VERBOSE) SELECT count(*) FROM r_lineitem;
SELECT pg_typeof(count(*)) FROM r_customer;
SET simd_asa.entity = 'r_customer';
EXPLAIN (COSTS OFF, VERBOSE) SELECT count(*) FROM r_lineitem;
RESET simd_asa.entity;
SELECT asa_drop_entity('r_orders');
SELECT asa_drop_link('r_lineitem', 'r_orders');
SELECT pg_typeof(count(*)) FROM r_lineitem;

RESET simd_asa.rewrite;
DROP VIEW r_orders_v;
SELECT asa_drop_link('r_orders', 'r_customer');
SELECT asa_drop_entity('r_customer');
DROP TABLE r_customer, r_orders, r_lineitem, r_nation;
