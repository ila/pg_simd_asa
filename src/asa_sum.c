/*
 * asa_sum.c — as_sum(mask, value) and as_avg(mask, value) -> real[64]
 *
 * Port of PacSumIntState / PacSumDoubleState (DuckDB: src/include/aggregates/as_sum.hpp,
 * src/aggregates/as_sum.cpp), default (approximate, double-sided) configuration.
 *
 * Integers (smallint, integer, bigint): instead of 64 conditional 64-bit additions,
 * a value is routed by magnitude into one of 25 lazily allocated levels of 16-bit
 * counters, staggered by 4 bits (112 bits covered). Each level is 16 uint64 words,
 * each holding four packed uint16 world counters (SWAR), so one update is 16
 * branch-free word additions of (value * 0x0001000100010001) masked by the expanded
 * membership bits. The value is truncated to its level (keeps 9-12 significant
 * bits: at most 2^-8 relative error per value, downward); this is the only
 * approximation, since cascading is lossless: when a level would overflow, the
 * upper 12 bits of every counter move to the next level and the lower 4 stay
 * behind. exact_total[level] bounds every world counter at that level, so the
 * overflow check is a single scalar comparison. Negative values are negated into
 * a second, lazily allocated unsigned state and subtracted at finalization
 * (numerically stabler, and one more bit of counter precision).
 *
 * numeric, as DuckDB's DECIMAL: the integer kernel on value * 10^scale, the scale derived
 * from the argument expression (asa_numeric_static_scale). Floating point (real, double
 * precision): predicated additions into float[64] (value * bit, branch-free).
 *
 * Output lane j is 2 * sum_j (50% inclusion compensation) and 0 for worlds no row
 * was assigned to, as in DuckDB. as_avg lane j is sum_j / count_j (NULL for empty
 * worlds), i.e. what DuckDB computes for priv_div(as_sum(m, v), as_count(m, v)).
 * as_noised_sum releases one noised sum from the same state; as_noised_avg releases a
 * noised sum and a noised count and divides them, as DuckDB's AVG rewrite does.
 *
 * Deviations from the DuckDB code (bugs there, fixed here): a world that only
 * received negative integers is reported (DuckDB took the world mask from the
 * positive state only), and level routing of magnitudes >= 2^63 uses unsigned
 * arithmetic (DuckDB routed them to level 0).
 */
#include "asa_common.h"
#include "asa_count.h"
#include "asa_noise.h"

#include <math.h>

#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "utils/fmgroids.h"
#include "nodes/nodeFuncs.h"

#define ASA_SUM_NUM_LEVELS 25
#define ASA_SUM_SWAR_ELEMENTS 16	/* uint64 words per level: 4 x uint16 each */
#define ASA_SUM_LEVEL_SHIFT 4
#define ASA_SUM_SWAR_MASK UINT64CONST(0x0001000100010001)
#define ASA_SUM_LEVEL_MASK ((1 << ASA_SUM_LEVEL_SHIFT) - 1)
#define ASA_SUM_REMAINDER_MASK ((uint64) ASA_SUM_LEVEL_MASK * ASA_SUM_SWAR_MASK)
#define ASA_SUM_OVERFLOW 65535
#define ASA_SUM_INLINE_LEVELS 9 /* levels below this may use the inline storage */
#define ASA_NUMERIC_MAX_SCALE 9

static const double asa_pow10[ASA_NUMERIC_MAX_SCALE + 1] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9};

/* Unsigned approximate integer sum state (PacSumIntState<false>). */
typedef struct AsaSumIntState
{
	uint64		key_hash;		/* OR of all masks seen */
	uint64		update_count;	/* non-NULL updates with a non-zero mask */
	int8		max_level_used; /* highest allocated level (-1 if none) */
	int8		inline_level_idx;	/* level stored inline (-1 if none) */
	int32		exact_total[ASA_SUM_NUM_LEVELS];	/* upper bound on every world counter */

	/*
	 * 25 level pointers take 200 bytes that are mostly empty: while fewer than 9
	 * levels are used, the back 128 bytes hold the first allocated level.
	 */
	union
	{
		uint64	   *levels[ASA_SUM_NUM_LEVELS];
		struct
		{
			void	   *dummy[ASA_SUM_INLINE_LEVELS];
			uint64		inline_level[ASA_SUM_SWAR_ELEMENTS];
		}			inl;
	}			u;
} AsaSumIntState;

/* Double-sided integer sum: positive values in pos, negated negative values in neg. */
typedef struct AsaSumIntPair
{
	AsaSumIntState pos;
	AsaSumIntState *neg;
	int32		scale;			/* numeric: values are summed as integers times 10^scale */
	AsaReleaseParams rel;		/* asa_* forms */
} AsaSumIntPair;

typedef struct AsaSumFloatState
{
	uint64		key_hash;
	uint64		update_count;
	AsaReleaseParams rel;		/* asa_* forms */
	float		totals[ASA_LANES];	/* approximate double with float, as in DuckDB */
} AsaSumFloatState;

typedef struct AsaAvgIntState
{
	AsaSumIntPair sum;
	AsaCountState count;
} AsaAvgIntState;

typedef struct AsaAvgFloatState
{
	AsaSumFloatState sum;
	AsaCountState count;
} AsaAvgFloatState;

/* ---------------------------------------------------------------------------
 * Integer kernel
 * ------------------------------------------------------------------------- */

static void asa_sum_cascade(AsaSumIntState *s, int k, MemoryContext ctx);

static inline void
asa_sum_int_init(AsaSumIntState *s)
{
	memset(s, 0, sizeof(*s));
	s->max_level_used = -1;
	s->inline_level_idx = -1;
}

/* Level whose 16-bit counters receive abs_val >> (4 * level): keeps the top 9-12 bits. */
static inline int
asa_sum_get_level(uint64 abs_val)
{
	int			level = ((63 - asa_clzll(1 | abs_val)) - 8) >> 2;

	return (abs_val < 4096) ? 0 : level;
}

