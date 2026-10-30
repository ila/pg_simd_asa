/*
 * asa_minmax.c — as_min(mask, value) / as_max(mask, value) -> real[64]
 *
 * Port of PacMinMaxState (DuckDB: src/include/aggregates/as_min_max.hpp).
 *
 * The state keeps T extremes[64]. An update is branch-free: the membership bits are
 * extracted SWAR-style into a T-wide 0/1 per world, turned into an all-ones/all-zeros
 * mask, and used to bit-select between the candidate and the current extreme, so the
 * 64-iteration loop auto-vectorizes. BOUNDOPT: every 2048 updates the worst of the 64
 * extremes is recomputed; a value that does not beat it cannot change any world and
 * is skipped without touching the array.
 *
 * Because of the SWAR extraction, extremes[] is stored in interleaved order: world j
 * lives at (j % SHIFTS) * ELEMS + j / SHIFTS, with ELEMS = 8 / sizeof(T) and
 * SHIFTS = 64 / ELEMS.
 *
 * Output lane j is the extreme of world j (no scaling) and 0 for worlds no row was
 * assigned to, as in DuckDB. numeric inputs take the double precision path.
 * as_noised_min / as_noised_max release one noised extreme from the same state.
 */
#include "asa_common.h"
#include "asa_noise.h"

#include <float.h>
#include <math.h>

#include "utils/builtins.h"

#define ASA_BOUND_RECOMPUTE_INTERVAL 2048

