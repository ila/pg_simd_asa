/*
 * asa_list.c — re-aggregation of counter arrays and element-wise division.
 *
 * Port of the LIST<FLOAT> aggregate overloads (DuckDB: PacListAggregateState in
 * src/categorical/pac_categorical.cpp) and of priv_div.
 *
 *   as_sum(real[]) / as_count(real[]) / as_min(real[]) / as_max(real[]) -> real[64]
 *     combine per-world counter arrays across rows, e.g. an inner query producing
 *     per-group worlds re-aggregated by an outer query. NULL elements are skipped;
 *     a world that never saw a non-NULL element is NULL in the output, and the
 *     result is NULL when no world saw one.
 *
 *   priv_div(real[], real[]) -> real[]
 *     element-wise num[j] / den[j]; NULL when either is NULL or den[j] = 0.
 */
#include "asa_common.h"

#include <float.h>

#include "catalog/pg_type.h"

typedef enum AsaListOp
{
	ASA_LIST_SUM,
	ASA_LIST_COUNT,
	ASA_LIST_MIN,
	ASA_LIST_MAX
} AsaListOp;

typedef struct AsaListState
{
	uint64		key_hash;		/* bit j set: world j saw a non-NULL element */
	asa_float	values[ASA_LANES];
	uint64		counts[ASA_LANES];
} AsaListState;

static inline asa_float
asa_list_init_value(AsaListOp op)
{
	return op == ASA_LIST_MIN ? FLT_MAX : op == ASA_LIST_MAX ? -FLT_MAX : 0;
}

static AsaListState *
asa_list_create(MemoryContext ctx, AsaListOp op)
{
	AsaListState *s = (AsaListState *) MemoryContextAllocZero(ctx, sizeof(AsaListState));

	for (int j = 0; j < ASA_LANES; j++)
		s->values[j] = asa_list_init_value(op);
	return s;
}

static inline void
asa_list_apply(AsaListState *s, AsaListOp op, int j, asa_float v, uint64 count)
{
	switch (op)
	{
		case ASA_LIST_SUM:
			s->values[j] += v;
			break;
		case ASA_LIST_COUNT:
			break;
		case ASA_LIST_MIN:
			s->values[j] = Min(s->values[j], v);
			break;
		case ASA_LIST_MAX:
			s->values[j] = Max(s->values[j], v);
			break;
	}
	s->counts[j] += count;
}

static Datum
asa_list_trans(FunctionCallInfo fcinfo, AsaListOp op)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_*(real[])");
	AsaListState *s = PG_ARGISNULL(0) ? NULL : (AsaListState *) PG_GETARG_POINTER(0);
	asa_float	vals[ASA_LANES];
	bool		nulls[ASA_LANES];
	int			n;

	if (PG_ARGISNULL(1))
	{
		if (!s)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(s);
	}
	if (!s)
		s = asa_list_create(aggctx, op);
	n = asa_read_array(PG_GETARG_ARRAYTYPE_P(1), vals, nulls);
	for (int j = 0; j < n; j++)
	{
		if (nulls[j])
			continue;
		s->key_hash |= UINT64CONST(1) << j;
		asa_list_apply(s, op, j, vals[j], 1);
	}
	PG_RETURN_POINTER(s);
}

static Datum
asa_list_combine(FunctionCallInfo fcinfo, AsaListOp op)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_*(real[]) combine");
	AsaListState *dst = PG_ARGISNULL(0) ? NULL : (AsaListState *) PG_GETARG_POINTER(0);
	AsaListState *src = PG_ARGISNULL(1) ? NULL : (AsaListState *) PG_GETARG_POINTER(1);

	if (!src)
	{
		if (!dst)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(dst);
	}
	if (!dst)
		dst = asa_list_create(aggctx, op);
	for (int j = 0; j < ASA_LANES; j++)
		if ((src->key_hash >> j) & 1)
			asa_list_apply(dst, op, j, src->values[j], src->counts[j]);
	dst->key_hash |= src->key_hash;
	PG_RETURN_POINTER(dst);
}