static void
asa_sum_allocate_level(AsaSumIntState *s, MemoryContext ctx, int k)
{
	const Size	level_size = ASA_SUM_SWAR_ELEMENTS * sizeof(uint64);

	if (k >= ASA_SUM_INLINE_LEVELS && s->inline_level_idx >= 0)
	{
		/* the inline area overlaps levels[9..]: move the inline level out first */
		uint64	   *ext = (uint64 *) MemoryContextAlloc(ctx, level_size);

		memcpy(ext, s->u.inl.inline_level, level_size);
		s->u.levels[s->inline_level_idx] = ext;
		s->inline_level_idx = -1;
		memset(s->u.inl.inline_level, 0, level_size);	/* levels[9..24] read as NULL */
	}
	if (k < ASA_SUM_INLINE_LEVELS && s->inline_level_idx < 0)
	{
		s->u.levels[k] = s->u.inl.inline_level;
		memset(s->u.inl.inline_level, 0, level_size);
		s->inline_level_idx = (int8) k;
	}
	else
		s->u.levels[k] = (uint64 *) MemoryContextAllocZero(ctx, level_size);
}

/* Ensures levels max_level_used+1 .. k are allocated. */
static inline void
asa_sum_ensure_level(AsaSumIntState *s, MemoryContext ctx, int k)
{
	if (likely(k <= s->max_level_used))
		return;
	for (int i = s->max_level_used + 1; i <= k; i++)
		asa_sum_allocate_level(s, ctx, i);
	s->max_level_used = (int8) k;
}

/* Adds amount to the level's bound, cascading the level first if it would overflow. */
static inline void
asa_sum_add_exact(AsaSumIntState *s, int level, int32 amount, MemoryContext ctx)
{
	int32		new_total;

	asa_sum_ensure_level(s, ctx, level);
	new_total = s->exact_total[level] + amount;
	if (unlikely(new_total > ASA_SUM_OVERFLOW))
	{
		asa_sum_cascade(s, level, ctx);
		s->exact_total[level] += amount;	/* cascade leaves the remainder bound behind */
	}
	else
		s->exact_total[level] = new_total;
}

/*
 * Lossless cascade of level k into k+1: x == (x >> 4) * 16 + (x & 15). Afterwards
 * every counter at level k is in [0, 15].
 */
static void
asa_sum_cascade(AsaSumIntState *s, int k, MemoryContext ctx)
{
	uint16	   *src;
	uint16	   *dst;

	if (k >= ASA_SUM_NUM_LEVELS - 1 || !s->u.levels[k])
		return;
	asa_sum_add_exact(s, k + 1, s->exact_total[k] >> ASA_SUM_LEVEL_SHIFT, ctx);

	/* read the pointers only now: allocating level k+1 may move the inline level */
	src = (uint16 *) s->u.levels[k];
	dst = (uint16 *) s->u.levels[k + 1];
	for (int j = 0; j < ASA_LANES; j++)
		dst[j] += (uint16) (src[j] >> ASA_SUM_LEVEL_SHIFT);
	for (int i = 0; i < ASA_SUM_SWAR_ELEMENTS; i++)
		s->u.levels[k][i] &= ASA_SUM_REMAINDER_MASK;
	s->exact_total[k] = ASA_SUM_LEVEL_MASK;
}

/* SWAR: adds value (< 2^16) to the 16-bit counter of every world whose mask bit is set. */
static ASA_AUTOVECTORIZE inline void
asa_sum_add_swar16(uint64 *ASA_RESTRICT total, uint64 value, uint64 mask)
{
	uint64		packed = (uint64) (uint16) value * ASA_SUM_SWAR_MASK;

	for (int i = 0; i < ASA_SUM_SWAR_ELEMENTS; i++)
	{
		uint64		bits = (mask >> i) & ASA_SUM_SWAR_MASK;
		uint64		expanded = (bits << 16) - bits; /* 0x0001 -> 0xFFFF per lane */

		total[i] += packed & expanded;
	}
}

static ASA_AUTOVECTORIZE inline void
asa_sum_int_update(AsaSumIntState *s, uint64 mask, uint64 value, MemoryContext ctx)
{
	int			level = asa_sum_get_level(value);
	uint64		shifted = value >> (level << 2);

	s->key_hash |= mask;
	s->update_count++;
	asa_sum_add_exact(s, level, (int32) shifted, ctx);
	asa_sum_add_swar16(s->u.levels[level], shifted, mask);
}

/* Adds sign * world totals (sum of all levels, each scaled by 2^(4*level)) to dst. */
static void
asa_sum_int_get_totals(const AsaSumIntState *s, double sign, double *dst)
{
	for (int k = 0; k <= s->max_level_used; k++)
	{
		const uint16 *counters = (const uint16 *) s->u.levels[k];
		double		scale = sign * ldexp(1.0, ASA_SUM_LEVEL_SHIFT * k);

		if (!counters)
			continue;
		for (int j = 0; j < ASA_LANES; j++)
			dst[j] += (double) counters[(j % 16) * 4 + j / 16] * scale;
	}
}

/* Adds src into dst level by level; src may be modified (it is consumed). */
static void
asa_sum_int_combine(AsaSumIntState *dst, AsaSumIntState *src, MemoryContext ctx)
{
	dst->key_hash |= src->key_hash;
	dst->update_count += src->update_count;
	for (int k = 0; k <= src->max_level_used; k++)
	{
		int32		new_total;

		if (!src->u.levels[k])
			continue;
		asa_sum_ensure_level(dst, ctx, k);
		new_total = dst->exact_total[k] + src->exact_total[k];
		if (new_total > ASA_SUM_OVERFLOW)
		{
			asa_sum_cascade(dst, k, ctx);	/* dst counters now <= 15 */
			if (src->exact_total[k] > ASA_SUM_OVERFLOW - ASA_SUM_LEVEL_MASK)
				asa_sum_cascade(src, k, ctx);	/* src level k+1 grows; merged next */
			dst->exact_total[k] += src->exact_total[k];
		}
		else
			dst->exact_total[k] = new_total;
		for (int j = 0; j < ASA_SUM_SWAR_ELEMENTS; j++)
			dst->u.levels[k][j] += src->u.levels[k][j];
	}
}

static Size
asa_sum_int_serial_size(const AsaSumIntState *s)
{
	return 2 * sizeof(uint64) + sizeof(int32) +
		(s->max_level_used + 1) * (sizeof(int32) + ASA_SUM_SWAR_ELEMENTS * sizeof(uint64));
}

