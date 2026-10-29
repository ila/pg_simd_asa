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

-- ---------------------------------------------------------------------------
-- PAC noise release (asa_noise.c)
--
-- as_noised_count/sum/avg/min/max(mask [, value]) release one noised value per group,
-- of the type the corresponding PostgreSQL aggregate returns: the PAC noise of the DuckDB
-- extension (simd_asa.noise, simd_asa.mi). They share the transition functions of the
-- as_* aggregates. The rewrite uses the asa_noised_* / asa_counters_* forms, whose last
-- arguments are the statement's query hash and the correction 2^(k-1) for rows derived
-- from k entities (DuckDB's correction: such rows are in a world with probability 2^-k;
-- it multiplies counts and sums and makes NULL releases less likely). Equal arguments let
-- PostgreSQL share one transition state between a noised aggregate and its counters
-- (HAVING, top-k).
-- ---------------------------------------------------------------------------

-- The query hash priv_hash(bigint) and the as_noised_* aggregates use: derived from
-- simd_asa.seed (the rewrite mixes in a per-statement nonce when noise is on).
CREATE FUNCTION @extschema@.asa_query_hash() RETURNS bigint
AS 'MODULE_PATHNAME', 'asa_query_hash' LANGUAGE C STABLE PARALLEL SAFE;

-- Mask under an explicit query hash.
CREATE FUNCTION @extschema@.priv_hash(bigint, bigint) RETURNS bigint
AS 'MODULE_PATHNAME', 'asa_priv_hash_q' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- Noised release of a counter array (e.g. an expression evaluated per world).
CREATE FUNCTION @extschema@.as_noised(real[]) RETURNS double precision
AS 'MODULE_PATHNAME', 'as_noised' LANGUAGE C VOLATILE STRICT PARALLEL RESTRICTED;
CREATE FUNCTION @extschema@.asa_noised(real[], bigint) RETURNS double precision
AS 'MODULE_PATHNAME', 'asa_noised' LANGUAGE C VOLATILE STRICT PARALLEL RESTRICTED;

-- Whether a row passes a predicate that holds in the worlds of mask.
CREATE FUNCTION @extschema@.priv_filter(bigint) RETURNS boolean
AS 'MODULE_PATHNAME', 'as_priv_filter' LANGUAGE C VOLATILE STRICT PARALLEL SAFE;
CREATE FUNCTION @extschema@.asa_filter(bigint, bigint) RETURNS boolean
AS 'MODULE_PATHNAME', 'asa_filter' LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

-- Mean of the non-NULL worlds (ranking before noise).
CREATE FUNCTION @extschema@.priv_mean(real[]) RETURNS double precision
AS 'MODULE_PATHNAME', 'as_priv_mean' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Transition functions with trailing query-hash and correction arguments, which they store
-- in the state for the final functions.
CREATE FUNCTION @extschema@.as_count_qtrans(internal, bigint, bigint, double precision) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION @extschema@.as_count_any_qtrans(internal, bigint, "any", bigint, double precision) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;

-- Noised numeric sums and averages are summed as DuckDB's fused as_noised_sum sums DECIMAL:
-- integers times 10^scale on the integer kernel. Counter arrays of numeric keep the float
-- path, as DuckDB's as_sum lists.
CREATE FUNCTION @extschema@.as_sum_numeric_scaled_trans(internal, bigint, numeric) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION @extschema@.as_avg_numeric_scaled_trans(internal, bigint, numeric) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;

DO $$
DECLARE
    form record;
    t record;
    op text;
    kind text;
