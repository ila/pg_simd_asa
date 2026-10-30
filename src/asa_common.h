/*
 * asa_common.h — shared definitions for the SIMD-ASA PostgreSQL extension.
 *
 * Port of the aggregate engine of the DuckDB `privacy` extension
 * (src/include/aggregates/as_aggregate.hpp): the stochastic aggregates, the PAC noise
 * release (asa_noise.c) and the query rewrite (asa_rewrite.c). The DP mechanisms are
 * not ported.
 *
 * A 64-bit membership mask (PostgreSQL bigint, interpreted as uint64) selects
 * which of the 64 worlds a tuple belongs to: bit j set => the tuple contributes
 * to world j. Every as_* aggregate maintains 64 world-specific states and
 * returns them as a real[] of length 64.
 */
#ifndef ASA_COMMON_H
#define ASA_COMMON_H

#include "postgres.h"
#include "fmgr.h"
#include "utils/array.h"

/* pg_prng_double_normal (noise) and the range-table permission model (rewrite) */
#if PG_VERSION_NUM < 160000
#error "simd_asa requires PostgreSQL 16 or later"
#endif

#define ASA_LANES 64

/* Output element type: PostgreSQL real (float4), as LIST<FLOAT> in DuckDB. */
typedef float asa_float;

#define ASA_RESTRICT __restrict__

/*
 * Enable AVX2 code generation for the update kernels on x86 (harmless elsewhere:
 * on ARM the compiler auto-vectorizes with NEON at -O3).
 */
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#define ASA_AUTOVECTORIZE __attribute__((target("avx2")))
#else
#define ASA_AUTOVECTORIZE
#endif

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "SIMD-ASA SWAR state layouts assume a little-endian target"
#endif

static inline int
asa_popcount64(uint64 x)
{
	return __builtin_popcountll(x);
}

static inline int
asa_clzll(uint64 x)
{
	return x ? __builtin_clzll(x) : 64;
}

/* Reads a bigint mask argument as uint64 (bit pattern preserved). */
#define ASA_GETARG_MASK(n) ((uint64) PG_GETARG_INT64(n))

/*
 * Release parameters of the asa_* forms, given as their two last arguments and stored in
 * the state by their transition functions (zero: not set).
 *   query_hash: the statement's query hash.
 *   correction: 2^(k-1) for rows derived from k entities, whose masks are AND-ed: such a
 *               row is in a world with probability 2^-k, not 1/2 (DuckDB's correction).
 */
typedef struct AsaReleaseParams
{
	uint64		query_hash;
	double		correction;
} AsaReleaseParams;

static inline double
asa_correction(const AsaReleaseParams *rel)
{
	return rel && rel->correction > 0 ? rel->correction : 1.0;
}

/* Keeps dst's parameters, or takes src's when dst has none (combine functions). */
static inline void
asa_release_params_merge(AsaReleaseParams *dst, const AsaReleaseParams *src)
{
	if (!dst->query_hash && dst->correction == 0)
		*dst = *src;
}

/* Multiplies 2x-scaled world counters of count and sum by the correction. */
static inline void
asa_correct_counters(asa_float *counters, const AsaReleaseParams *rel)
{
	double		c = asa_correction(rel);

	if (c != 1.0)
		for (int j = 0; j < ASA_LANES; j++)
			counters[j] = (asa_float) (counters[j] * c);
}

/* Returns the aggregate memory context, erroring when not called as an aggregate. */
static inline MemoryContext
asa_agg_context(FunctionCallInfo fcinfo, const char *fname)
{
	MemoryContext aggctx;

	if (!AggCheckCallContext(fcinfo, &aggctx))
		elog(ERROR, "%s called in non-aggregate context", fname);
	return aggctx;
}

/* Defines the rewrite settings and installs the parse-analysis hook (asa_rewrite.c). */
extern void asa_rewrite_init(void);

/* Defines the noise settings and installs the executor hooks (asa_noise.c). */
extern void asa_noise_init(void);

/*
 * Query hashes (asa_common.c). A query hash is XOR-ed into every membership mask
 * (priv_hash) and selects the released world and the noise of a statement.
 */
extern uint64 asa_query_hash_from_seed(uint64 seed);
extern uint64 asa_seed_query_hash(void);

/* Builds a 64-element real[] without NULLs. */
extern ArrayType *asa_make_array(const asa_float *vals);

/* Builds a 64-element real[]; lanes with nulls[j] set become NULL elements. */
extern ArrayType *asa_make_array_nulls(const asa_float *vals, const bool *nulls);

/*
 * Reads a real[] into vals/nulls (at most 64 elements). Returns the number of
 * elements read. Errors on a multi-dimensional array.
 */
extern int	asa_read_array(ArrayType *arr, asa_float *vals, bool *nulls);

/* Raw byte helpers for serial/deserial functions (same-machine parallel workers). */
typedef struct AsaWriter
{
	bytea	   *buf;
	char	   *pos;
} AsaWriter;

typedef struct AsaReader
{
	const char *pos;
	const char *end;
} AsaReader;

extern void asa_writer_init(AsaWriter *w, Size payload_size);
extern bytea *asa_writer_finish(AsaWriter *w);
extern void asa_reader_init(AsaReader *r, bytea *buf);

static inline void
asa_write(AsaWriter *w, const void *src, Size n)
{
	memcpy(w->pos, src, n);
	w->pos += n;
}

static inline void
asa_read(AsaReader *r, void *dst, Size n)
{
	if (r->pos + n > r->end)
		elog(ERROR, "SIMD-ASA: truncated serialized aggregate state");
	memcpy(dst, r->pos, n);
	r->pos += n;
}

#endif							/* ASA_COMMON_H */