/* key_hash, update_count, max_level_used, then per level: exact_total + 16 words (0s if absent). */
static void
asa_sum_int_write(AsaWriter *w, const AsaSumIntState *s)
{
	static const uint64 zeros[ASA_SUM_SWAR_ELEMENTS] = {0};
	int32		max_level = s->max_level_used;

	asa_write(w, &s->key_hash, sizeof(uint64));
	asa_write(w, &s->update_count, sizeof(uint64));
	asa_write(w, &max_level, sizeof(int32));
	for (int k = 0; k <= max_level; k++)
	{
		asa_write(w, &s->exact_total[k], sizeof(int32));
		asa_write(w, s->u.levels[k] ? s->u.levels[k] : zeros, sizeof(zeros));
	}
}

static void
asa_sum_int_read(AsaReader *r, AsaSumIntState *s)
{
	int32		max_level;

	asa_sum_int_init(s);
	asa_read(r, &s->key_hash, sizeof(uint64));
	asa_read(r, &s->update_count, sizeof(uint64));
	asa_read(r, &max_level, sizeof(int32));
	if (max_level >= ASA_SUM_NUM_LEVELS)
		elog(ERROR, "SIMD-ASA: corrupt serialized sum state");
	asa_sum_ensure_level(s, CurrentMemoryContext, max_level);
	for (int k = 0; k <= max_level; k++)
	{
		asa_read(r, &s->exact_total[k], sizeof(int32));
		asa_read(r, s->u.levels[k], ASA_SUM_SWAR_ELEMENTS * sizeof(uint64));
	}
}

/* ---------------------------------------------------------------------------
 * Double-sided integer pair
 * ------------------------------------------------------------------------- */

static inline void
asa_sum_pair_init(AsaSumIntPair *p)
{
	asa_sum_int_init(&p->pos);
	p->neg = NULL;
	p->scale = 0;
	p->rel.query_hash = 0;
	p->rel.correction = 0;
}

static inline AsaSumIntState *
asa_sum_pair_ensure_neg(AsaSumIntPair *p, MemoryContext ctx)
{
	if (!p->neg)
	{
		p->neg = (AsaSumIntState *) MemoryContextAlloc(ctx, sizeof(AsaSumIntState));
		asa_sum_int_init(p->neg);
	}
	return p->neg;
}

static inline void
asa_sum_pair_update(AsaSumIntPair *p, uint64 mask, int64 value, MemoryContext ctx)
{
	if (unlikely(!mask))
		return;					/* skip masks in no world (helps stability if frequent) */
	if (value < 0)
		asa_sum_int_update(asa_sum_pair_ensure_neg(p, ctx), mask, (uint64) 0 - (uint64) value, ctx);
	else
		asa_sum_int_update(&p->pos, mask, (uint64) value, ctx);
}

/* World mask and signed world totals. */
static uint64
asa_sum_pair_totals(const AsaSumIntPair *p, double *totals)
{
	uint64		key_hash = p->pos.key_hash;

	asa_sum_int_get_totals(&p->pos, 1.0, totals);
	if (p->neg)
	{
		asa_sum_int_get_totals(p->neg, -1.0, totals);
		key_hash |= p->neg->key_hash;
	}
	if (p->scale)
		for (int j = 0; j < ASA_LANES; j++)
			totals[j] /= asa_pow10[p->scale];
	return key_hash;
}

static void
asa_sum_pair_combine(AsaSumIntPair *dst, AsaSumIntPair *src, MemoryContext ctx)
{
	if (dst->scale != src->scale)
	{
		if (dst->pos.max_level_used >= 0 || dst->neg)
			elog(ERROR, "SIMD-ASA: cannot combine numeric sums of scale %d and %d", dst->scale, src->scale);
		dst->scale = src->scale;	/* dst is still empty */
	}
	asa_release_params_merge(&dst->rel, &src->rel);
	asa_sum_int_combine(&dst->pos, &src->pos, ctx);
	if (src->neg)
		asa_sum_int_combine(asa_sum_pair_ensure_neg(dst, ctx), src->neg, ctx);
}

static Size
asa_sum_pair_serial_size(const AsaSumIntPair *p)
{
	return asa_sum_int_serial_size(&p->pos) + sizeof(bool) + sizeof(int32) + sizeof(AsaReleaseParams) +
		(p->neg ? asa_sum_int_serial_size(p->neg) : 0);
}

static void
asa_sum_pair_write(AsaWriter *w, const AsaSumIntPair *p)
{
	bool		has_neg = p->neg != NULL;

	asa_sum_int_write(w, &p->pos);
	asa_write(w, &p->scale, sizeof(int32));
	asa_write(w, &p->rel, sizeof(AsaReleaseParams));
	asa_write(w, &has_neg, sizeof(bool));
	if (has_neg)
		asa_sum_int_write(w, p->neg);
}

static void
asa_sum_pair_read(AsaReader *r, AsaSumIntPair *p)
{
	bool		has_neg;

	asa_sum_pair_init(p);
	asa_sum_int_read(r, &p->pos);
	asa_read(r, &p->scale, sizeof(int32));
	asa_read(r, &p->rel, sizeof(AsaReleaseParams));
	asa_read(r, &has_neg, sizeof(bool));
	if (has_neg)
		asa_sum_int_read(r, asa_sum_pair_ensure_neg(p, CurrentMemoryContext));
}

/* ---------------------------------------------------------------------------
 * Floating-point kernel
 * ------------------------------------------------------------------------- */

static ASA_AUTOVECTORIZE inline void
asa_sum_float_update(AsaSumFloatState *s, uint64 mask, double value)
{
	float		v = (float) value;

	s->key_hash |= mask;
	if (likely(mask))
		s->update_count++;
	for (int j = 0; j < ASA_LANES; j++)
		s->totals[j] += v * (float) ((mask >> j) & 1); /* multiply by 0 or 1: no branch */
}

static void
asa_sum_float_combine(AsaSumFloatState *dst, const AsaSumFloatState *src)
{
	dst->key_hash |= src->key_hash;
	dst->update_count += src->update_count;
	asa_release_params_merge(&dst->rel, &src->rel);
	for (int j = 0; j < ASA_LANES; j++)
		dst->totals[j] += src->totals[j];
}

