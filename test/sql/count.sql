SET extra_float_digits = 0;
CREATE TABLE ct AS
SELECT k % 10 AS g, priv_hash(asa_hash(k::bigint)) AS m, CASE WHEN k % 7 = 0 THEN NULL ELSE k END AS v
FROM generate_series(1, 50000) k;

-- small hand-checked example: world j counts rows with bit j set, scaled by 2
SELECT (as_count(m))[1:4] FROM (VALUES (1::bigint), (2), (3), (NULL)) t(m);
SELECT (as_count(m, v))[1:4] FROM (VALUES (1::bigint, 1), (2, NULL), (3, 3)) t(m, v);
-- empty input and mask 0: all worlds 0
SELECT as_count(m) = array_fill(0::real, ARRAY[64]) FROM ct WHERE false;
SELECT as_count(0::bigint) = array_fill(0::real, ARRAY[64]) FROM ct;
SELECT array_length(as_count(m), 1) FROM ct;

-- every world equals the naive conditional COUNT (more than 255 rows per group: SWAR cascades)
WITH a AS (SELECT g, as_count(m) AS c, as_count(m, v) AS cv FROM ct GROUP BY g),
     n AS (SELECT g, j, 2 * count(*) FILTER (WHERE (m >> j) & 1 = 1) AS e,
                  2 * count(v) FILTER (WHERE (m >> j) & 1 = 1) AS ev
           FROM ct, generate_series(0, 63) j GROUP BY g, j)
SELECT count(*) AS cells,
       count(*) FILTER (WHERE c[j + 1] <> e) AS count_mismatch,
       count(*) FILTER (WHERE cv[j + 1] <> ev) AS count_value_mismatch
FROM a JOIN n USING (g);
