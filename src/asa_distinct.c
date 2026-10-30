/*
 * asa_distinct.c — count/sum/avg(DISTINCT value) over world masks.
 *
 * A distinct value is in world j if any row carrying it is: its mask is the OR of those
 * rows' masks. DuckDB pre-aggregates (bit_or(hash) GROUP BY groups, value) and runs the
 * ordinary aggregate over the distinct values (InsertDistinctPreAggregation). Here the
 * asa_*_distinct aggregates are ordered by their value (asa_noised_count_distinct(mask, v,
 * query_hash, correction ORDER BY v), as the rewrite writes them): PostgreSQL sorts each
 * group's input, as for its own count(DISTINCT), and the transition ORs the masks of a run
 * of equal values, feeding the base asa_* transition once per distinct value, so that the
 * base state, and thus every release, is that of DuckDB's pre-aggregated rows. NULL values
 * are skipped, as by DISTINCT.
 *
 * The base aggregates' finals run on the base state. Ordered aggregates are never
 * partial, so there are no combine functions.
 */
#include "asa_common.h"

#include "catalog/pg_type.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

typedef struct AsaDistinctState
{
	Datum		base;			/* state of the base transition */
	bool		base_null;
	PGFunction	base_trans;
	FmgrInfo   *flinfo;			/* of the transition: argument types for the base */
	Oid			collation;
	TypeCacheEntry *type;		/* comparison of values */
	bool		pending;		/* a run of equal values not yet fed to the base */
	Datum		value;
	uint64		mask;			/* OR of the run's masks */
	Datum		query_hash;
	Datum		correction;
} AsaDistinctState;

/* Feeds the pending distinct value to the base transition (once: finals may share a state). */
static void
asa_distinct_flush(AsaDistinctState *s, fmNodePtr context)
{
	LOCAL_FCINFO(fc, 5);
	Datum		result;

	if (!s->pending)
		return;
	InitFunctionCallInfoData(*fc, s->flinfo, 5, s->collation, context, NULL);
	fc->args[0].value = s->base;
	fc->args[0].isnull = s->base_null;
	fc->args[1].value = Int64GetDatum((int64) s->mask);
	fc->args[1].isnull = false;
	fc->args[2].value = s->value;
	fc->args[2].isnull = false;
	fc->args[3].value = s->query_hash;
	fc->args[3].isnull = false;
	fc->args[4].value = s->correction;
	fc->args[4].isnull = false;
	result = s->base_trans(fc);
	s->base = result;
	s->base_null = fc->isnull;
	s->pending = false;
}

/* The aggregate must be ordered by its value argument (the second). */
static void
asa_distinct_check_order(FunctionCallInfo fcinfo)
{
	Aggref	   *aggref = AggGetAggref(fcinfo);
	const char *name = aggref ? get_func_name(aggref->aggfnoid) : "asa_*_distinct";
	SortGroupClause *first;
	ListCell   *lc;

	if (aggref && aggref->aggorder != NIL)
	{
		first = linitial_node(SortGroupClause, aggref->aggorder);
		foreach(lc, aggref->args)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			if (tle->ressortgroupref == first->tleSortGroupRef && tle->resno == 2)
				return;
		}
	}
	ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("%s must be ordered by its value argument", name),
					errhint("Write %s(mask, value, query_hash, correction ORDER BY value).", name)));
}

static Datum
asa_distinct_trans(FunctionCallInfo fcinfo, PGFunction base_trans, const char *name)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, name);
	AsaDistinctState *s = PG_ARGISNULL(0) ? NULL : (AsaDistinctState *) PG_GETARG_POINTER(0);
	Datum		value;

	if (!s)
	{
		Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 2);

		asa_distinct_check_order(fcinfo);
		s = (AsaDistinctState *) MemoryContextAllocZero(aggctx, sizeof(AsaDistinctState));
		s->base_null = true;
		s->base_trans = base_trans;
		s->flinfo = fcinfo->flinfo;
		s->collation = PG_GET_COLLATION();
		s->type = lookup_type_cache(type, TYPECACHE_CMP_PROC_FINFO);
		if (!OidIsValid(s->type->cmp_proc_finfo.fn_oid))
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION),
							errmsg("could not identify a comparison function for type %s", format_type_be(type))));
	}
	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_POINTER(s);
	value = PG_GETARG_DATUM(2);
	if (s->pending &&
		DatumGetInt32(FunctionCall2Coll(&s->type->cmp_proc_finfo, s->collation, s->value, value)) == 0)
	{
		s->mask |= ASA_GETARG_MASK(1);
		PG_RETURN_POINTER(s);
	}
	asa_distinct_flush(s, fcinfo->context);
	if (!s->type->typbyval && s->value)
		pfree(DatumGetPointer(s->value));
	{
		MemoryContext old = MemoryContextSwitchTo(aggctx);

		s->value = datumCopy(value, s->type->typbyval, s->type->typlen);
		MemoryContextSwitchTo(old);
	}
	s->mask = ASA_GETARG_MASK(1);
	s->query_hash = PG_GETARG_DATUM(3);
	s->correction = PG_GETARG_DATUM(4);
	s->pending = true;
	PG_RETURN_POINTER(s);
}

