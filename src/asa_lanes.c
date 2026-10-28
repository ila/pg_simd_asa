/*
 * asa_lanes.c — per-world (lane-wise) arithmetic and comparisons on counter arrays.
 *
 *   asa_lane_add / sub / mul / div(a, b) -> real[]   a, b: real[] or double precision
 *   asa_lane_neg(real[]) -> real[]
 *   asa_lane_lt / le / gt / ge / eq / ne(a, b) -> bigint (membership mask)
 *
 * A double precision argument is broadcast to every world. Arithmetic propagates NULL
 * worlds; division by zero yields NULL (as priv_div). A comparison returns the mask of the
 * worlds where it holds (NULL worlds: false), so a predicate over worlds can be folded into
 * the membership mask of the rows it filters — the composition the rewrite uses for
 * correlated subqueries such as TPC-H Q17. These are the PostgreSQL counterparts of the
 * list_transform lambdas the DuckDB categorical rewriter generates.
 */
#include "asa_common.h"

#include "catalog/pg_type.h"

typedef struct AsaLanes
{
	double		vals[ASA_LANES];
	bool		nulls[ASA_LANES];
	int			len;			/* -1: broadcast scalar */
} AsaLanes;

static void
asa_lanes_arg(FunctionCallInfo fcinfo, int argno, AsaLanes *lanes)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, argno);

	if (type == FLOAT8OID)
	{
		double		v = PG_GETARG_FLOAT8(argno);

		for (int j = 0; j < ASA_LANES; j++)
		{
			lanes->vals[j] = v;
			lanes->nulls[j] = false;
		}
		lanes->len = -1;
	}
	else
	{
		asa_float	vals[ASA_LANES];

		lanes->len = asa_read_array(PG_GETARG_ARRAYTYPE_P(argno), vals, lanes->nulls);
		for (int j = 0; j < lanes->len; j++)
			lanes->vals[j] = vals[j];
	}
}

/* Number of worlds of a binary operation: the shorter array (scalars broadcast). */
static int
asa_lanes_len(const AsaLanes *a, const AsaLanes *b)
{
	if (a->len < 0 && b->len < 0)
		elog(ERROR, "SIMD-ASA: lane-wise operation needs at least one real[] argument");
	return a->len < 0 ? b->len : b->len < 0 ? a->len : Min(a->len, b->len);
}

static ArrayType *
asa_lanes_result(const double *vals, const bool *nulls, int n)
{
	Datum		datums[ASA_LANES];
	asa_float	out[ASA_LANES];
	int			dims[1] = {n};
	int			lbs[1] = {1};

	for (int j = 0; j < n; j++)
		out[j] = (asa_float) vals[j];
	if (n == ASA_LANES)
		return asa_make_array_nulls(out, nulls);
	for (int j = 0; j < n; j++)
		datums[j] = Float4GetDatum(out[j]);
	return construct_md_array(datums, (bool *) nulls, 1, dims, lbs, FLOAT4OID, sizeof(float4), true, TYPALIGN_INT);
}

#define ASA_DEFINE_LANE_ARITH(NAME, OP_NULL, OP) \
	PG_FUNCTION_INFO_V1(NAME); \
	Datum \
	NAME(PG_FUNCTION_ARGS) \
	{ \
		AsaLanes	a, b; \
		double		out[ASA_LANES]; \
		bool		nulls[ASA_LANES]; \
		int			n; \
		asa_lanes_arg(fcinfo, 0, &a); \
		asa_lanes_arg(fcinfo, 1, &b); \
		n = asa_lanes_len(&a, &b); \
		for (int j = 0; j < n; j++) \
		{ \
			double		x = a.vals[j], y = b.vals[j]; \
			nulls[j] = a.nulls[j] || b.nulls[j] || (OP_NULL); \
			out[j] = nulls[j] ? 0 : (OP); \
		} \
		PG_RETURN_ARRAYTYPE_P(asa_lanes_result(out, nulls, n)); \
	}

ASA_DEFINE_LANE_ARITH(asa_lane_add, false, x + y)
ASA_DEFINE_LANE_ARITH(asa_lane_sub, false, x - y)
ASA_DEFINE_LANE_ARITH(asa_lane_mul, false, x * y)
ASA_DEFINE_LANE_ARITH(asa_lane_div, y == 0, x / y)

PG_FUNCTION_INFO_V1(asa_lane_neg);
Datum
asa_lane_neg(PG_FUNCTION_ARGS)
{
	AsaLanes	a;
	double		out[ASA_LANES];

	asa_lanes_arg(fcinfo, 0, &a);
	for (int j = 0; j < a.len; j++)
		out[j] = -a.vals[j];
	PG_RETURN_ARRAYTYPE_P(asa_lanes_result(out, a.nulls, a.len));
}

#define ASA_DEFINE_LANE_CMP(NAME, OP) \
	PG_FUNCTION_INFO_V1(NAME); \
	Datum \
	NAME(PG_FUNCTION_ARGS) \
	{ \
		AsaLanes	a, b; \
		uint64		mask = 0; \
		int			n; \
		asa_lanes_arg(fcinfo, 0, &a); \
		asa_lanes_arg(fcinfo, 1, &b); \
		n = asa_lanes_len(&a, &b); \
		for (int j = 0; j < n; j++) \
			if (!a.nulls[j] && !b.nulls[j] && a.vals[j] OP b.vals[j]) \
				mask |= UINT64CONST(1) << j; \
		PG_RETURN_INT64((int64) mask); \
	}

ASA_DEFINE_LANE_CMP(asa_lane_lt, <)
ASA_DEFINE_LANE_CMP(asa_lane_le, <=)
ASA_DEFINE_LANE_CMP(asa_lane_gt, >)
ASA_DEFINE_LANE_CMP(asa_lane_ge, >=)
ASA_DEFINE_LANE_CMP(asa_lane_eq, ==)
ASA_DEFINE_LANE_CMP(asa_lane_ne, !=)