/* numeric inputs of min/max and of the 0.1 sum/avg take the floating-point path */
static inline double
asa_numeric_to_double(Datum d)
{
	return DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow, d));
}

/*
 * Decimal scale of a numeric expression, by the rules of DuckDB's DECIMAL types (which are
 * also those of numeric display scales): a column's declared scale, a constant's own, the
 * sum of the scales for *, the larger one for + - CASE COALESCE, 0 for integers. -1 if
 * unknown.
 */
static int
asa_numeric_static_scale(Node *node)
{
	Oid			type;
	int32		typmod;
	int			a;
	int			b;
	ListCell   *lc;

	if (node == NULL)
		return -1;
	type = exprType(node);
	if (type == INT2OID || type == INT4OID || type == INT8OID)
		return 0;
	if (type != NUMERICOID)
		return -1;
	typmod = exprTypmod(node);
	if (typmod >= (int32) VARHDRSZ)
		return Max(((((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024), 0);	/* sign-extended 11-bit scale */
	switch (nodeTag(node))
	{
		case T_Const:
			return ((Const *) node)->constisnull ? 0 :
				DatumGetInt32(DirectFunctionCall1(numeric_scale, ((Const *) node)->constvalue));
		case T_RelabelType:
			return asa_numeric_static_scale((Node *) ((RelabelType *) node)->arg);
		case T_FuncExpr:
			if (((FuncExpr *) node)->funcformat != COERCE_EXPLICIT_CALL && ((FuncExpr *) node)->args)
				return asa_numeric_static_scale(linitial(((FuncExpr *) node)->args));	/* a cast */
			return -1;
		case T_OpExpr:
			set_opfuncid((OpExpr *) node);
			a = asa_numeric_static_scale(linitial(((OpExpr *) node)->args));
			b = list_length(((OpExpr *) node)->args) > 1 ? asa_numeric_static_scale(lsecond(((OpExpr *) node)->args)) : 0;
			if (a < 0 || b < 0)
				return -1;
			switch (((OpExpr *) node)->opfuncid)
			{
				case F_NUMERIC_ADD:
				case F_NUMERIC_SUB:
					return Max(a, b);
				case F_NUMERIC_MUL:
					return a + b;
				case F_NUMERIC_UMINUS:
					return a;
				default:
					return -1;
			}
		case T_CaseExpr:
			a = asa_numeric_static_scale((Node *) ((CaseExpr *) node)->defresult);
			foreach(lc, ((CaseExpr *) node)->args)
			{
				b = asa_numeric_static_scale((Node *) ((CaseWhen *) lfirst(lc))->result);
				a = (a < 0 || b < 0) ? -1 : Max(a, b);
			}
			return a;
		case T_CoalesceExpr:
			a = 0;
			foreach(lc, ((CoalesceExpr *) node)->args)
			{
				b = asa_numeric_static_scale(lfirst(lc));
				a = (a < 0 || b < 0) ? -1 : Max(a, b);
			}
			return a;
		default:
			return -1;
	}
}

/*
 * Scale of a numeric sum: static from the aggregate's value argument (so that parallel
 * workers agree), else the first value's display scale; at most ASA_NUMERIC_MAX_SCALE.
 */
static int32
asa_numeric_sum_scale(FunctionCallInfo fcinfo, Datum first)
{
	Aggref	   *aggref = AggGetAggref(fcinfo);
	int			scale = -1;

	if (aggref && list_length(aggref->args) >= 2)
		scale = asa_numeric_static_scale((Node *) lsecond_node(TargetEntry, aggref->args)->expr);
	if (scale < 0)
		scale = DatumGetInt32(DirectFunctionCall1(numeric_scale, first));
	return Min(scale, ASA_NUMERIC_MAX_SCALE);
}

/* A numeric value as an integer times 10^scale (exact up to 15 significant digits). */
static inline int64
asa_numeric_scaled(Datum d, int32 scale)
{
	double		v = asa_numeric_to_double(d) * asa_pow10[scale];

	if (!(fabs(v) < 9.2e18))
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						errmsg("SIMD-ASA: numeric value out of range for a sum at scale %d", scale),
						errhint("Cast the value to double precision.")));
	return (int64) llround(v);
}

/* ---------------------------------------------------------------------------
 * Finalization
 * ------------------------------------------------------------------------- */

/* Lane j: 2 * total_j for worlds in key_hash, 0 otherwise. */
static void
asa_sum_output(uint64 key_hash, const double *totals, asa_float *out)
{
	for (int j = 0; j < ASA_LANES; j++)
		out[j] = ((key_hash >> j) & 1) ? (asa_float) (totals[j] * 2.0) : 0;
}

/* Lane j: sum_j / count_j, NULL for empty worlds (priv_div semantics on 2x-scaled lanes). */
static ArrayType *
asa_avg_output(uint64 key_hash, const double *totals, const AsaCountState *count)
{
	asa_float	sums[ASA_LANES];
	asa_float	counts[ASA_LANES];
	asa_float	out[ASA_LANES];
	bool		nulls[ASA_LANES];

	asa_sum_output(key_hash, totals, sums);
	asa_count_finalize(count, counts);
	for (int j = 0; j < ASA_LANES; j++)
	{
		nulls[j] = counts[j] == 0;
		out[j] = nulls[j] ? 0 : sums[j] / counts[j];
	}
	return asa_make_array_nulls(out, nulls);
}

/* ---------------------------------------------------------------------------
 * SQL-callable: as_sum
 * ------------------------------------------------------------------------- */

#define ASA_STATE_OR_NULL(T, n) (PG_ARGISNULL(n) ? NULL : (T *) PG_GETARG_POINTER(n))

/* Returns the current state unchanged when the row is skipped (NULL mask or value). */
#define ASA_SKIP_NULL_INPUT(s) \
	do { \
		if (PG_ARGISNULL(1) || PG_ARGISNULL(2)) \
		{ \
			if (!(s)) \
				PG_RETURN_NULL(); \
			PG_RETURN_POINTER(s); \
		} \
	} while (0)

static Datum
asa_sum_int_trans(FunctionCallInfo fcinfo, int64 value)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_sum");
	AsaSumIntPair *p = ASA_STATE_OR_NULL(AsaSumIntPair, 0);

	if (!p)
	{
		p = (AsaSumIntPair *) MemoryContextAlloc(aggctx, sizeof(AsaSumIntPair));
		asa_sum_pair_init(p);
	}
	asa_sum_pair_update(p, ASA_GETARG_MASK(1), value, aggctx);
	PG_RETURN_POINTER(p);
}