static Datum
asa_distinct_final(FunctionCallInfo fcinfo, PGFunction base_final)
{
	AsaDistinctState *s = PG_ARGISNULL(0) ? NULL : (AsaDistinctState *) PG_GETARG_POINTER(0);

	if (s)
		asa_distinct_flush(s, fcinfo->context);
	fcinfo->args[0].value = s ? s->base : (Datum) 0;
	fcinfo->args[0].isnull = !s || s->base_null;
	return base_final(fcinfo);
}

#define ASA_DEFINE_DISTINCT_TRANS(NAME, BASE) \
	extern Datum BASE(PG_FUNCTION_ARGS); \
	PG_FUNCTION_INFO_V1(NAME); \
	Datum \
	NAME(PG_FUNCTION_ARGS) \
	{ \
		return asa_distinct_trans(fcinfo, BASE, #NAME); \
	}

#define ASA_DEFINE_DISTINCT_FINAL(NAME, BASE) \
	extern Datum BASE(PG_FUNCTION_ARGS); \
	PG_FUNCTION_INFO_V1(NAME); \
	Datum \
	NAME(PG_FUNCTION_ARGS) \
	{ \
		return asa_distinct_final(fcinfo, BASE); \
	}

ASA_DEFINE_DISTINCT_TRANS(as_count_any_distinct_trans, as_count_any_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_sum_int2_distinct_trans, as_sum_int2_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_sum_int4_distinct_trans, as_sum_int4_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_sum_int8_distinct_trans, as_sum_int8_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_sum_float4_distinct_trans, as_sum_float4_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_sum_float8_distinct_trans, as_sum_float8_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_sum_numeric_distinct_trans, as_sum_numeric_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_sum_numeric_scaled_distinct_trans, as_sum_numeric_scaled_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_avg_int2_distinct_trans, as_avg_int2_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_avg_int4_distinct_trans, as_avg_int4_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_avg_int8_distinct_trans, as_avg_int8_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_avg_float4_distinct_trans, as_avg_float4_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_avg_float8_distinct_trans, as_avg_float8_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_avg_numeric_distinct_trans, as_avg_numeric_qtrans)
ASA_DEFINE_DISTINCT_TRANS(as_avg_numeric_scaled_distinct_trans, as_avg_numeric_scaled_qtrans)

ASA_DEFINE_DISTINCT_FINAL(as_count_distinct_noised_final, as_count_qnoised_final)
ASA_DEFINE_DISTINCT_FINAL(as_count_distinct_counters_final, as_count_final)
ASA_DEFINE_DISTINCT_FINAL(as_sum_int_distinct_noised_final, as_sum_int_qnoised_final)
ASA_DEFINE_DISTINCT_FINAL(as_sum_float_distinct_noised_final, as_sum_float_qnoised_final)
ASA_DEFINE_DISTINCT_FINAL(as_sum_int_distinct_counters_final, as_sum_int_counters_final)
ASA_DEFINE_DISTINCT_FINAL(as_sum_float_distinct_counters_final, as_sum_float_counters_final)
ASA_DEFINE_DISTINCT_FINAL(as_avg_int_distinct_noised_final, as_avg_int_qnoised_final)
ASA_DEFINE_DISTINCT_FINAL(as_avg_float_distinct_noised_final, as_avg_float_qnoised_final)
ASA_DEFINE_DISTINCT_FINAL(as_avg_int_distinct_counters_final, as_avg_int_final)
ASA_DEFINE_DISTINCT_FINAL(as_avg_float_distinct_counters_final, as_avg_float_final)
