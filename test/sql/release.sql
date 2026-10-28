-- Automatic rewrite with PAC releases (the default): noised aggregates, FROM-clause
-- subqueries, CTEs, HAVING, predicates over worlds, top-k. Without noise every release is
-- deterministic (world 0 of the counters, as DuckDB with pac_mi = 0), so each rewritten
-- query is checked against the hand-written query it should become.
LOAD 'simd_asa';
SET extra_float_digits = 0;
SET max_parallel_workers_per_gather = 0;
SET simd_asa.noise = off;
SET simd_asa.sample_diversity_check = off;

CREATE TABLE p_customer (c_custkey integer, c_nation integer, c_bal numeric(10,2));
CREATE TABLE p_orders (o_orderkey bigint, o_custkey integer, o_total integer);
CREATE TABLE p_lineitem (l_orderkey bigint, l_qty integer, l_price float8);
CREATE TABLE p_nation (n_key integer, n_name text);
INSERT INTO p_customer SELECT c, c % 5, (c % 37) * 2.5 FROM generate_series(1, 300) c;
INSERT INTO p_orders SELECT o, o % 300 + 1, o % 97 FROM generate_series(1, 1500) o;
INSERT INTO p_lineitem SELECT l % 1500 + 1, l % 50 + 1, (l % 13) * 1.5 FROM generate_series(1, 6000) l;
INSERT INTO p_nation SELECT n, 'nation ' || n FROM generate_series(0, 9) n;
SELECT asa_add_entity('p_customer', 'c_custkey');
SELECT asa_add_link('p_orders', 'o_custkey', 'p_customer', 'c_custkey');
SELECT asa_add_link('p_lineitem', 'l_orderkey', 'p_orders', 'o_orderkey');

CREATE FUNCTION pg_temp.same(a regclass, b regclass) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
    r boolean;
BEGIN
    EXECUTE format('SELECT NOT EXISTS (SELECT * FROM %s EXCEPT ALL SELECT * FROM %s) AND '
                   'NOT EXISTS (SELECT * FROM %s EXCEPT ALL SELECT * FROM %s)', a, b, b, a) INTO r;
    RETURN r;
END
$$;

-- the hand-written queries: masks without a per-statement nonce (noise off)
CREATE TEMP VIEW c AS SELECT *, priv_hash(asa_hash(c_custkey)) AS m FROM p_customer;
CREATE TEMP VIEW o AS SELECT *, priv_hash(asa_hash(o_custkey)) AS m FROM p_orders;
CREATE TEMP VIEW l AS SELECT p_lineitem.*, priv_hash(asa_hash(o_custkey)) AS m
FROM p_lineitem JOIN p_orders ON l_orderkey = o_orderkey;

SET simd_asa.rewrite = on;

-- ---------------------------------------------------------------------------
-- Aggregates become noised releases of the same type
-- ---------------------------------------------------------------------------
SELECT c_nation, count(*), sum(c_custkey), sum(c_bal), avg(c_bal), min(c_custkey), max(c_bal)
FROM p_customer GROUP BY 1 \gdesc
EXPLAIN (COSTS OFF, VERBOSE) SELECT sum(o_total) FROM p_orders;
CREATE TEMP TABLE rw AS SELECT c_nation, count(*) AS n, sum(c_custkey) AS s, sum(c_bal) AS b, avg(c_bal) AS a,
                               min(c_custkey) AS mn, max(c_bal) AS mx FROM p_customer GROUP BY 1;
CREATE TEMP TABLE hw AS SELECT c_nation, as_noised_count(m) AS n, as_noised_sum(m, c_custkey) AS s,
                               as_noised_sum(m, c_bal) AS b, as_noised_avg(m, c_bal) AS a,
                               as_noised_min(m, c_custkey) AS mn, as_noised_max(m, c_bal) AS mx FROM c GROUP BY 1;
SELECT pg_temp.same('rw', 'hw') AS releases;
DROP TABLE rw, hw;

-- an arithmetic expression over several aggregates is evaluated per world and released once;
-- other expressions apply to the released values
CREATE TEMP TABLE rw AS SELECT c_nation, 100.0 * sum(c_bal) / count(*) AS share, round(sum(c_bal) / 7, 2) AS r,
                               sum(c_custkey) > 1000 AS big FROM p_customer GROUP BY 1;