static Datum
asa_sum_float_trans(FunctionCallInfo fcinfo, double value)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_sum");
	AsaSumFloatState *s = ASA_STATE_OR_NULL(AsaSumFloatState, 0);

	if (!s)
		s = (AsaSumFloatState *) MemoryContextAllocZero(aggctx, sizeof(AsaSumFloatState));
	asa_sum_float_update(s, ASA_GETARG_MASK(1), value);
	PG_RETURN_POINTER(s);
}

#define ASA_DEFINE_SUM_TRANS(NAME, STATE, KERNEL, GETVAL) \
	PG_FUNCTION_INFO_V1(NAME); \
	Datum \
	NAME(PG_FUNCTION_ARGS) \
	{ \
		ASA_SKIP_NULL_INPUT(ASA_STATE_OR_NULL(STATE, 0)); \
		return KERNEL(fcinfo, GETVAL); \
	}

ASA_DEFINE_SUM_TRANS(as_sum_int2_trans, AsaSumIntPair, asa_sum_int_trans, (int64) PG_GETARG_INT16(2))
ASA_DEFINE_SUM_TRANS(as_sum_int4_trans, AsaSumIntPair, asa_sum_int_trans, (int64) PG_GETARG_INT32(2))
ASA_DEFINE_SUM_TRANS(as_sum_int8_trans, AsaSumIntPair, asa_sum_int_trans, PG_GETARG_INT64(2))
ASA_DEFINE_SUM_TRANS(as_sum_float4_trans, AsaSumFloatState, asa_sum_float_trans, (double) PG_GETARG_FLOAT4(2))
ASA_DEFINE_SUM_TRANS(as_sum_float8_trans, AsaSumFloatState, asa_sum_float_trans, PG_GETARG_FLOAT8(2))
ASA_DEFINE_SUM_TRANS(as_sum_numeric_trans, AsaSumFloatState, asa_sum_float_trans,
					 asa_numeric_to_double(PG_GETARG_DATUM(2)))

/* as_sum(mask, numeric): DECIMAL-style, the integer kernel on values times 10^scale. */
PG_FUNCTION_INFO_V1(as_sum_numeric_scaled_trans);
Datum
as_sum_numeric_scaled_trans(PG_FUNCTION_ARGS)
{
	MemoryContext aggctx;
	AsaSumIntPair *p = ASA_STATE_OR_NULL(AsaSumIntPair, 0);

	ASA_SKIP_NULL_INPUT(p);
	aggctx = asa_agg_context(fcinfo, "as_sum");
	if (!p)
	{
		p = (AsaSumIntPair *) MemoryContextAlloc(aggctx, sizeof(AsaSumIntPair));
		asa_sum_pair_init(p);
		p->scale = asa_numeric_sum_scale(fcinfo, PG_GETARG_DATUM(2));
	}
	asa_sum_pair_update(p, ASA_GETARG_MASK(1), asa_numeric_scaled(PG_GETARG_DATUM(2), p->scale), aggctx);
	PG_RETURN_POINTER(p);
}

PG_FUNCTION_INFO_V1(as_sum_int_final);
Datum
as_sum_int_final(PG_FUNCTION_ARGS)
{
	AsaSumIntPair *p = ASA_STATE_OR_NULL(AsaSumIntPair, 0);
	double		totals[ASA_LANES] = {0};
	asa_float	out[ASA_LANES];

	asa_sum_output(p ? asa_sum_pair_totals(p, totals) : 0, totals, out);
	asa_correct_counters(out, p ? &p->rel : NULL);
	PG_RETURN_ARRAYTYPE_P(asa_make_array(out));
}

PG_FUNCTION_INFO_V1(as_sum_float_final);
Datum
as_sum_float_final(PG_FUNCTION_ARGS)
{
	AsaSumFloatState *s = ASA_STATE_OR_NULL(AsaSumFloatState, 0);
	double		totals[ASA_LANES] = {0};
	asa_float	out[ASA_LANES];

	if (s)
		for (int j = 0; j < ASA_LANES; j++)
			totals[j] = s->totals[j];
	asa_sum_output(s ? s->key_hash : 0, totals, out);
	asa_correct_counters(out, s ? &s->rel : NULL);
	PG_RETURN_ARRAYTYPE_P(asa_make_array(out));
}

/*
 * asa_counters_sum: as as_sum, but NULL without input rows (as sum, and as a correlated
 * sub-select in DuckDB, which decorrelates it into a join).
 */
PG_FUNCTION_INFO_V1(as_sum_int_counters_final);
Datum
as_sum_int_counters_final(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	return as_sum_int_final(fcinfo);
}

PG_FUNCTION_INFO_V1(as_sum_float_counters_final);
Datum
as_sum_float_counters_final(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	return as_sum_float_final(fcinfo);
}

/* World counters, mask and row count of an integer sum state. */
static uint64
asa_sum_int_counters(const AsaSumIntPair *p, asa_float *out, uint64 *update_count)
{
	double		totals[ASA_LANES] = {0};
	uint64		key_hash = p ? asa_sum_pair_totals(p, totals) : 0;

	asa_sum_output(key_hash, totals, out);
	*update_count = p ? p->pos.update_count + (p->neg ? p->neg->update_count : 0) : 0;
	return key_hash;
}

static uint64
asa_sum_float_counters(const AsaSumFloatState *s, asa_float *out, uint64 *update_count)
{
	double		totals[ASA_LANES] = {0};

	if (s)
		for (int j = 0; j < ASA_LANES; j++)
			totals[j] = s->totals[j];
	asa_sum_output(s ? s->key_hash : 0, totals, out);
	*update_count = s ? s->update_count : 0;
	return s ? s->key_hash : 0;
}