static Datum
asa_list_final(FunctionCallInfo fcinfo, AsaListOp op)
{
	AsaListState *s = PG_ARGISNULL(0) ? NULL : (AsaListState *) PG_GETARG_POINTER(0);
	asa_float	out[ASA_LANES];
	bool		nulls[ASA_LANES];

	if (!s || s->key_hash == 0)
		PG_RETURN_NULL();
	for (int j = 0; j < ASA_LANES; j++)
	{
		nulls[j] = !((s->key_hash >> j) & 1);
		out[j] = nulls[j] ? 0 : op == ASA_LIST_COUNT ? (asa_float) s->counts[j] : s->values[j];
	}
	PG_RETURN_ARRAYTYPE_P(asa_make_array_nulls(out, nulls));
}

#define ASA_DEFINE_LIST_AGG(NAME, OP) \
	PG_FUNCTION_INFO_V1(NAME##_trans); \
	Datum NAME##_trans(PG_FUNCTION_ARGS) { return asa_list_trans(fcinfo, OP); } \
	PG_FUNCTION_INFO_V1(NAME##_combine); \
	Datum NAME##_combine(PG_FUNCTION_ARGS) { return asa_list_combine(fcinfo, OP); } \
	PG_FUNCTION_INFO_V1(NAME##_final); \
	Datum NAME##_final(PG_FUNCTION_ARGS) { return asa_list_final(fcinfo, OP); }

ASA_DEFINE_LIST_AGG(as_list_sum, ASA_LIST_SUM)
ASA_DEFINE_LIST_AGG(as_list_count, ASA_LIST_COUNT)
ASA_DEFINE_LIST_AGG(as_list_min, ASA_LIST_MIN)
ASA_DEFINE_LIST_AGG(as_list_max, ASA_LIST_MAX)

PG_FUNCTION_INFO_V1(as_list_serial);
Datum
as_list_serial(PG_FUNCTION_ARGS)
{
	AsaWriter	w;

	asa_writer_init(&w, sizeof(AsaListState));
	asa_write(&w, PG_GETARG_POINTER(0), sizeof(AsaListState));
	PG_RETURN_BYTEA_P(asa_writer_finish(&w));
}

PG_FUNCTION_INFO_V1(as_list_deserial);
Datum
as_list_deserial(PG_FUNCTION_ARGS)
{
	AsaListState *s = (AsaListState *) palloc(sizeof(AsaListState));
	AsaReader	r;

	asa_reader_init(&r, PG_GETARG_BYTEA_PP(0));
	asa_read(&r, s, sizeof(AsaListState));
	PG_RETURN_POINTER(s);
}

PG_FUNCTION_INFO_V1(asa_priv_div);
Datum
asa_priv_div(PG_FUNCTION_ARGS)
{
	asa_float	num[ASA_LANES], den[ASA_LANES], out[ASA_LANES];
	bool		num_null[ASA_LANES], den_null[ASA_LANES], nulls[ASA_LANES];
	int			n_num = asa_read_array(PG_GETARG_ARRAYTYPE_P(0), num, num_null);
	int			n_den = asa_read_array(PG_GETARG_ARRAYTYPE_P(1), den, den_null);
	int			n = Min(n_num, n_den);
	Datum		datums[ASA_LANES];
	int			dims[1];
	int			lbs[1] = {1};

	if (n == ASA_LANES)
	{
		for (int j = 0; j < n; j++)
		{
			nulls[j] = num_null[j] || den_null[j] || den[j] == 0;
			out[j] = nulls[j] ? 0 : num[j] / den[j];
		}
		PG_RETURN_ARRAYTYPE_P(asa_make_array_nulls(out, nulls));
	}
	/* shorter inputs: general path */
	for (int j = 0; j < n; j++)
	{
		nulls[j] = num_null[j] || den_null[j] || den[j] == 0;
		datums[j] = Float4GetDatum(nulls[j] ? 0 : num[j] / den[j]);
	}
	dims[0] = n;
	PG_RETURN_ARRAYTYPE_P(construct_md_array(datums, nulls, 1, dims, lbs, FLOAT4OID, sizeof(float4), true,
											 TYPALIGN_INT));
}