CREATE TEMP TABLE hw AS SELECT c_nation,
                               asa_noised(asa_lane_div(asa_lane_mul(100, as_sum(m, c_bal)), as_count(m)),
                                          asa_query_hash())::numeric AS share,
                               round(as_noised_sum(m, c_bal) / 7, 2) AS r,
                               as_noised_sum(m, c_custkey) > 1000 AS big FROM c GROUP BY 1;
SELECT pg_temp.same('rw', 'hw') AS expressions;
DROP TABLE rw, hw;

-- empty input: no world received a row
SELECT sum(c_custkey) AS s, count(*) AS n FROM p_customer WHERE false;

-- ---------------------------------------------------------------------------
-- FROM-clause subqueries and CTEs
-- ---------------------------------------------------------------------------
-- a non-aggregating subquery passes its row mask up (TPC-H Q7, Q8, Q9)
EXPLAIN (COSTS OFF, VERBOSE)
SELECT g, sum(t) FROM (SELECT o_total % 3 AS g, o_total AS t FROM p_orders) d GROUP BY g;
CREATE TEMP TABLE rw AS SELECT g, sum(t) AS s FROM (SELECT o_total % 3 AS g, l_qty AS t FROM p_orders, p_lineitem
                                                    WHERE o_orderkey = l_orderkey) d GROUP BY g;
CREATE TEMP TABLE hw AS SELECT o_total % 3 AS g, as_noised_sum(l.m, l_qty) AS s FROM p_orders, l
WHERE p_orders.o_orderkey = l.l_orderkey GROUP BY 1;
SELECT pg_temp.same('rw', 'hw') AS derived_mask;
DROP TABLE rw, hw;

-- a subquery grouped by the entity key aggregates single entities exactly; its key is the
-- mask of the outer aggregate (TPC-H Q13)
CREATE TEMP TABLE rw AS SELECT n, count(*) AS k FROM (SELECT c_custkey, count(o_orderkey) AS n
    FROM p_customer LEFT JOIN p_orders ON o_custkey = c_custkey AND o_total > 50 GROUP BY c_custkey) t GROUP BY n;
CREATE TEMP TABLE hw AS SELECT n, as_noised_count(priv_hash(asa_hash(c_custkey))) AS k FROM (SELECT c_custkey, count(o_orderkey) AS n
    FROM p_customer LEFT JOIN p_orders ON o_custkey = c_custkey AND o_total > 50 GROUP BY c_custkey) t GROUP BY n;
SELECT pg_temp.same('rw', 'hw') AS per_entity_subquery;
DROP TABLE rw, hw;

-- ... and in an IN sub-select (grouped by a key that determines the entity: TPC-H Q18)
CREATE TEMP TABLE rw AS SELECT o_custkey % 4 AS g, sum(o_total) AS s FROM p_orders
WHERE o_orderkey IN (SELECT l_orderkey FROM p_lineitem GROUP BY l_orderkey HAVING sum(l_qty) > 100) GROUP BY 1;
SET simd_asa.rewrite = off;
CREATE TEMP TABLE hw AS SELECT o_custkey % 4 AS g, as_noised_sum(m, o_total) AS s FROM o
WHERE o_orderkey IN (SELECT l_orderkey FROM p_lineitem GROUP BY l_orderkey HAVING sum(l_qty) > 100) GROUP BY 1;
SET simd_asa.rewrite = on;
SELECT pg_temp.same('rw', 'hw') AS per_entity_in;
DROP TABLE rw, hw;

-- an aggregating CTE is released in place; what reads it is ordinary SQL (TPC-H Q15)
CREATE TEMP TABLE rw AS WITH r AS (SELECT o_total % 7 AS g, sum(o_total) AS s FROM p_orders GROUP BY 1)
SELECT g, s FROM r WHERE s = (SELECT max(s) FROM r);
SET simd_asa.rewrite = off;
CREATE TEMP TABLE hw AS WITH r AS (SELECT o_total % 7 AS g, as_noised_sum(m, o_total) AS s FROM o GROUP BY 1)
SELECT g, s FROM r WHERE s = (SELECT max(s) FROM r);
SET simd_asa.rewrite = on;
SELECT pg_temp.same('rw', 'hw') AS cte_release, (SELECT count(*) FROM rw) AS rows;
DROP TABLE rw, hw;