static Datum
asa_sum_noised_release(FunctionCallInfo fcinfo, const asa_float *counters, uint64 key_hash, uint64 update_count,
					   AsaReleaseParams rel)
{
	double		value;

	if (!asa_release(fcinfo, 0, counters, key_hash, update_count, &rel, true, "as_noised_sum", &value))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(asa_release_datum(fcinfo, value));
}

#define ASA_DEFINE_SUM_NOISED_FINAL(NAME, STATE, COUNTERS, FROM_ARG) \
	PG_FUNCTION_INFO_V1(NAME); \
	Datum \
	NAME(PG_FUNCTION_ARGS) \
	{ \
		STATE	   *s = ASA_STATE_OR_NULL(STATE, 0); \
		asa_float	counters[ASA_LANES]; \
		uint64		update_count; \
		uint64		key_hash = COUNTERS(s, counters, &update_count); \
		return asa_sum_noised_release(fcinfo, counters, key_hash, update_count, \
									  asa_agg_release_params(FROM_ARG, s ? &s->rel : NULL)); \
	}

ASA_DEFINE_SUM_NOISED_FINAL(as_sum_int_noised_final, AsaSumIntPair, asa_sum_int_counters, false)
ASA_DEFINE_SUM_NOISED_FINAL(as_sum_int_qnoised_final, AsaSumIntPair, asa_sum_int_counters, true)
ASA_DEFINE_SUM_NOISED_FINAL(as_sum_float_noised_final, AsaSumFloatState, asa_sum_float_counters, false)
ASA_DEFINE_SUM_NOISED_FINAL(as_sum_float_qnoised_final, AsaSumFloatState, asa_sum_float_counters, true)

PG_FUNCTION_INFO_V1(as_sum_int_combine);
Datum
as_sum_int_combine(PG_FUNCTION_ARGS)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_sum_int_combine");
	AsaSumIntPair *dst = ASA_STATE_OR_NULL(AsaSumIntPair, 0);
	AsaSumIntPair *src = ASA_STATE_OR_NULL(AsaSumIntPair, 1);

	if (!src)
	{
		if (!dst)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(dst);
	}
	if (!dst)
	{
		dst = (AsaSumIntPair *) MemoryContextAlloc(aggctx, sizeof(AsaSumIntPair));
		asa_sum_pair_init(dst);
	}
	asa_sum_pair_combine(dst, src, aggctx);
	PG_RETURN_POINTER(dst);
}

PG_FUNCTION_INFO_V1(as_sum_float_combine);
Datum
as_sum_float_combine(PG_FUNCTION_ARGS)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_sum_float_combine");
	AsaSumFloatState *dst = ASA_STATE_OR_NULL(AsaSumFloatState, 0);
	AsaSumFloatState *src = ASA_STATE_OR_NULL(AsaSumFloatState, 1);

	if (!src)
	{
		if (!dst)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(dst);
	}
	if (!dst)
		dst = (AsaSumFloatState *) MemoryContextAllocZero(aggctx, sizeof(AsaSumFloatState));
	asa_sum_float_combine(dst, src);
	PG_RETURN_POINTER(dst);
}

PG_FUNCTION_INFO_V1(as_sum_int_serial);
Datum
as_sum_int_serial(PG_FUNCTION_ARGS)
{
	AsaSumIntPair *p = (AsaSumIntPair *) PG_GETARG_POINTER(0);
	AsaWriter	w;

	asa_writer_init(&w, asa_sum_pair_serial_size(p));
	asa_sum_pair_write(&w, p);
	PG_RETURN_BYTEA_P(asa_writer_finish(&w));
}

PG_FUNCTION_INFO_V1(as_sum_int_deserial);
Datum
as_sum_int_deserial(PG_FUNCTION_ARGS)
{
	AsaSumIntPair *p = (AsaSumIntPair *) palloc(sizeof(AsaSumIntPair));
	AsaReader	r;

	asa_reader_init(&r, PG_GETARG_BYTEA_PP(0));
	asa_sum_pair_read(&r, p);
	PG_RETURN_POINTER(p);
}

PG_FUNCTION_INFO_V1(as_sum_float_serial);
Datum
as_sum_float_serial(PG_FUNCTION_ARGS)
{
	AsaWriter	w;

	asa_writer_init(&w, sizeof(AsaSumFloatState));
	asa_write(&w, PG_GETARG_POINTER(0), sizeof(AsaSumFloatState));
	PG_RETURN_BYTEA_P(asa_writer_finish(&w));
}

PG_FUNCTION_INFO_V1(as_sum_float_deserial);
Datum
as_sum_float_deserial(PG_FUNCTION_ARGS)
{
	AsaSumFloatState *s = (AsaSumFloatState *) palloc(sizeof(AsaSumFloatState));
	AsaReader	r;

	asa_reader_init(&r, PG_GETARG_BYTEA_PP(0));
	asa_read(&r, s, sizeof(AsaSumFloatState));
	PG_RETURN_POINTER(s);
}

/* ---------------------------------------------------------------------------
 * SQL-callable: as_avg (sum state + count state in one aggregate)
 * ------------------------------------------------------------------------- */

static Datum
asa_avg_int_trans(FunctionCallInfo fcinfo, int64 value)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_avg");
	AsaAvgIntState *s = ASA_STATE_OR_NULL(AsaAvgIntState, 0);
	uint64		mask = ASA_GETARG_MASK(1);

	if (!s)
	{
		s = (AsaAvgIntState *) MemoryContextAllocZero(aggctx, sizeof(AsaAvgIntState));
		asa_sum_pair_init(&s->sum);
	}
	asa_sum_pair_update(&s->sum, mask, value, aggctx);
	asa_count_update(&s->count, mask, aggctx);
	PG_RETURN_POINTER(s);
}

static Datum
asa_avg_float_trans(FunctionCallInfo fcinfo, double value)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_avg");
	AsaAvgFloatState *s = ASA_STATE_OR_NULL(AsaAvgFloatState, 0);
	uint64		mask = ASA_GETARG_MASK(1);

	if (!s)
		s = (AsaAvgFloatState *) MemoryContextAllocZero(aggctx, sizeof(AsaAvgFloatState));
	asa_sum_float_update(&s->sum, mask, value);
	asa_count_update(&s->count, mask, aggctx);
	PG_RETURN_POINTER(s);
}

