CREATE EXTENSION simd_asa;

-- asa_hash is DuckDB's hash() on integers (values taken from DuckDB)
SELECT asa_hash(1::bigint) = 4717996019076358352 AS duckdb_hash_1,
       asa_hash(42::bigint) = 7199933130570745587 AS duckdb_hash_42,
       asa_hash(42) = asa_hash(42::bigint) AS int4_matches_int8;

-- priv_hash = hash XOR f(seed). DuckDB (privacy_seed = 42) returns the UBIGINTs 16305503821874362565
-- and 11759313366594131989 below; here they are printed as the same bit patterns in a signed bigint
SHOW simd_asa.seed;
SHOW simd_asa.hash_repair;
SELECT priv_hash(0) = (16305503821874362565 - 2::numeric ^ 64)::bigint AS duckdb_seed_42;
SELECT priv_hash(asa_hash(1::bigint)) = (11759313366594131989 - 2::numeric ^ 64)::bigint AS duckdb_key_1;

-- the seed changes the assignment of entities to worlds
SET simd_asa.seed = 7;
SELECT priv_hash(asa_hash(1::bigint)) <> (11759313366594131989 - 2::numeric ^ 64)::bigint AS reseeded;
RESET simd_asa.seed;

-- with repair, every entity belongs to exactly 32 of the 64 worlds
SET simd_asa.hash_repair = on;
SELECT min(bit_count(priv_hash(asa_hash(k))::bit(64))) AS min_bits,
       max(bit_count(priv_hash(asa_hash(k))::bit(64))) AS max_bits
FROM generate_series(1, 100000) k;
RESET simd_asa.hash_repair;

SELECT priv_hash(NULL) IS NULL AS strict;
