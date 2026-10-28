-- SIMD-ASA: SIMD-accelerated stochastic aggregation for PostgreSQL.
-- Every as_* aggregate takes a 64-bit membership mask (bit j: the row belongs to
-- world j) and returns one result per world as a real[] of length 64.

\echo Use "CREATE EXTENSION simd_asa" to load this file. \quit

-- ---------------------------------------------------------------------------
-- Membership masks
-- ---------------------------------------------------------------------------

-- DuckDB-compatible integer hash (DuckDB's hash() on BIGINT / INTEGER keys).
CREATE FUNCTION asa_hash(bigint) RETURNS bigint
AS 'MODULE_PATHNAME', 'asa_hash_int8' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION asa_hash(integer) RETURNS bigint
AS 'MODULE_PATHNAME', 'asa_hash_int4' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Membership mask of a sampling entity: hash XOR seed, optionally repaired to exactly
-- 32 set bits (simd_asa.hash_repair, off by default as in DuckDB). Depends on the simd_asa.* settings, hence STABLE.
CREATE FUNCTION priv_hash(bigint) RETURNS bigint
AS 'MODULE_PATHNAME', 'asa_priv_hash' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- Element-wise division of two counter arrays (NULL where den = 0).
CREATE FUNCTION priv_div(real[], real[]) RETURNS real[]
AS 'MODULE_PATHNAME', 'asa_priv_div' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- ---------------------------------------------------------------------------
-- as_count
-- ---------------------------------------------------------------------------

CREATE FUNCTION as_count_trans(internal, bigint) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_count_any_trans(internal, bigint, "any") RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_count_final(internal) RETURNS real[]
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_count_combine(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_count_serial(internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;
CREATE FUNCTION as_count_deserial(bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;

CREATE AGGREGATE as_count(bigint) (
    SFUNC = as_count_trans, STYPE = internal, SSPACE = 112,
    FINALFUNC = as_count_final, COMBINEFUNC = as_count_combine,
    SERIALFUNC = as_count_serial, DESERIALFUNC = as_count_deserial, PARALLEL = SAFE
);
CREATE AGGREGATE as_count(bigint, "any") (
    SFUNC = as_count_any_trans, STYPE = internal, SSPACE = 112,
    FINALFUNC = as_count_final, COMBINEFUNC = as_count_combine,
    SERIALFUNC = as_count_serial, DESERIALFUNC = as_count_deserial, PARALLEL = SAFE
);

-- ---------------------------------------------------------------------------
-- as_sum / as_avg
-- ---------------------------------------------------------------------------

CREATE FUNCTION as_sum_int2_trans(internal, bigint, smallint) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_sum_int4_trans(internal, bigint, integer) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_sum_int8_trans(internal, bigint, bigint) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_sum_float4_trans(internal, bigint, real) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_sum_float8_trans(internal, bigint, double precision) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_sum_numeric_trans(internal, bigint, numeric) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;

CREATE FUNCTION as_sum_int_final(internal) RETURNS real[]
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_sum_int_combine(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_sum_int_serial(internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;
CREATE FUNCTION as_sum_int_deserial(bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION as_sum_float_final(internal) RETURNS real[]
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_sum_float_combine(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_sum_float_serial(internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;
CREATE FUNCTION as_sum_float_deserial(bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION as_avg_int2_trans(internal, bigint, smallint) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_avg_int4_trans(internal, bigint, integer) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_avg_int8_trans(internal, bigint, bigint) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_avg_float4_trans(internal, bigint, real) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_avg_float8_trans(internal, bigint, double precision) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_avg_numeric_trans(internal, bigint, numeric) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;

CREATE FUNCTION as_avg_int_final(internal) RETURNS real[]
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_avg_int_combine(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_avg_int_serial(internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;
CREATE FUNCTION as_avg_int_deserial(bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION as_avg_float_final(internal) RETURNS real[]
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_avg_float_combine(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION as_avg_float_serial(internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;
CREATE FUNCTION as_avg_float_deserial(bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;

-- Integer inputs share the cascading SWAR state; floating-point and numeric inputs
-- share the float[64] state. Separate overloads keep integer arguments on the
-- integer path (PostgreSQL would otherwise prefer double precision).
DO $$
DECLARE
    t record;
BEGIN
    FOR t IN SELECT * FROM (VALUES
        ('smallint', 'int2', 'int', 288), ('integer', 'int4', 'int', 288), ('bigint', 'int8', 'int', 288),
        ('real', 'float4', 'float', 272), ('double precision', 'float8', 'float', 272),
        ('numeric', 'numeric', 'float', 272)) AS v(sqltype, tag, kind, sspace)
    LOOP
        EXECUTE format(
            'CREATE AGGREGATE as_sum(bigint, %s) (SFUNC = as_sum_%s_trans, STYPE = internal, SSPACE = %s, '
            'FINALFUNC = as_sum_%s_final, COMBINEFUNC = as_sum_%s_combine, SERIALFUNC = as_sum_%s_serial, '
            'DESERIALFUNC = as_sum_%s_deserial, PARALLEL = SAFE)',
            t.sqltype, t.tag, t.sspace, t.kind, t.kind, t.kind, t.kind);
        EXECUTE format(
            'CREATE AGGREGATE as_avg(bigint, %s) (SFUNC = as_avg_%s_trans, STYPE = internal, SSPACE = %s, '
            'FINALFUNC = as_avg_%s_final, COMBINEFUNC = as_avg_%s_combine, SERIALFUNC = as_avg_%s_serial, '
            'DESERIALFUNC = as_avg_%s_deserial, PARALLEL = SAFE)',
            t.sqltype, t.tag, t.sspace + 112, t.kind, t.kind, t.kind, t.kind);
    END LOOP;
END
$$;

-- ---------------------------------------------------------------------------
-- as_min / as_max
-- ---------------------------------------------------------------------------

DO $$
DECLARE
    t record;
    op text;
BEGIN
    FOR t IN SELECT * FROM (VALUES
        ('smallint', 'int2', 160), ('integer', 'int4', 288), ('bigint', 'int8', 544),
        ('real', 'float4', 288), ('double precision', 'float8', 544), ('numeric', 'numeric', 544))
        AS v(sqltype, tag, sspace)
    LOOP
        FOREACH op IN ARRAY ARRAY['min', 'max']
        LOOP
            EXECUTE format('CREATE FUNCTION as_%s_%s_trans(internal, bigint, %s) RETURNS internal '
                           'AS %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE',
                           op, t.tag, t.sqltype, 'MODULE_PATHNAME');
            EXECUTE format('CREATE FUNCTION as_%s_%s_final(internal) RETURNS real[] '
                           'AS %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE', op, t.tag, 'MODULE_PATHNAME');
            EXECUTE format('CREATE FUNCTION as_%s_%s_combine(internal, internal) RETURNS internal '
                           'AS %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE', op, t.tag, 'MODULE_PATHNAME');
            EXECUTE format('CREATE FUNCTION as_%s_%s_serial(internal) RETURNS bytea '
                           'AS %L LANGUAGE C STRICT PARALLEL SAFE', op, t.tag, 'MODULE_PATHNAME');
            EXECUTE format('CREATE FUNCTION as_%s_%s_deserial(bytea, internal) RETURNS internal '
                           'AS %L LANGUAGE C STRICT PARALLEL SAFE', op, t.tag, 'MODULE_PATHNAME');
            EXECUTE format(
                'CREATE AGGREGATE as_%s(bigint, %s) (SFUNC = as_%s_%s_trans, STYPE = internal, SSPACE = %s, '
                'FINALFUNC = as_%s_%s_final, COMBINEFUNC = as_%s_%s_combine, SERIALFUNC = as_%s_%s_serial, '
                'DESERIALFUNC = as_%s_%s_deserial, PARALLEL = SAFE)',
                op, t.sqltype, op, t.tag, t.sspace, op, t.tag, op, t.tag, op, t.tag, op, t.tag);
        END LOOP;
    END LOOP;
END
$$;

-- ---------------------------------------------------------------------------
-- Re-aggregation of counter arrays: as_sum/as_count/as_min/as_max(real[])
-- ---------------------------------------------------------------------------

CREATE FUNCTION as_list_serial(internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;
CREATE FUNCTION as_list_deserial(bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;

DO $$
DECLARE
    op text;
BEGIN
    FOREACH op IN ARRAY ARRAY['sum', 'count', 'min', 'max']
    LOOP
        EXECUTE format('CREATE FUNCTION as_list_%s_trans(internal, real[]) RETURNS internal '
                       'AS %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE', op, 'MODULE_PATHNAME');
        EXECUTE format('CREATE FUNCTION as_list_%s_final(internal) RETURNS real[] '
                       'AS %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE', op, 'MODULE_PATHNAME');
        EXECUTE format('CREATE FUNCTION as_list_%s_combine(internal, internal) RETURNS internal '
                       'AS %L LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE', op, 'MODULE_PATHNAME');
        EXECUTE format(
            'CREATE AGGREGATE as_%s(real[]) (SFUNC = as_list_%s_trans, STYPE = internal, SSPACE = 776, '
            'FINALFUNC = as_list_%s_final, COMBINEFUNC = as_list_%s_combine, SERIALFUNC = as_list_serial, '
            'DESERIALFUNC = as_list_deserial, PARALLEL = SAFE)', op, op, op, op);
    END LOOP;
END
$$;