BEGIN
    -- noised final functions: as_* (seed's query hash) and asa_* (query hash argument)
    FOR form IN SELECT * FROM (VALUES ('as', 'noised'), ('asa', 'qnoised')) AS v(prefix, sym) LOOP
        EXECUTE format('CREATE FUNCTION @extschema@.%s_noised_count_final(internal) RETURNS bigint '
                       'AS %L, %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                       form.prefix, 'MODULE_PATHNAME', 'as_count_' || form.sym || '_final');
    END LOOP;

    -- ckind / nkind: state kind of the counter / noised forms; ntrans: C transition of the noised form
    FOR t IN SELECT * FROM (VALUES
        ('smallint', 'int2', 'int', 'int', 'int2', 'bigint', 'numeric', 288, 288, 160),
        ('integer', 'int4', 'int', 'int', 'int4', 'bigint', 'numeric', 288, 288, 288),
        ('bigint', 'int8', 'int', 'int', 'int8', 'numeric', 'numeric', 288, 288, 544),
        ('real', 'float4', 'float', 'float', 'float4', 'real', 'double precision', 272, 272, 288),
        ('double precision', 'float8', 'float', 'float', 'float8', 'double precision', 'double precision', 272, 272, 544),
        ('numeric', 'numeric', 'float', 'int', 'numeric_scaled', 'numeric', 'numeric', 272, 288, 544))
        AS v(sqltype, tag, ckind, nkind, ntrans, sumtype, avgtype, cspace, nspace, mmspace)
    LOOP
        -- query-hash transitions: counters (as as_*) and noised
        FOREACH op IN ARRAY ARRAY['sum', 'avg'] LOOP
            EXECUTE format('CREATE FUNCTION @extschema@.as_%s_%s_qtrans(internal, bigint, %s, bigint, double precision) '
                           'RETURNS internal '
                           'AS %L, %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                           op, t.tag, t.sqltype, 'MODULE_PATHNAME', 'as_' || op || '_' || t.tag || '_qtrans');
            IF t.ntrans <> t.tag THEN
                EXECUTE format('CREATE FUNCTION @extschema@.as_%s_%s_qtrans(internal, bigint, %s, bigint, double precision) '
                           'RETURNS internal '
                               'AS %L, %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                               op, t.ntrans, t.sqltype, 'MODULE_PATHNAME', 'as_' || op || '_' || t.ntrans || '_qtrans');
            END IF;
        END LOOP;
        FOREACH op IN ARRAY ARRAY['min', 'max'] LOOP
            EXECUTE format('CREATE FUNCTION @extschema@.as_%s_%s_qtrans(internal, bigint, %s, bigint, double precision) '
                           'RETURNS internal '
                           'AS %L, %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                           op, t.tag, t.sqltype, 'MODULE_PATHNAME', 'as_' || op || '_' || t.tag || '_qtrans');
        END LOOP;

        FOR form IN SELECT * FROM (VALUES ('as', 'noised'), ('asa', 'qnoised')) AS v(prefix, sym) LOOP
            EXECUTE format('CREATE FUNCTION @extschema@.%s_noised_sum_%s_final(internal) RETURNS %s '
                           'AS %L, %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                           form.prefix, t.tag, t.sumtype, 'MODULE_PATHNAME', 'as_sum_' || t.nkind || '_' || form.sym || '_final');
            EXECUTE format('CREATE FUNCTION @extschema@.%s_noised_avg_%s_final(internal) RETURNS %s '
                           'AS %L, %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                           form.prefix, t.tag, t.avgtype, 'MODULE_PATHNAME', 'as_avg_' || t.nkind || '_' || form.sym || '_final');
            FOREACH op IN ARRAY ARRAY['min', 'max'] LOOP
                EXECUTE format('CREATE FUNCTION @extschema@.%s_noised_%s_%s_final(internal) RETURNS %s '
                               'AS %L, %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                               form.prefix, op, t.tag, t.sqltype, 'MODULE_PATHNAME',
                               'as_' || op || '_' || t.tag || '_' || form.sym || '_final');
            END LOOP;
        END LOOP;

        EXECUTE format('CREATE FUNCTION @extschema@.asa_counters_sum_%s_final(internal) RETURNS real[] '
                       'AS %L, %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                       t.tag, 'MODULE_PATHNAME', 'as_sum_' || t.ckind || '_counters_final');
        FOREACH op IN ARRAY ARRAY['min', 'max'] LOOP
            EXECUTE format('CREATE FUNCTION @extschema@.asa_counters_%s_%s_final(internal) RETURNS real[] '
                           'AS %L, %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                           op, t.tag, 'MODULE_PATHNAME', 'as_' || op || '_' || t.tag || '_counters_final');
        END LOOP;

        FOREACH op IN ARRAY ARRAY['sum', 'avg'] LOOP
            -- as_noised_sum/avg(mask, value)
            EXECUTE format(
                'CREATE AGGREGATE @extschema@.as_noised_%s(bigint, %s) (SFUNC = @extschema@.as_%s_%s_trans, '
                'STYPE = internal, SSPACE = %s, FINALFUNC = @extschema@.as_noised_%s_%s_final, '
                'COMBINEFUNC = @extschema@.as_%s_%s_combine, SERIALFUNC = @extschema@.as_%s_%s_serial, '
                'DESERIALFUNC = @extschema@.as_%s_%s_deserial, PARALLEL = SAFE)',
                op, t.sqltype, op, t.ntrans, t.nspace + CASE op WHEN 'avg' THEN 112 ELSE 0 END, op, t.tag,
                op, t.nkind, op, t.nkind, op, t.nkind);
            -- asa_noised_sum/avg(mask, value, query_hash, correction)
            EXECUTE format(
                'CREATE AGGREGATE @extschema@.asa_noised_%s(bigint, %s, bigint, double precision) (SFUNC = @extschema@.as_%s_%s_qtrans, '
                'STYPE = internal, SSPACE = %s, FINALFUNC = @extschema@.asa_noised_%s_%s_final, '
                'COMBINEFUNC = @extschema@.as_%s_%s_combine, SERIALFUNC = @extschema@.as_%s_%s_serial, '
                'DESERIALFUNC = @extschema@.as_%s_%s_deserial, PARALLEL = SAFE)',
                op, t.sqltype, op, t.ntrans, t.nspace + CASE op WHEN 'avg' THEN 112 ELSE 0 END, op, t.tag,
                op, t.nkind, op, t.nkind, op, t.nkind);
            -- asa_counters_sum/avg(mask, value, query_hash, correction): as as_sum/as_avg, NULL without rows
            EXECUTE format(
                'CREATE AGGREGATE @extschema@.asa_counters_%s(bigint, %s, bigint, double precision) (SFUNC = @extschema@.as_%s_%s_qtrans, '
                'STYPE = internal, SSPACE = %s, FINALFUNC = @extschema@.%s, '
                'COMBINEFUNC = @extschema@.as_%s_%s_combine, SERIALFUNC = @extschema@.as_%s_%s_serial, '
                'DESERIALFUNC = @extschema@.as_%s_%s_deserial, PARALLEL = SAFE)',
                op, t.sqltype, op, t.tag, t.cspace + CASE op WHEN 'avg' THEN 112 ELSE 0 END,
                CASE op WHEN 'sum' THEN format('asa_counters_sum_%s_final', t.tag)
                        ELSE format('as_avg_%s_final', t.ckind) END,
                op, t.ckind, op, t.ckind, op, t.ckind);
        END LOOP;

        FOREACH op IN ARRAY ARRAY['min', 'max'] LOOP
            EXECUTE format(
                'CREATE AGGREGATE @extschema@.as_noised_%s(bigint, %s) (SFUNC = @extschema@.as_%s_%s_trans, STYPE = internal, '
                'SSPACE = %s, FINALFUNC = @extschema@.as_noised_%s_%s_final, COMBINEFUNC = @extschema@.as_%s_%s_combine, '
                'SERIALFUNC = @extschema@.as_%s_%s_serial, DESERIALFUNC = @extschema@.as_%s_%s_deserial, PARALLEL = SAFE)',
                op, t.sqltype, op, t.tag, t.mmspace, op, t.tag, op, t.tag, op, t.tag, op, t.tag);
            FOREACH kind IN ARRAY ARRAY['noised', 'counters'] LOOP
                EXECUTE format(
                    'CREATE AGGREGATE @extschema@.asa_%s_%s(bigint, %s, bigint, double precision) (SFUNC = @extschema@.as_%s_%s_qtrans, '
                    'STYPE = internal, SSPACE = %s, FINALFUNC = @extschema@.%s, '
                    'COMBINEFUNC = @extschema@.as_%s_%s_combine, SERIALFUNC = @extschema@.as_%s_%s_serial, '
                    'DESERIALFUNC = @extschema@.as_%s_%s_deserial, PARALLEL = SAFE)',
                    kind, op, t.sqltype, op, t.tag, t.mmspace,
                    CASE kind WHEN 'noised' THEN format('asa_noised_%s_%s_final', op, t.tag)
                              ELSE format('asa_counters_%s_%s_final', op, t.tag) END,
                    op, t.tag, op, t.tag, op, t.tag);
            END LOOP;
        END LOOP;
    END LOOP;
END
$$;

CREATE AGGREGATE @extschema@.as_noised_count(bigint) (
    SFUNC = @extschema@.as_count_trans, STYPE = internal, SSPACE = 112,
    FINALFUNC = @extschema@.as_noised_count_final, COMBINEFUNC = @extschema@.as_count_combine,
    SERIALFUNC = @extschema@.as_count_serial, DESERIALFUNC = @extschema@.as_count_deserial, PARALLEL = SAFE
);
CREATE AGGREGATE @extschema@.as_noised_count(bigint, "any") (
    SFUNC = @extschema@.as_count_any_trans, STYPE = internal, SSPACE = 112,
    FINALFUNC = @extschema@.as_noised_count_final, COMBINEFUNC = @extschema@.as_count_combine,
    SERIALFUNC = @extschema@.as_count_serial, DESERIALFUNC = @extschema@.as_count_deserial, PARALLEL = SAFE
);
CREATE AGGREGATE @extschema@.asa_noised_count(bigint, bigint, double precision) (
    SFUNC = @extschema@.as_count_qtrans, STYPE = internal, SSPACE = 112,
    FINALFUNC = @extschema@.asa_noised_count_final, COMBINEFUNC = @extschema@.as_count_combine,
    SERIALFUNC = @extschema@.as_count_serial, DESERIALFUNC = @extschema@.as_count_deserial, PARALLEL = SAFE
);
CREATE AGGREGATE @extschema@.asa_noised_count(bigint, "any", bigint, double precision) (
    SFUNC = @extschema@.as_count_any_qtrans, STYPE = internal, SSPACE = 112,
    FINALFUNC = @extschema@.asa_noised_count_final, COMBINEFUNC = @extschema@.as_count_combine,
    SERIALFUNC = @extschema@.as_count_serial, DESERIALFUNC = @extschema@.as_count_deserial, PARALLEL = SAFE
);
CREATE AGGREGATE @extschema@.asa_counters_count(bigint, bigint, double precision) (
    SFUNC = @extschema@.as_count_qtrans, STYPE = internal, SSPACE = 112,
    FINALFUNC = @extschema@.as_count_final, COMBINEFUNC = @extschema@.as_count_combine,
    SERIALFUNC = @extschema@.as_count_serial, DESERIALFUNC = @extschema@.as_count_deserial, PARALLEL = SAFE
);
CREATE AGGREGATE @extschema@.asa_counters_count(bigint, "any", bigint, double precision) (
    SFUNC = @extschema@.as_count_any_qtrans, STYPE = internal, SSPACE = 112,
    FINALFUNC = @extschema@.as_count_final, COMBINEFUNC = @extschema@.as_count_combine,
    SERIALFUNC = @extschema@.as_count_serial, DESERIALFUNC = @extschema@.as_count_deserial, PARALLEL = SAFE
);

-- ---------------------------------------------------------------------------
-- DISTINCT aggregates (asa_distinct.c)
--
-- asa_noised_*_distinct / asa_counters_*_distinct(mask, value, query_hash, correction
-- ORDER BY value): count/sum/avg(DISTINCT value) for the rewrite. A distinct value is in
-- the worlds of any row carrying it (the OR of their masks, DuckDB's bit_or pre-
-- aggregation); NULL values are skipped. They must be ordered by the value, so that equal
-- values arrive together, and are not parallelized (as PostgreSQL's own DISTINCT
-- aggregates). min/max(DISTINCT) equal min/max.
-- ---------------------------------------------------------------------------
CREATE FUNCTION @extschema@.as_count_any_distinct_trans(internal, bigint, "any", bigint, double precision)
RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION @extschema@.asa_noised_count_distinct_final(internal)
RETURNS bigint AS 'MODULE_PATHNAME', 'as_count_distinct_noised_final' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION @extschema@.asa_counters_count_distinct_final(internal)
RETURNS real[] AS 'MODULE_PATHNAME', 'as_count_distinct_counters_final' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE AGGREGATE @extschema@.asa_noised_count_distinct(bigint, "any", bigint, double precision) (
    SFUNC = @extschema@.as_count_any_distinct_trans, STYPE = internal,
    FINALFUNC = @extschema@.asa_noised_count_distinct_final, FINALFUNC_MODIFY = SHAREABLE,
    PARALLEL = SAFE
);
CREATE AGGREGATE @extschema@.asa_counters_count_distinct(bigint, "any", bigint, double precision) (
    SFUNC = @extschema@.as_count_any_distinct_trans, STYPE = internal,
    FINALFUNC = @extschema@.asa_counters_count_distinct_final, FINALFUNC_MODIFY = SHAREABLE,
    PARALLEL = SAFE
);

DO $$
DECLARE
    t record;
    op text;
    kind text;
BEGIN
    -- ntrans / ctrans: base transition of the noised / counter form; nkind / ckind: their final
    FOR t IN SELECT * FROM (VALUES
        ('smallint', 'int2', 'int2', 'int2', 'int', 'int', 'bigint', 'numeric'),
        ('integer', 'int4', 'int4', 'int4', 'int', 'int', 'bigint', 'numeric'),
        ('bigint', 'int8', 'int8', 'int8', 'int', 'int', 'numeric', 'numeric'),
        ('real', 'float4', 'float4', 'float4', 'float', 'float', 'real', 'double precision'),
        ('double precision', 'float8', 'float8', 'float8', 'float', 'float', 'double precision', 'double precision'),
        ('numeric', 'numeric', 'numeric_scaled', 'numeric', 'int', 'float', 'numeric', 'numeric'))
        AS v(sqltype, tag, ntrans, ctrans, nkind, ckind, sumtype, avgtype)
    LOOP
        FOREACH op IN ARRAY ARRAY['sum', 'avg'] LOOP
            EXECUTE format('CREATE FUNCTION @extschema@.as_%s_%s_distinct_trans(internal, bigint, %s, bigint, '
                           'double precision) RETURNS internal AS %L, %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                           op, t.tag, t.sqltype, 'MODULE_PATHNAME', 'as_' || op || '_' || t.tag || '_distinct_trans');
            IF t.ntrans <> t.ctrans THEN
                EXECUTE format('CREATE FUNCTION @extschema@.as_%s_%s_distinct_trans(internal, bigint, %s, bigint, '
                               'double precision) RETURNS internal AS %L, %L LANGUAGE C CALLED ON NULL INPUT '
                               'PARALLEL SAFE',
                               op, t.ntrans, t.sqltype, 'MODULE_PATHNAME', 'as_' || op || '_' || t.ntrans || '_distinct_trans');
            END IF;
            FOREACH kind IN ARRAY ARRAY['noised', 'counters'] LOOP
                EXECUTE format('CREATE FUNCTION @extschema@.asa_%s_%s_distinct_%s_final(internal) RETURNS %s AS %L, %L '
                               'LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                               kind, op, t.tag,
                               CASE WHEN kind = 'counters' THEN 'real[]' WHEN op = 'sum' THEN t.sumtype ELSE t.avgtype END,
                               'MODULE_PATHNAME',
                               format('as_%s_%s_distinct_%s_final', op,
                                      CASE kind WHEN 'noised' THEN t.nkind ELSE t.ckind END, kind));
                EXECUTE format('CREATE AGGREGATE @extschema@.asa_%s_%s_distinct(bigint, %s, bigint, double precision) '
                               '(SFUNC = @extschema@.as_%s_%s_distinct_trans, STYPE = internal, '
                               'FINALFUNC = @extschema@.asa_%s_%s_distinct_%s_final, '
                               'FINALFUNC_MODIFY = SHAREABLE, PARALLEL = SAFE)',
                               kind, op, t.sqltype, op, CASE kind WHEN 'noised' THEN t.ntrans ELSE t.ctrans END,
                               kind, op, t.tag);
            END LOOP;
        END LOOP;
    END LOOP;
END
$$;