-- EXISTS / IN without aggregation over dependent rows: ordinary filters (TPC-H Q4, Q21)
EXPLAIN (COSTS OFF)
SELECT count(*) FROM p_orders WHERE EXISTS (SELECT * FROM p_lineitem WHERE l_orderkey = o_orderkey AND l_qty > 40);

-- ---------------------------------------------------------------------------
-- Predicates over worlds
-- ---------------------------------------------------------------------------
-- below an aggregate: AND-ed into the mask, released through counters (TPC-H Q17)
CREATE TEMP TABLE rw AS SELECT sum(l_price) / 7.0 AS v FROM p_lineitem l1
WHERE l_qty < (SELECT avg(l_qty) FROM p_lineitem l2 WHERE l2.l_orderkey % 10 = l1.l_orderkey % 10);
CREATE TEMP TABLE hw AS SELECT asa_noised(asa_lane_div(as_sum(l1.m & asa_lane_lt(l1.l_qty::float8,
    (SELECT as_avg(l2.m, l2.l_qty) FROM l l2 WHERE l2.l_orderkey % 10 = l1.l_orderkey % 10)), l1.l_price), 7.0::float8),
    asa_query_hash())::numeric AS v FROM l l1;
SELECT pg_temp.same('rw', 'hw') AS predicate_mask;
DROP TABLE rw, hw;

-- in a subquery below a grouped aggregate: rows in no world are dropped (TPC-H Q22)
EXPLAIN (COSTS OFF, VERBOSE)
SELECT g, count(*) FROM (SELECT c_nation AS g FROM p_customer WHERE c_bal > (SELECT avg(c_bal) FROM p_customer)) t
GROUP BY g;
CREATE TEMP TABLE rw AS SELECT g, count(*) AS n FROM (SELECT c_nation AS g FROM p_customer
    WHERE c_bal > (SELECT avg(c_bal) FROM p_customer)) t GROUP BY g;
CREATE TEMP TABLE hw AS SELECT g, asa_noised(as_count(mm), asa_query_hash())::bigint AS n FROM
    (SELECT c_nation AS g, m & asa_lane_gt(c_bal::float8, (SELECT as_avg(m, c_bal) FROM c)) AS mm FROM c) t
WHERE mm <> 0 GROUP BY g;
SELECT pg_temp.same('rw', 'hw') AS predicate_in_subquery;
DROP TABLE rw, hw;