ASA_DEFINE_SUM_TRANS(as_avg_int2_trans, AsaAvgIntState, asa_avg_int_trans, (int64) PG_GETARG_INT16(2))
ASA_DEFINE_SUM_TRANS(as_avg_int4_trans, AsaAvgIntState, asa_avg_int_trans, (int64) PG_GETARG_INT32(2))
ASA_DEFINE_SUM_TRANS(as_avg_int8_trans, AsaAvgIntState, asa_avg_int_trans, PG_GETARG_INT64(2))
ASA_DEFINE_SUM_TRANS(as_avg_float4_trans, AsaAvgFloatState, asa_avg_float_trans, (double) PG_GETARG_FLOAT4(2))
ASA_DEFINE_SUM_TRANS(as_avg_float8_trans, AsaAvgFloatState, asa_avg_float_trans, PG_GETARG_FLOAT8(2))
ASA_DEFINE_SUM_TRANS(as_avg_numeric_trans, AsaAvgFloatState, asa_avg_float_trans,
					 asa_numeric_to_double(PG_GETARG_DATUM(2)))

PG_FUNCTION_INFO_V1(as_avg_numeric_scaled_trans);
Datum
as_avg_numeric_scaled_trans(PG_FUNCTION_ARGS)
{
	MemoryContext aggctx;
	AsaAvgIntState *s = ASA_STATE_OR_NULL(AsaAvgIntState, 0);
	uint64		mask;

	ASA_SKIP_NULL_INPUT(s);
	aggctx = asa_agg_context(fcinfo, "as_avg");
	mask = ASA_GETARG_MASK(1);
	if (!s)
	{
		s = (AsaAvgIntState *) MemoryContextAllocZero(aggctx, sizeof(AsaAvgIntState));
		asa_sum_pair_init(&s->sum);
		s->sum.scale = asa_numeric_sum_scale(fcinfo, PG_GETARG_DATUM(2));
	}
	asa_sum_pair_update(&s->sum, mask, asa_numeric_scaled(PG_GETARG_DATUM(2), s->sum.scale), aggctx);
	asa_count_update(&s->count, mask, aggctx);
	PG_RETURN_POINTER(s);
}

PG_FUNCTION_INFO_V1(as_avg_int_final);
Datum
as_avg_int_final(PG_FUNCTION_ARGS)
{
	AsaAvgIntState *s = ASA_STATE_OR_NULL(AsaAvgIntState, 0);
	double		totals[ASA_LANES] = {0};

	if (!s)
		PG_RETURN_NULL();
	PG_RETURN_ARRAYTYPE_P(asa_avg_output(asa_sum_pair_totals(&s->sum, totals), totals, &s->count));
}

PG_FUNCTION_INFO_V1(as_avg_float_final);
Datum
as_avg_float_final(PG_FUNCTION_ARGS)
{
	AsaAvgFloatState *s = ASA_STATE_OR_NULL(AsaAvgFloatState, 0);
	double		totals[ASA_LANES];

	if (!s)
		PG_RETURN_NULL();
	for (int j = 0; j < ASA_LANES; j++)
		totals[j] = s->sum.totals[j];
	PG_RETURN_ARRAYTYPE_P(asa_avg_output(s->sum.key_hash, totals, &s->count));
}

/* Noised sum / noised count; NULL if either release is NULL or the count is 0. */
static Datum
asa_avg_noised_release(FunctionCallInfo fcinfo, const asa_float *sums, uint64 sum_key_hash, uint64 sum_rows,
					   const AsaCountState *count, AsaReleaseParams rel)
{
	asa_float	counts[ASA_LANES];
	double		sum;
	double		cnt;

	asa_count_finalize(count, counts);
	if (!asa_release(fcinfo, 0, sums, sum_key_hash, sum_rows, &rel, true, "as_noised_sum", &sum))
		PG_RETURN_NULL();
	if (!asa_release(fcinfo, 1, counts, count->key_hash, count->update_count + count->swar_fill, &rel, true,
					 "as_noised_count", &cnt))
		PG_RETURN_NULL();
	if (cnt == 0)
		PG_RETURN_NULL();
	PG_RETURN_DATUM(asa_release_datum(fcinfo, sum / cnt));
}

static Datum
asa_avg_int_noised(FunctionCallInfo fcinfo, bool from_arg)
{
	AsaAvgIntState *s = ASA_STATE_OR_NULL(AsaAvgIntState, 0);
	asa_float	sums[ASA_LANES];
	uint64		rows;
	uint64		key_hash;

	if (!s)
		PG_RETURN_NULL();
	key_hash = asa_sum_int_counters(&s->sum, sums, &rows);
	return asa_avg_noised_release(fcinfo, sums, key_hash, rows, &s->count,
								  asa_agg_release_params(from_arg, &s->sum.rel));
}

static Datum
asa_avg_float_noised(FunctionCallInfo fcinfo, bool from_arg)
{
	AsaAvgFloatState *s = ASA_STATE_OR_NULL(AsaAvgFloatState, 0);
	asa_float	sums[ASA_LANES];
	uint64		rows;
	uint64		key_hash;

	if (!s)
		PG_RETURN_NULL();
	key_hash = asa_sum_float_counters(&s->sum, sums, &rows);
	return asa_avg_noised_release(fcinfo, sums, key_hash, rows, &s->count,
								  asa_agg_release_params(from_arg, &s->sum.rel));
}

PG_FUNCTION_INFO_V1(as_avg_int_noised_final);
Datum
as_avg_int_noised_final(PG_FUNCTION_ARGS)
{
	return asa_avg_int_noised(fcinfo, false);
}

PG_FUNCTION_INFO_V1(as_avg_int_qnoised_final);
Datum
as_avg_int_qnoised_final(PG_FUNCTION_ARGS)
{
	return asa_avg_int_noised(fcinfo, true);
}

PG_FUNCTION_INFO_V1(as_avg_float_noised_final);
Datum
as_avg_float_noised_final(PG_FUNCTION_ARGS)
{
	return asa_avg_float_noised(fcinfo, false);
}

