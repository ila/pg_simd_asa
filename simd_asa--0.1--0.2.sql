-- simd_asa 0.1 -> 0.2: sampling-entity metadata and the automatic query rewrite.

\echo Use "ALTER EXTENSION simd_asa UPDATE TO '0.2'" to load this file. \quit

-- ---------------------------------------------------------------------------
-- Ownership metadata
--
-- PostgreSQL extensions cannot add DDL syntax, so the DuckDB statements
--   ALTER TABLE customer ADD PRIVACY_KEY (c_custkey); ALTER TABLE customer SET PU;
--   ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer (c_custkey);
-- become
--   SELECT asa_add_entity('customer', 'c_custkey');
--   SELECT asa_add_link('orders', 'o_custkey', 'customer', 'c_custkey');
-- Links need not be enforced foreign keys.
-- ---------------------------------------------------------------------------

-- Candidate sampling entities; each query uses one (simd_asa.entity).
CREATE TABLE @extschema@.asa_entity (
    relid regclass PRIMARY KEY,
    key_column name NOT NULL
);

-- Each row of relid belongs to the ref_relid row with ref_column = link_column.
CREATE TABLE @extschema@.asa_link (
    relid regclass NOT NULL,
    link_column name NOT NULL,
    ref_relid regclass NOT NULL,
    ref_column name NOT NULL,
    PRIMARY KEY (relid, ref_relid),
    CHECK (relid <> ref_relid)
);

SELECT pg_catalog.pg_extension_config_dump('@extschema@.asa_entity', '');
SELECT pg_catalog.pg_extension_config_dump('@extschema@.asa_link', '');

CREATE FUNCTION @extschema@.asa_check_key_column(tbl regclass, col name) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
    typ regtype;
BEGIN
    SELECT atttypid::regtype INTO typ FROM pg_catalog.pg_attribute
    WHERE attrelid = tbl AND attname = col AND attnum > 0 AND NOT attisdropped;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'column "%" of relation % does not exist', col, tbl;
    END IF;
    IF typ NOT IN ('smallint'::regtype, 'integer'::regtype, 'bigint'::regtype) THEN
        RAISE EXCEPTION 'column %.% has type %; sampling-entity keys and links must be smallint, integer or bigint',
            tbl, col, typ;
    END IF;
END
$$;

-- Declares tbl as a candidate sampling entity identified by key_column.
CREATE FUNCTION @extschema@.asa_add_entity(tbl regclass, key_column name) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM @extschema@.asa_check_key_column(tbl, key_column);
    INSERT INTO @extschema@.asa_entity VALUES (tbl, key_column)
    ON CONFLICT (relid) DO UPDATE SET key_column = EXCLUDED.key_column;
END
$$;

-- Declares that each row of tbl belongs to the ref_tbl row with ref_column = link_column.
CREATE FUNCTION @extschema@.asa_add_link(tbl regclass, link_column name, ref_tbl regclass, ref_column name)
RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    PERFORM @extschema@.asa_check_key_column(tbl, link_column);
    PERFORM @extschema@.asa_check_key_column(ref_tbl, ref_column);
    INSERT INTO @extschema@.asa_link VALUES (tbl, link_column, ref_tbl, ref_column)
    ON CONFLICT (relid, ref_relid)
    DO UPDATE SET link_column = EXCLUDED.link_column, ref_column = EXCLUDED.ref_column;
END
$$;

CREATE FUNCTION @extschema@.asa_drop_entity(tbl regclass) RETURNS void
LANGUAGE sql AS $$ DELETE FROM @extschema@.asa_entity WHERE relid = tbl $$;

CREATE FUNCTION @extschema@.asa_drop_link(tbl regclass, ref_tbl regclass) RETURNS void
LANGUAGE sql AS $$ DELETE FROM @extschema@.asa_link WHERE relid = tbl AND ref_relid = ref_tbl $$;

-- ---------------------------------------------------------------------------
-- Per-world (lane-wise) arithmetic and comparisons on counter arrays; a double precision
-- argument is broadcast to every world. Comparisons return the mask of the worlds where
-- they hold. The rewrite uses them for expressions over aggregates and predicates over
-- worlds; they can also be used directly.
-- ---------------------------------------------------------------------------
DO $$
DECLARE
    fn text;
    sig text;
BEGIN
    FOREACH fn IN ARRAY ARRAY['add', 'sub', 'mul', 'div', 'lt', 'le', 'gt', 'ge', 'eq', 'ne']
    LOOP
        FOREACH sig IN ARRAY ARRAY['real[], real[]', 'real[], double precision', 'double precision, real[]']
        LOOP
            EXECUTE format('CREATE FUNCTION @extschema@.asa_lane_%s(%s) RETURNS %s AS %L, %L '
                           'LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE',
                           fn, sig, CASE WHEN fn IN ('add', 'sub', 'mul', 'div') THEN 'real[]' ELSE 'bigint' END,
                           'MODULE_PATHNAME', 'asa_lane_' || fn);
        END LOOP;
    END LOOP;
END
$$;
CREATE FUNCTION @extschema@.asa_lane_neg(real[]) RETURNS real[]
AS 'MODULE_PATHNAME', 'asa_lane_neg' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
