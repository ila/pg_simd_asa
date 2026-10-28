SET extra_float_digits = 0;
CREATE TABLE pt AS
SELECT k % 50 AS g, priv_hash(asa_hash(k::bigint)) AS m, (k % 100000 - 30000)::integer AS vi,
       (k::bigint * 2654435761 % 4000000000000 - 1000000000000) AS vb, ((k % 997) / 3.0 - 100)::float8 AS vd
FROM generate_series(1, 300000) k;
ANALYZE pt;

CREATE TABLE serial_res AS
SELECT g, as_count(m) c, as_sum(m, vi) si, as_sum(m, vb) sb, as_avg(m, vi) ai, as_min(m, vi) mn, as_max(m, vd) mx,
       as_sum(m, vd) sd
FROM pt GROUP BY g;

-- force a parallel plan: partial aggregation in workers, serialize -> deserialize -> combine
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 4;
SET parallel_leader_participation = off;
EXPLAIN (COSTS OFF)
SELECT g, as_count(m), as_sum(m, vi), as_sum(m, vb), as_avg(m, vi), as_min(m, vi), as_max(m, vd), as_sum(m, vd)
FROM pt GROUP BY g;

CREATE TABLE parallel_res AS
SELECT g, as_count(m) c, as_sum(m, vi) si, as_sum(m, vb) sb, as_avg(m, vi) ai, as_min(m, vi) mn, as_max(m, vd) mx,
       as_sum(m, vd) sd
FROM pt GROUP BY g;

-- integer states merge losslessly across workers (bigint: same per-value truncation);
-- floating-point sums only differ by float32 accumulation order
SELECT count(*) AS groups,
       bool_and(s.c = p.c AND s.si = p.si AND s.sb = p.sb AND s.mn = p.mn AND s.mx = p.mx) AS exact_ok,
       bool_and(s.ai = p.ai) AS avg_ok,
       max((SELECT max(abs(x - y) / greatest(abs(y), 1)) FROM unnest(p.sd, s.sd) u(x, y))) < 1e-4 AS float_ok
FROM serial_res s JOIN parallel_res p USING (g);

-- ungrouped aggregate
EXPLAIN (COSTS OFF) SELECT as_sum(m, vb), as_count(m) FROM pt;
CREATE TABLE parallel_total AS SELECT as_sum(m, vb) s, as_count(m) c FROM pt;
RESET max_parallel_workers_per_gather;
SET max_parallel_workers_per_gather = 0;
SELECT p.s = t.s AND p.c = t.c AS ungrouped_ok FROM parallel_total p, (SELECT as_sum(m, vb) s, as_count(m) c FROM pt) t;
