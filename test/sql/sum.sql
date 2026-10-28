SET extra_float_digits = 0;
CREATE TABLE st AS
SELECT k % 10 AS g, priv_hash(asa_hash(k::bigint)) AS m,
       (k % 1000 - 300)::smallint AS vs, (k % 100000 - 30000)::integer AS vi,
       ((k::bigint * 2654435761) % 4000000000000 - 1000000000000)::bigint AS vb,
       ((k % 997) / 3.0 - 100)::float8 AS vd, ((k % 997) / 3.0 - 100)::real AS vr, ((k % 997) / 3.0 - 100)::numeric AS vn
FROM generate_series(1, 50000) k;

-- hand-checked: lane j = 2 * sum of the values whose mask has bit j set
SELECT (as_sum(m, v))[1:4] FROM (VALUES (1::bigint, 10), (2, -7), (3, 5), (3, NULL), (NULL, 100)) t(m, v);
-- worlds that only received negative values (lost by the DuckDB counter finalize)
SELECT (as_sum(3, v))[1:3] FROM (VALUES (-5), (-7)) t(v);
-- extreme magnitudes
SELECT (as_sum(1, v))[1:2] FROM (VALUES (9223372036854775807::bigint)) t(v);
SELECT (as_sum(1, v))[1:2] FROM (VALUES ((-9223372036854775807)::bigint - 1)) t(v);
-- floating point and numeric
SELECT (as_sum(-1, v))[1:2] FROM (VALUES (1.5::float8), (2.25)) t(v);
SELECT (as_sum(-1, v))[1:2] FROM (VALUES (1.5::numeric), (2.25)) t(v);
-- empty input: all worlds 0
SELECT as_sum(m, vi) = array_fill(0::real, ARRAY[64]) FROM st WHERE false;

-- integers below 4096 are summed exactly, also after many cascades
SELECT (as_sum(-1, v))[1] = (2 * sum(v))::real AS exact_small FROM generate_series(1, 200000) v0, LATERAL (SELECT (v0 % 4096)::int AS v) s;

-- every world equals the naive conditional SUM. Integers with |v| < 4096 (smallint here) are exact;
-- larger ones are truncated to 9-12 significant bits, so |error| <= 2^-8 * sum(|v|); floating point
-- accumulates in float32
WITH a AS (SELECT g, as_sum(m, vs) s_s, as_sum(m, vi) s_i, as_sum(m, vb) s_b, as_sum(m, vd) s_d,
                  as_sum(m, vr) s_r, as_sum(m, vn) s_n FROM st GROUP BY g),
     n AS (SELECT g, j, 2 * sum(vs) FILTER (WHERE (m >> j) & 1 = 1) e_s,
                  2 * sum(vi) FILTER (WHERE (m >> j) & 1 = 1) e_i, 2 * sum(abs(vi)) FILTER (WHERE (m >> j) & 1 = 1) a_i,
                  2 * sum(vb) FILTER (WHERE (m >> j) & 1 = 1) e_b, 2 * sum(abs(vb)) FILTER (WHERE (m >> j) & 1 = 1) a_b,
                  2 * sum(vd) FILTER (WHERE (m >> j) & 1 = 1) e_d, 2 * sum(vn) FILTER (WHERE (m >> j) & 1 = 1) e_n
           FROM st, generate_series(0, 63) j GROUP BY g, j),
     r AS (SELECT abs(s_s[j + 1] - e_s) / greatest(abs(e_s), 1) AS r_s, abs(s_i[j + 1] - e_i) / a_i AS r_i,
                  abs(s_b[j + 1] - e_b) / a_b AS r_b, abs(s_d[j + 1] - e_d) / greatest(abs(e_d), 1) AS r_d,
                  abs(s_r[j + 1] - e_d) / greatest(abs(e_d), 1) AS r_r, abs(s_n[j + 1] - e_n) / greatest(abs(e_n), 1) AS r_n
           FROM a JOIN n USING (g))
SELECT count(*) AS cells, max(r_s) < 1e-6 AS smallint_exact, max(r_i) < 2 ^ (-8) AS integer_ok,
       max(r_b) < 2 ^ (-8) AS bigint_ok, max(r_d) < 1e-4 AS float8_ok, max(r_r) < 1e-4 AS float4_ok,
       max(r_n) < 1e-4 AS numeric_ok
FROM r;