PG_FUNCTION_INFO_V1(as_avg_float_qnoised_final);
Datum
as_avg_float_qnoised_final(PG_FUNCTION_ARGS)
{
	return asa_avg_float_noised(fcinfo, true);
}

PG_FUNCTION_INFO_V1(as_avg_int_combine);
Datum
as_avg_int_combine(PG_FUNCTION_ARGS)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_avg_int_combine");
	AsaAvgIntState *dst = ASA_STATE_OR_NULL(AsaAvgIntState, 0);
	AsaAvgIntState *src = ASA_STATE_OR_NULL(AsaAvgIntState, 1);

	if (!src)
	{
		if (!dst)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(dst);
	}
	if (!dst)
	{
		dst = (AsaAvgIntState *) MemoryContextAllocZero(aggctx, sizeof(AsaAvgIntState));
		asa_sum_pair_init(&dst->sum);
	}
	asa_sum_pair_combine(&dst->sum, &src->sum, aggctx);
	asa_count_combine(&dst->count, &src->count, aggctx);
	PG_RETURN_POINTER(dst);
}

PG_FUNCTION_INFO_V1(as_avg_float_combine);
Datum
as_avg_float_combine(PG_FUNCTION_ARGS)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_avg_float_combine");
	AsaAvgFloatState *dst = ASA_STATE_OR_NULL(AsaAvgFloatState, 0);
	AsaAvgFloatState *src = ASA_STATE_OR_NULL(AsaAvgFloatState, 1);

	if (!src)
	{
		if (!dst)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(dst);
	}
	if (!dst)
		dst = (AsaAvgFloatState *) MemoryContextAllocZero(aggctx, sizeof(AsaAvgFloatState));
	asa_sum_float_combine(&dst->sum, &src->sum);
	asa_count_combine(&dst->count, &src->count, aggctx);
	PG_RETURN_POINTER(dst);
}

PG_FUNCTION_INFO_V1(as_avg_int_serial);
Datum
as_avg_int_serial(PG_FUNCTION_ARGS)
{
	AsaAvgIntState *s = (AsaAvgIntState *) PG_GETARG_POINTER(0);
	AsaWriter	w;

	asa_writer_init(&w, asa_sum_pair_serial_size(&s->sum) + asa_count_serial_size());
	asa_sum_pair_write(&w, &s->sum);
	asa_count_write(&w, &s->count);
	PG_RETURN_BYTEA_P(asa_writer_finish(&w));
}

PG_FUNCTION_INFO_V1(as_avg_int_deserial);
Datum
as_avg_int_deserial(PG_FUNCTION_ARGS)
{
	AsaAvgIntState *s = (AsaAvgIntState *) palloc(sizeof(AsaAvgIntState));
	AsaReader	r;

	asa_reader_init(&r, PG_GETARG_BYTEA_PP(0));
	asa_sum_pair_read(&r, &s->sum);
	s->count = *asa_count_read(&r);
	PG_RETURN_POINTER(s);
}

PG_FUNCTION_INFO_V1(as_avg_float_serial);
Datum
as_avg_float_serial(PG_FUNCTION_ARGS)
{
	AsaAvgFloatState *s = (AsaAvgFloatState *) PG_GETARG_POINTER(0);
	AsaWriter	w;

	asa_writer_init(&w, sizeof(AsaSumFloatState) + asa_count_serial_size());
	asa_write(&w, &s->sum, sizeof(AsaSumFloatState));
	asa_count_write(&w, &s->count);
	PG_RETURN_BYTEA_P(asa_writer_finish(&w));
}

PG_FUNCTION_INFO_V1(as_avg_float_deserial);
Datum
as_avg_float_deserial(PG_FUNCTION_ARGS)
{
	AsaAvgFloatState *s = (AsaAvgFloatState *) palloc(sizeof(AsaAvgFloatState));
	AsaReader	r;

	asa_reader_init(&r, PG_GETARG_BYTEA_PP(0));
	asa_read(&r, &s->sum, sizeof(AsaSumFloatState));
	s->count = *asa_count_read(&r);
	PG_RETURN_POINTER(s);
}

/* Transition functions of the asa_* forms (release parameters as the last arguments, 3 and 4). */
ASA_DEFINE_QTRANS(as_sum_int2_qtrans, as_sum_int2_trans, AsaSumIntPair, rel, 3)
ASA_DEFINE_QTRANS(as_sum_int4_qtrans, as_sum_int4_trans, AsaSumIntPair, rel, 3)
ASA_DEFINE_QTRANS(as_sum_int8_qtrans, as_sum_int8_trans, AsaSumIntPair, rel, 3)
ASA_DEFINE_QTRANS(as_sum_float4_qtrans, as_sum_float4_trans, AsaSumFloatState, rel, 3)
ASA_DEFINE_QTRANS(as_sum_float8_qtrans, as_sum_float8_trans, AsaSumFloatState, rel, 3)
ASA_DEFINE_QTRANS(as_sum_numeric_qtrans, as_sum_numeric_trans, AsaSumFloatState, rel, 3)
ASA_DEFINE_QTRANS(as_sum_numeric_scaled_qtrans, as_sum_numeric_scaled_trans, AsaSumIntPair, rel, 3)
ASA_DEFINE_QTRANS(as_avg_int2_qtrans, as_avg_int2_trans, AsaAvgIntState, sum.rel, 3)
ASA_DEFINE_QTRANS(as_avg_int4_qtrans, as_avg_int4_trans, AsaAvgIntState, sum.rel, 3)
ASA_DEFINE_QTRANS(as_avg_int8_qtrans, as_avg_int8_trans, AsaAvgIntState, sum.rel, 3)
ASA_DEFINE_QTRANS(as_avg_float4_qtrans, as_avg_float4_trans, AsaAvgFloatState, sum.rel, 3)
ASA_DEFINE_QTRANS(as_avg_float8_qtrans, as_avg_float8_trans, AsaAvgFloatState, sum.rel, 3)
ASA_DEFINE_QTRANS(as_avg_numeric_qtrans, as_avg_numeric_trans, AsaAvgFloatState, sum.rel, 3)
ASA_DEFINE_QTRANS(as_avg_numeric_scaled_qtrans, as_avg_numeric_scaled_trans, AsaAvgIntState, sum.rel, 3)
