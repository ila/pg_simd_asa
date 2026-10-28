#!/bin/sh
# Writes the DuckDB references of bench/tpch_crosscheck.py: the 22 TPC-H queries through the
# DuckDB privacy extension (https://github.com/cwida/privacy) without noise.
#
# usage: bench/duckdb_reference.sh <duckdb binary with the privacy extension> <TPC-H database> \
#                                  <privacy repository> <output directory>
set -e
DUCKDB=$1 DB=$2 REPO=$3 OUT=$4
HERE=$(cd "$(dirname "$0")" && pwd)
SCHEMA="$REPO/benchmark/queries/q01_avg/tpch_privacy_schema.sql"
SETTINGS="SET pac_mi=0;
SET threads=1;
SET privacy_seed=42;
SET pac_sample_diversity_check=false;"
mkdir -p "$OUT"

# the automatic PAC rewrite (Q3 groups by a key column, which priv_check protects)
for q in 01 02 03 04 05 06 07 08 09 11 12 13 14 15 16 17 19 20 21; do
    { cat "$SCHEMA"; echo "$SETTINGS"; [ $q = 03 ] && echo "SET priv_check=false;"; echo ".mode csv";
      cat "$HERE/tpch_queries/q$q.sql"; } | "$DUCKDB" "$DB" 2>&1 |
        sed '1,/0 rows/d' | grep -v '^┌\|^│\|^└\|0 rows\|^Note' > "$OUT/out_q$q.csv"
done
# hand-written AS queries for what the compiler rejects (columns of the privacy unit outside aggregates)
for q in 10 18 22; do
    { echo "$SETTINGS"; echo "SET priv_rewrite=false;"; echo ".mode csv";
      cat "$REPO/benchmark/tpch/tpch_as_queries/q$q.sql"; } | "$DUCKDB" "$DB" > "$OUT/out_q$q.csv"
done
# Q22 in the compiler's form for predicates over worlds (counter lists and as_noised)
{ echo "$SETTINGS"; echo "SET priv_rewrite=false;"; echo ".mode csv"; cat "$HERE/duckdb/q22_categorical.sql"; } |
    "$DUCKDB" "$DB" > "$OUT/out_q22_categorical.csv"