#define ASA_DEFINE_MINMAX(NAME, T, UINT_T, BITS_T, SHIFTS, MASK, IS_MAX, IS_FLOAT, INIT, GETARG) \
	typedef struct NAME##_state \
	{ \
		uint64		update_count; \
		uint64		key_hash;	/* OR of all masks seen */ \
		AsaReleaseParams rel;	/* asa_* forms */ \
		T			global_bound;	/* worst of the 64 extremes (refreshed periodically) */ \
		T			extremes[ASA_LANES]; \
	} NAME##_state; \
	\
	static inline bool \
	NAME##_is_better(T a, T b) \
	{ \
		return IS_MAX ? (a > b) : (a < b); \
	} \
	\
	static inline T \
	NAME##_better(T a, T b) \
	{ \
		return NAME##_is_better(a, b) ? a : b; \
	} \
	\
	static NAME##_state * \
	NAME##_create(MemoryContext ctx) \
	{ \
		NAME##_state *s = (NAME##_state *) MemoryContextAllocZero(ctx, sizeof(NAME##_state)); \
		for (int i = 0; i < ASA_LANES; i++) \
			s->extremes[i] = (INIT); \
		s->global_bound = (INIT); \
		return s; \
	} \
	\
	/* Branch-free predicated update of all worlds selected by mask. */ \
	static ASA_AUTOVECTORIZE inline void \
	NAME##_update_extremes(T *ASA_RESTRICT extremes, uint64 mask, T value) \
	{ \
		union \
		{ \
			uint64		u64[SHIFTS]; \
			BITS_T		bits[ASA_LANES]; \
		}			buf; \
		for (int i = 0; i < (SHIFTS); i++) /* SWAR: fewer than 64 iterations */ \
			buf.u64[i] = (mask >> i) & (MASK); \
		for (int i = 0; i < ASA_LANES; i++) \
		{ \
			UINT_T		sel = (UINT_T) (-buf.bits[i]);	/* 1 -> 0xFF.., 0 -> 0 */ \
			if (IS_FLOAT) \
			{ \
				union { T val; UINT_T bits; } cur, res; \
				cur.val = extremes[i]; \
				res.val = NAME##_better(value, cur.val); \
				res.bits = (res.bits & sel) | (cur.bits & ~sel); \
				extremes[i] = res.val; \
			} \
			else \
			{ \
				/* unselected worlds see the neutral value: INT_MIN for max, INT_MAX for min */ \
				UINT_T		noop = (UINT_T) (~sel & 1) << (sizeof(T) * 8 - 1); \
				UINT_T		val_bits = (UINT_T) value; \
				UINT_T		res_bits = IS_MAX ? ((val_bits & sel) | noop) : ((val_bits | ~sel) & ~noop); \
				extremes[i] = NAME##_better((T) res_bits, extremes[i]); \
			} \
		} \
	} \
	\
	static void \
	NAME##_recompute_bound(NAME##_state *s) \
	{ \
		if ((++s->update_count & (ASA_BOUND_RECOMPUTE_INTERVAL - 1)) == 0) \
		{ \
			T			bound = s->extremes[0]; \
			for (int i = 1; i < ASA_LANES; i++) \
				bound = NAME##_is_better(bound, s->extremes[i]) ? s->extremes[i] : bound; \
			s->global_bound = bound; \
		} \
	} \
	\
	PG_FUNCTION_INFO_V1(NAME##_trans); \
	Datum \
	NAME##_trans(PG_FUNCTION_ARGS) \
	{ \
		MemoryContext aggctx = asa_agg_context(fcinfo, #NAME); \
		NAME##_state *s = PG_ARGISNULL(0) ? NULL : (NAME##_state *) PG_GETARG_POINTER(0); \
		uint64		mask; \
		T			value; \
		if (PG_ARGISNULL(1) || PG_ARGISNULL(2)) \
		{ \
			if (!s) \
				PG_RETURN_NULL(); \
			PG_RETURN_POINTER(s); \
		} \
		if (!s) \
			s = NAME##_create(aggctx); \
		mask = ASA_GETARG_MASK(1); \
		value = (GETARG); \
		s->key_hash |= mask; \
		if (!NAME##_is_better(value, s->global_bound)) \
			PG_RETURN_POINTER(s); /* BOUNDOPT: cannot improve any world */ \
		NAME##_update_extremes(s->extremes, mask, value); \
		NAME##_recompute_bound(s); \
		PG_RETURN_POINTER(s); \
	} \
	\
	static void \
	NAME##_counters(const NAME##_state *s, asa_float *out) \
	{ \
		const int	elems = 8 / (int) sizeof(T); \
		const int	shifts = ASA_LANES / elems; \
		for (int j = 0; j < ASA_LANES; j++) \
			out[j] = (s && ((s->key_hash >> j) & 1)) ? \
				(asa_float) s->extremes[(j % shifts) * elems + j / shifts] : 0; \
	} \
	\
	PG_FUNCTION_INFO_V1(NAME##_final); \
	Datum \
	NAME##_final(PG_FUNCTION_ARGS) \
	{ \
		asa_float	out[ASA_LANES]; \
		NAME##_counters(PG_ARGISNULL(0) ? NULL : (NAME##_state *) PG_GETARG_POINTER(0), out); \
		PG_RETURN_ARRAYTYPE_P(asa_make_array(out)); \
	} \
	\
	/* asa_counters_min/max: NULL without input rows */ \
	PG_FUNCTION_INFO_V1(NAME##_counters_final); \
	Datum \
	NAME##_counters_final(PG_FUNCTION_ARGS) \
	{ \
		if (PG_ARGISNULL(0)) \
			PG_RETURN_NULL(); \
		return NAME##_final(fcinfo); \
	} \
	\
	static Datum \
	NAME##_noised(FunctionCallInfo fcinfo, bool from_arg) \
	{ \
		NAME##_state *s = PG_ARGISNULL(0) ? NULL : (NAME##_state *) PG_GETARG_POINTER(0); \
		asa_float	out[ASA_LANES]; \
		AsaReleaseParams rel = asa_agg_release_params(from_arg, s ? &s->rel : NULL); \
		double		value; \
		NAME##_counters(s, out); \
		/* the correction makes NULL less likely, but does not scale extremes */ \
		if (!asa_release(fcinfo, 0, out, s ? s->key_hash : 0, s ? s->update_count : 0, &rel, false, \
						 IS_MAX ? "as_noised_max" : "as_noised_min", &value)) \
			PG_RETURN_NULL(); \
		PG_RETURN_DATUM(asa_release_datum(fcinfo, value)); \
	} \
	\
	PG_FUNCTION_INFO_V1(NAME##_noised_final); \
	Datum \
	NAME##_noised_final(PG_FUNCTION_ARGS) \
	{ \
		return NAME##_noised(fcinfo, false); \
	} \
	\
	PG_FUNCTION_INFO_V1(NAME##_qnoised_final); \
	Datum \
	NAME##_qnoised_final(PG_FUNCTION_ARGS) \
	{ \
		return NAME##_noised(fcinfo, true); \
	} \
	\
	PG_FUNCTION_INFO_V1(NAME##_combine); \
	Datum \
	NAME##_combine(PG_FUNCTION_ARGS) \
	{ \
		MemoryContext aggctx = asa_agg_context(fcinfo, #NAME "_combine"); \
		NAME##_state *dst = PG_ARGISNULL(0) ? NULL : (NAME##_state *) PG_GETARG_POINTER(0); \
		NAME##_state *src = PG_ARGISNULL(1) ? NULL : (NAME##_state *) PG_GETARG_POINTER(1); \
		if (!src) \
		{ \
			if (!dst) \
				PG_RETURN_NULL(); \
			PG_RETURN_POINTER(dst); \
		} \
		if (!dst) \
			dst = NAME##_create(aggctx); \
		dst->key_hash |= src->key_hash; \
		dst->update_count += src->update_count; \
		asa_release_params_merge(&dst->rel, &src->rel); \
		for (int i = 0; i < ASA_LANES; i++) /* same type => same interleaving */ \
			dst->extremes[i] = NAME##_better(dst->extremes[i], src->extremes[i]); \
		NAME##_recompute_bound(dst); \
		PG_RETURN_POINTER(dst); \
	} \
	\
	ASA_DEFINE_QTRANS(NAME##_qtrans, NAME##_trans, NAME##_state, rel, 3) \
	\
	PG_FUNCTION_INFO_V1(NAME##_serial); \
	Datum \
	NAME##_serial(PG_FUNCTION_ARGS) \
	{ \
		AsaWriter	w; \
		asa_writer_init(&w, sizeof(NAME##_state)); \
		asa_write(&w, PG_GETARG_POINTER(0), sizeof(NAME##_state)); \
		PG_RETURN_BYTEA_P(asa_writer_finish(&w)); \
	} \
	\
	PG_FUNCTION_INFO_V1(NAME##_deserial); \
	Datum \
	NAME##_deserial(PG_FUNCTION_ARGS) \
	{ \
		NAME##_state *s = (NAME##_state *) palloc(sizeof(NAME##_state)); \
		AsaReader	r; \
		asa_reader_init(&r, PG_GETARG_BYTEA_PP(0)); \
		asa_read(&r, s, sizeof(NAME##_state)); \
		PG_RETURN_POINTER(s); \
	}

#define ASA_NUMERIC_ARG(n) DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow, PG_GETARG_DATUM(n)))

/* NAME, T, UINT_T, BITS_T, SHIFTS, MASK, IS_MAX, IS_FLOAT, INIT, GETARG */
ASA_DEFINE_MINMAX(as_min_int2, int16, uint16, int16, 16, UINT64CONST(0x0001000100010001), false, false,
				  PG_INT16_MAX, PG_GETARG_INT16(2))
ASA_DEFINE_MINMAX(as_max_int2, int16, uint16, int16, 16, UINT64CONST(0x0001000100010001), true, false,
				  PG_INT16_MIN, PG_GETARG_INT16(2))
ASA_DEFINE_MINMAX(as_min_int4, int32, uint32, int32, 32, UINT64CONST(0x0000000100000001), false, false,
				  PG_INT32_MAX, PG_GETARG_INT32(2))
ASA_DEFINE_MINMAX(as_max_int4, int32, uint32, int32, 32, UINT64CONST(0x0000000100000001), true, false,
				  PG_INT32_MIN, PG_GETARG_INT32(2))
ASA_DEFINE_MINMAX(as_min_int8, int64, uint64, int64, 64, UINT64CONST(1), false, false,
				  PG_INT64_MAX, PG_GETARG_INT64(2))
ASA_DEFINE_MINMAX(as_max_int8, int64, uint64, int64, 64, UINT64CONST(1), true, false,
				  PG_INT64_MIN, PG_GETARG_INT64(2))
ASA_DEFINE_MINMAX(as_min_float4, float4, uint32, int32, 32, UINT64CONST(0x0000000100000001), false, true,
				  INFINITY, PG_GETARG_FLOAT4(2))
ASA_DEFINE_MINMAX(as_max_float4, float4, uint32, int32, 32, UINT64CONST(0x0000000100000001), true, true,
				  -INFINITY, PG_GETARG_FLOAT4(2))
ASA_DEFINE_MINMAX(as_min_float8, float8, uint64, int64, 64, UINT64CONST(1), false, true,
				  INFINITY, PG_GETARG_FLOAT8(2))
ASA_DEFINE_MINMAX(as_max_float8, float8, uint64, int64, 64, UINT64CONST(1), true, true,
				  -INFINITY, PG_GETARG_FLOAT8(2))
ASA_DEFINE_MINMAX(as_min_numeric, float8, uint64, int64, 64, UINT64CONST(1), false, true,
				  INFINITY, ASA_NUMERIC_ARG(2))
ASA_DEFINE_MINMAX(as_max_numeric, float8, uint64, int64, 64, UINT64CONST(1), true, true,
				  -INFINITY, ASA_NUMERIC_ARG(2))
