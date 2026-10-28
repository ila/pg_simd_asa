SET extra_float_digits = 0;
CREATE TABLE mt AS
SELECT k % 10 AS g, priv_hash(asa_hash(k::bigint)) AS m,
       (k % 30000 - 15000)::smallint AS vs, (k::bigint * 2654435761 % 1000003 - 500000)::integer AS vi,
       (k::bigint * 2654435761 % 100000000007 - 50000000000)::bigint AS vb,
       ((k * 7919) % 100003 / 7.0 - 7000)::real AS vr, ((k * 7919) % 100003 / 7.0 - 7000)::float8 AS vd,
       ((k * 7919) % 100003 / 7.0 - 7000)::numeric AS vn
FROM generate_series(1, 50000) k;

SELECT (as_min(m, v))[1:4] AS mn, (as_max(m, v))[1:4] AS mx
FROM (VALUES (1::bigint, 10), (2, -7), (3, 5), (3, NULL)) t(m, v);
SELECT as_max(m, vi) = array_fill(0::real, ARRAY[64]) FROM mt WHERE false;

-- every world equals the naive conditional MIN/MAX (values are exactly representable in real
-- except bigint and numeric, which are compared after the same rounding to real)
WITH a AS (SELECT g, as_min(m, vs) mn_s, as_max(m, vs) mx_s, as_min(m, vi) mn_i, as_max(m, vi) mx_i,
                  as_min(m, vb) mn_b, as_max(m, vb) mx_b, as_min(m, vr) mn_r, as_max(m, vr) mx_r,
                  as_min(m, vd) mn_d, as_max(m, vd) mx_d, as_min(m, vn) mn_n, as_max(m, vn) mx_n
           FROM mt GROUP BY g),
     n AS (SELECT g, j,
                  min(vs) FILTER (WHERE (m >> j) & 1 = 1) e_mn_s, max(vs) FILTER (WHERE (m >> j) & 1 = 1) e_mx_s,
                  min(vi) FILTER (WHERE (m >> j) & 1 = 1) e_mn_i, max(vi) FILTER (WHERE (m >> j) & 1 = 1) e_mx_i,
                  min(vb) FILTER (WHERE (m >> j) & 1 = 1) e_mn_b, max(vb) FILTER (WHERE (m >> j) & 1 = 1) e_mx_b,
                  min(vr) FILTER (WHERE (m >> j) & 1 = 1) e_mn_r, max(vr) FILTER (WHERE (m >> j) & 1 = 1) e_mx_r,
                  min(vd) FILTER (WHERE (m >> j) & 1 = 1) e_mn_d, max(vd) FILTER (WHERE (m >> j) & 1 = 1) e_mx_d,
                  min(vn) FILTER (WHERE (m >> j) & 1 = 1) e_mn_n, max(vn) FILTER (WHERE (m >> j) & 1 = 1) e_mx_n
           FROM mt, generate_series(0, 63) j GROUP BY g, j)
SELECT count(*) AS cells,
       count(*) FILTER (WHERE mn_s[j + 1] <> e_mn_s OR mx_s[j + 1] <> e_mx_s) AS smallint_mismatch,
       count(*) FILTER (WHERE mn_i[j + 1] <> e_mn_i OR mx_i[j + 1] <> e_mx_i) AS integer_mismatch,
       count(*) FILTER (WHERE mn_b[j + 1] <> e_mn_b::real OR mx_b[j + 1] <> e_mx_b::real) AS bigint_mismatch,
       count(*) FILTER (WHERE mn_r[j + 1] <> e_mn_r OR mx_r[j + 1] <> e_mx_r) AS real_mismatch,
       count(*) FILTER (WHERE mn_d[j + 1] <> e_mn_d::real OR mx_d[j + 1] <> e_mx_d::real) AS float8_mismatch,
       count(*) FILTER (WHERE mn_n[j + 1] <> e_mn_n::float8::real OR mx_n[j + 1] <> e_mx_n::float8::real) AS numeric_mismatch
FROM a JOIN n USING (g);
