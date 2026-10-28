#!/usr/bin/env bash
# Cross-validates the PostgreSQL port against the DuckDB `privacy` extension:
# masks (priv_hash), and every as_* aggregate per group and per world.
#
# usage: DUCKDB=/path/to/duckdb PRIVACY_EXT=/path/to/privacy.duckdb_extension \
#        PSQL="psql -h localhost -p 5432 -U postgres" test/crosscheck_duckdb.sh [rows]
set -euo pipefail

ROWS=${1:-2000000}
DUCKDB=${DUCKDB:?set DUCKDB to the duckdb binary}
PRIVACY_EXT=${PRIVACY_EXT:?set PRIVACY_EXT to privacy.duckdb_extension}
PSQL=${PSQL:-psql}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

AGGS="as_count(m) AS cnt, as_count(m, vi) AS cnt_v,
      as_sum(m, vi) AS sum_i, as_sum(m, vb) AS sum_b, as_sum(m, vs) AS sum_s, as_sum(m, vd) AS sum_d,
      as_min(m, vi) AS min_i, as_max(m, vb) AS max_b, as_min(m, vd) AS min_d, as_max(m, vd) AS max_d"
COLS="cnt cnt_v sum_i sum_b sum_s sum_d min_i max_b min_d max_d avg_i avg_d"

# 1. Data + DuckDB masks and results. Masks are exported as signed BIGINT bit patterns.
"$DUCKDB" -unsigned <<EOF
LOAD '$PRIVACY_EXT';
SET pac_mi = 0; SET privacy_seed = 42;
CREATE TABLE t AS
SELECT i % 100 AS g, (hash(i * 7919) % 50000)::BIGINT AS k,
       CASE WHEN i % 13 = 0 THEN NULL ELSE (i % 1000) - 300 END::INTEGER AS vi,
       (hash(i) % 2000000000000)::BIGINT - 500000000000 AS vb,
       (i % 200 - 50)::SMALLINT AS vs,
       ((i % 10007) / 7.0 - 300.0)::DOUBLE AS vd
FROM range($ROWS) r(i);
CREATE TABLE tm AS SELECT *, priv_hash(hash(k)) AS m FROM t;
COPY (SELECT g, k, vi, vb, vs, vd,
             (m::HUGEINT - CASE WHEN m >= 9223372036854775808 THEN 18446744073709551616 ELSE 0 END)::BIGINT AS duck_mask
      FROM tm) TO '$WORK/data.csv' (HEADER);
-- DuckDB prints lists as [..]; PostgreSQL array literals use {..}
COPY (
  SELECT g, replace(replace(COLUMNS(* EXCLUDE (g))::VARCHAR, '[', '{'), ']', '}') FROM (
    SELECT g, $AGGS,
           priv_div(as_sum(m, vi), as_count(m, vi)) AS avg_i, priv_div(as_sum(m, vd), as_count(m, vd)) AS avg_d
    FROM tm GROUP BY g)
) TO '$WORK/duck.csv' (HEADER);
EOF

# 2. PostgreSQL: load, check masks, compute the same aggregates.
$PSQL -X -q -v ON_ERROR_STOP=1 <<EOF
SET simd_asa.seed = 42;
DROP TABLE IF EXISTS xc_data, xc_duck, xc_pg;
CREATE TABLE xc_data (g int, k bigint, vi integer, vb bigint, vs smallint, vd float8, duck_mask bigint);
\copy xc_data FROM '$WORK/data.csv' CSV HEADER
CREATE TABLE xc_duck (g int, $(for c in $COLS; do printf '%s real[], ' "$c"; done | sed 's/, $//'));
\copy xc_duck FROM '$WORK/duck.csv' CSV HEADER
\echo === masks: rows where priv_hash(asa_hash(k)) differs from DuckDB priv_hash(hash(k)) (expect 0)
SELECT count(*) AS mask_mismatches FROM xc_data WHERE priv_hash(asa_hash(k)) <> duck_mask;
CREATE TABLE xc_pg AS
SELECT g, $AGGS, as_avg(m, vi) AS avg_i, as_avg(m, vd) AS avg_d
FROM (SELECT *, priv_hash(asa_hash(k)) AS m FROM xc_data) s GROUP BY g;
\echo === per aggregate: max relative difference over all groups and 64 worlds
$(for c in $COLS; do cat <<SQL
SELECT '$c' AS agg, count(*) AS cells,
       count(*) FILTER (WHERE (p IS NULL) <> (d IS NULL)) AS null_mismatch,
       coalesce(max(abs(p - d) / greatest(abs(d), 1e-6)), 0)::numeric(12, 9) AS max_rel_diff
FROM xc_pg x JOIN xc_duck y USING (g), unnest(x.$c, y.$c) AS u(p, d);
SQL
done)
DROP TABLE xc_data, xc_duck, xc_pg;
EOF