-- rows that are not aggregated pass by majority over the worlds (TPC-H Q20)
CREATE TEMP TABLE rw AS SELECT n_key FROM p_nation WHERE n_key * 30 < (SELECT sum(c_nation) FROM p_customer WHERE c_custkey <= 100);
CREATE TEMP TABLE hw AS SELECT n_key FROM p_nation
WHERE priv_filter(asa_lane_lt((n_key * 30)::float8, (SELECT as_sum(m, c_nation) FROM c WHERE c_custkey <= 100)));
SELECT pg_temp.same('rw', 'hw') AS predicate_filter, (SELECT count(*) FROM rw) AS passing;
-- a correlated sub-select over no rows: the predicate is NULL (as DuckDB's decorrelated join)
SELECT count(*) AS passing FROM p_nation
WHERE n_key < (SELECT sum(c_nation) FROM p_customer WHERE c_nation = n_key + 100);
DROP TABLE rw, hw;

-- HAVING over aggregates decides per group
CREATE TEMP TABLE rw AS SELECT c_nation, count(*) AS n FROM p_customer GROUP BY 1 HAVING sum(c_custkey) > 9500;
CREATE TEMP TABLE hw AS SELECT c_nation, as_noised_count(m) AS n FROM c GROUP BY 1
HAVING priv_filter(asa_lane_gt(as_sum(m, c_custkey), 9500::float8));
SELECT pg_temp.same('rw', 'hw') AS having_filter, (SELECT count(*) FROM rw) AS groups;
DROP TABLE rw, hw;

-- ---------------------------------------------------------------------------
-- Top-k: rank by the mean of the worlds, release only the selected groups
-- ---------------------------------------------------------------------------
EXPLAIN (COSTS OFF, VERBOSE)
SELECT c_nation, sum(c_custkey) AS s FROM p_customer GROUP BY 1 ORDER BY s DESC LIMIT 2;
SELECT c_nation, sum(c_custkey) AS s FROM p_customer GROUP BY 1 ORDER BY s DESC LIMIT 2;
SET simd_asa.rewrite = off;
SELECT c_nation, asa_noised(s, asa_query_hash())::bigint AS s FROM
    (SELECT c_nation, as_sum(m, c_custkey) AS s FROM c GROUP BY 1 ORDER BY priv_mean(as_sum(m, c_custkey)) DESC LIMIT 2) t
ORDER BY priv_mean(s) DESC;
SET simd_asa.rewrite = on;
SET simd_asa.pushdown_topk = off;
SELECT c_nation, sum(c_custkey) AS s FROM p_customer GROUP BY 1 ORDER BY s DESC LIMIT 2;
RESET simd_asa.pushdown_topk;

-- ---------------------------------------------------------------------------
-- Statements: query hash, parallel query, prepared statements
-- ---------------------------------------------------------------------------
-- the query hash survives partial aggregation in parallel workers
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
CREATE TEMP TABLE par AS SELECT l_qty % 3 AS g, sum(l_qty) AS s, count(*) AS n FROM p_lineitem GROUP BY 1;
SET max_parallel_workers_per_gather = 0;
CREATE TEMP TABLE ser AS SELECT l_qty % 3 AS g, sum(l_qty) AS s, count(*) AS n FROM p_lineitem GROUP BY 1;
SELECT pg_temp.same('par', 'ser') AS parallel_equals_serial;
DROP TABLE par, ser;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

-- with noise every statement draws fresh worlds; re-executing a prepared statement releases
-- the same values (its generators and p-tracking restart)
SET simd_asa.noise = on;
SET simd_asa.seed = 11;
PREPARE q AS SELECT c_nation, sum(c_custkey) AS s FROM p_customer GROUP BY 1;
CREATE TEMP TABLE e1 AS EXECUTE q;
CREATE TEMP TABLE e2 AS EXECUTE q;
SELECT pg_temp.same('e1', 'e2') AS prepared_reproducible;
CREATE TEMP TABLE e3 AS SELECT c_nation, sum(c_custkey) AS s FROM p_customer GROUP BY 1;
CREATE TEMP TABLE e4 AS SELECT c_nation, sum(c_custkey) AS s FROM p_customer GROUP BY 1;
SELECT pg_temp.same('e3', 'e4') AS statements_same;
SELECT count(*) AS plausible FROM e3 JOIN (SELECT c_nation, sum(c_custkey) AS t FROM p_customer GROUP BY 1) x
    USING (c_nation) WHERE abs(e3.s - x.t) < 0.5 * x.t;
DEALLOCATE q;
DROP TABLE e1, e2, e3, e4;
RESET simd_asa.seed;
SET simd_asa.noise = off;

-- counters mode keeps the per-world arrays and rejects what needs a release
SET simd_asa.counters = on;
SELECT sum(o_total) FROM p_orders \gdesc
SELECT c_nation FROM p_customer GROUP BY 1 HAVING sum(c_custkey) > 10;
SELECT n_key FROM p_nation WHERE n_key < (SELECT avg(c_nation) FROM p_customer);
WITH r AS (SELECT o_total % 7 AS g, sum(o_total) AS s FROM p_orders GROUP BY 1) SELECT * FROM r;
RESET simd_asa.counters;

RESET simd_asa.rewrite;
DROP VIEW c, o, l;
SELECT asa_drop_link('p_lineitem', 'p_orders');
SELECT asa_drop_link('p_orders', 'p_customer');
SELECT asa_drop_entity('p_customer');
DROP TABLE p_customer, p_orders, p_lineitem, p_nation;
