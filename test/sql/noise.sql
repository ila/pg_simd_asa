-- PAC noise release: as_noised_* aggregates, as_noised, priv_filter, priv_mean.
SET extra_float_digits = 0;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE nt (k integer, g integer, vi integer, vb bigint, vf float8, vn numeric(10,2));
INSERT INTO nt SELECT x, x % 3, x % 17 - 5, x * 1000, (x % 11) * 0.25, (x % 23) * 1.25
FROM generate_series(1, 3000) x;

-- ---------------------------------------------------------------------------
-- Without noise a release is world 0 of the counters (DuckDB, pac_mi = 0)
-- ---------------------------------------------------------------------------
SET simd_asa.noise = off;
SELECT g,
       as_noised_count(m) = (as_count(m))[1] AS count_w0,
       as_noised_count(m, vi) = (as_count(m, vi))[1] AS count_value_w0,
       as_noised_sum(m, vi) = (as_sum(m, vi))[1] AS sum_int_w0,
       as_noised_sum(m, vb) = (as_sum(m, vb))[1] AS sum_bigint_w0,
       as_noised_sum(m, vf) = (as_sum(m, vf))[1]::float8 AS sum_float_w0,
       as_noised_min(m, vi) = (as_min(m, vi))[1] AS min_w0,
       as_noised_max(m, vf) = (as_max(m, vf))[1] AS max_w0,
       as_noised_avg(m, vf) = (as_sum(m, vf))[1]::float8 / (as_count(m, vf))[1]::float8 AS avg_w0
FROM (SELECT *, priv_hash(asa_hash(k)) AS m FROM nt) t GROUP BY g ORDER BY g;

-- the types of the corresponding PostgreSQL aggregates
SELECT pg_typeof(as_noised_count(m)) AS count, pg_typeof(as_noised_sum(m, vi)) AS sum_int,
       pg_typeof(as_noised_sum(m, vb)) AS sum_bigint, pg_typeof(as_noised_sum(m, vn)) AS sum_numeric,
       pg_typeof(as_noised_avg(m, vi)) AS avg_int, pg_typeof(as_noised_avg(m, vf)) AS avg_float,
       pg_typeof(as_noised_max(m, vn)) AS max_numeric
FROM (SELECT *, priv_hash(asa_hash(k)) AS m FROM nt) t;

-- numeric is summed as integers times 10^scale (DuckDB's DECIMAL): exact here, unlike the
-- float counters of as_sum; avg divides the noised sum by the noised count
SELECT as_noised_sum(m, vn) AS sum_numeric, 2 * sum(vn) FILTER (WHERE m & 1 = 1) AS world0_exact,
       abs(as_noised_avg(m, vn) - sum(vn) FILTER (WHERE m & 1 = 1) / count(vn) FILTER (WHERE m & 1 = 1)) < 1e-5
           AS avg_numeric
FROM (SELECT *, priv_hash(asa_hash(k)) AS m FROM nt WHERE k <= 40) t;

-- NULL for cells in no world: no rows, or only rows of mask 0
SELECT as_noised_sum(priv_hash(asa_hash(k)), vi) IS NULL AS no_rows,
       as_noised_count(priv_hash(asa_hash(k))) IS NULL AS no_rows_count FROM nt WHERE false;
SELECT as_noised_sum(0::bigint, vi) IS NULL AS mask_zero FROM nt;

-- noise off and mi = 0 are the same mode
SET simd_asa.noise = on;
SET simd_asa.mi = 0;
SELECT as_noised_sum(m, vi) = (as_sum(m, vi))[1] AS mi_zero_is_world0
FROM (SELECT *, priv_hash(asa_hash(k)) AS m FROM nt) t;
RESET simd_asa.mi;

-- ---------------------------------------------------------------------------
-- With noise
-- ---------------------------------------------------------------------------
-- the seed's query hash: a statement's releases are reproducible ...
CREATE TEMP TABLE n1 AS SELECT g, as_noised_sum(priv_hash(asa_hash(k)), vi) AS s FROM nt GROUP BY g;
CREATE TEMP TABLE n2 AS SELECT g, as_noised_sum(priv_hash(asa_hash(k)), vi) AS s FROM nt GROUP BY g;
SELECT count(*) AS same_releases FROM n1 JOIN n2 USING (g) WHERE n1.s = n2.s;
-- ... noised: not the world-0 value, within a few noise deviations of the world mean (the
-- noise variance is the worlds' variance / (2 mi): its deviation 8x the worlds' at mi = 1/128)
SELECT g, n1.s <> w.w0 AS noised, abs(n1.s - w.mean) < 5 * 8 * w.sd AS plausible
FROM n1 JOIN (SELECT g, (s)[1] AS w0, priv_mean(s) AS mean, (SELECT stddev(x) FROM unnest(s) x) AS sd
              FROM (SELECT g, as_sum(priv_hash(asa_hash(k)), vi) AS s FROM nt GROUP BY g) a) w USING (g) ORDER BY g;
-- a different query hash draws different worlds and noise
SELECT asa_noised_sum(priv_hash(asa_hash(k), 12345), vi, 12345) <> as_noised_sum(priv_hash(asa_hash(k)), vi)
    AS other_query_hash FROM nt;
-- cells of single entities are suppressed with probability popcount(~mask) / 64
SELECT count(*) FILTER (WHERE s IS NULL) > 0 AS some_suppressed,
       count(*) FILTER (WHERE s IS NOT NULL) > 0 AS some_released
FROM (SELECT k, as_noised_sum(priv_hash(asa_hash(k)), vi) AS s FROM nt WHERE k <= 200 GROUP BY k) t;
SET simd_asa.ptracking = off;
SELECT as_noised_sum(priv_hash(asa_hash(k)), vi) IS NOT NULL AS released_without_ptracking FROM nt;
RESET simd_asa.ptracking;

-- groups of single entities fail the sample-diversity check (with repaired masks every
-- entity is in exactly 32 worlds, the pattern the check looks for)
SET simd_asa.hash_repair = on;
SELECT count(s) >= 0 FROM (SELECT k, as_noised_sum(priv_hash(asa_hash(k)), vi) AS s FROM nt GROUP BY k) t;
SET simd_asa.sample_diversity_check = off;
SELECT count(s) >= 0 AS released FROM (SELECT k, as_noised_sum(priv_hash(asa_hash(k)), vi) AS s FROM nt GROUP BY k) t;
RESET simd_asa.sample_diversity_check;
RESET simd_asa.hash_repair;

-- ---------------------------------------------------------------------------
-- Scalars
-- ---------------------------------------------------------------------------
SET simd_asa.noise = off;
SELECT as_noised(array_fill(3::real, ARRAY[64])) AS noised_world0,
       priv_mean(ARRAY[1, 2, NULL, 6]::real[]) AS mean_non_null,
       priv_filter(-1) AS all_worlds, priv_filter(0) AS no_world,
       priv_filter(x'00000000FFFFFFFF'::bigint) AS half, priv_filter(x'000000007FFFFFFF'::bigint) AS below_half;
SET simd_asa.noise = on;
SELECT count(*) FILTER (WHERE priv_filter(x'000000000000FFFF'::bigint)) BETWEEN 100 AND 400 AS quarter_pass
FROM generate_series(1, 1000);

RESET simd_asa.noise;
DROP TABLE nt;
