/*
 * asa_noise.c — PAC noise release of world counters.
 *
 * Port of the PAC terminal layer of the DuckDB extension (src/aggregates/as_aggregate.cpp:
 * PacNoiseInNull, PacNoisySampleFrom64Counters, CheckPacSampleDiversity; src/categorical/
 * pac_categorical.cpp: as_noised, priv_filter; src/query_processing/pac_topk_rewriter.cpp:
 * priv_mean). A release turns the 64 world counters of one cell into one value:
 *
 *   1. NULL decision: with noise, the cell is NULL with probability popcount(~key_hash)/64
 *      (cells few entities contribute to are likely suppressed); without noise, only cells
 *      no world received a row are NULL.
 *   2. Value: world J = query_hash mod 64 (the same secret world for every cell of a
 *      statement) plus Gaussian noise of variance sigma^2 / (2 mi), sigma^2 the variance
 *      of the 64 counters. With p-tracking (default) the variance is weighted by a Bayesian
 *      posterior over the worlds, updated after every release of the statement, so that
 *      the noise accounts for what earlier cells revealed. Without noise: world 0.
 *
 * simd_asa.noise = off (or simd_asa.mi = 0) is DuckDB's deterministic pac_mi = 0 mode.
 *
 * Randomness is drawn from a per-statement generator seeded by the query hash, and the
 * p-tracking posterior lives with it. Both are reset when a top-level statement starts
 * executing, so re-executing a prepared statement (same query hash) releases the same
 * values rather than fresh noise that could be averaged out.
 */
#include "asa_noise.h"

#include <float.h>
#include <math.h>

#include "access/parallel.h"
#include "catalog/pg_type.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "nodes/primnodes.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"

#define ASA_MAGIC_HASH UINT64CONST(2983746509182734091)
#define ASA_SUSPICIOUS_THRESHOLD 500	/* PAC_SUSPICIOUS_THRESHOLD */
#define ASA_QUERY_STATES 8

static bool asa_noise = true;
static double asa_mi = 1.0 / 128;	/* pac_mi default */
static bool asa_ptracking = true;
static bool asa_diversity_check = true;

/* ---------------------------------------------------------------------------
 * Per-statement randomness and p-tracking
 * ------------------------------------------------------------------------- */

typedef struct AsaQueryState
{
	uint64		query_hash;
	pg_prng_state rng;
	double		p[ASA_LANES];	/* posterior over the secret world */
} AsaQueryState;

static AsaQueryState asa_query_states[ASA_QUERY_STATES];
static int	asa_num_query_states = 0;
static int	asa_next_query_state = 0;
static int	asa_exec_nesting = 0;

static ExecutorStart_hook_type prev_executor_start = NULL;
static ExecutorRun_hook_type prev_executor_run = NULL;
static ExecutorFinish_hook_type prev_executor_finish = NULL;

static AsaQueryState *
asa_query_state(uint64 query_hash)
{
	AsaQueryState *qs;
	uint64		seed = query_hash;

	for (int i = 0; i < asa_num_query_states; i++)
		if (asa_query_states[i].query_hash == query_hash)
			return &asa_query_states[i];
	if (asa_num_query_states < ASA_QUERY_STATES)
		qs = &asa_query_states[asa_num_query_states++];
	else
	{
		qs = &asa_query_states[asa_next_query_state];
		asa_next_query_state = (asa_next_query_state + 1) % ASA_QUERY_STATES;
	}
	if (IsParallelWorker())		/* workers filter rows too: give each its own stream */
		seed ^= ASA_MAGIC_HASH * (uint64) (ParallelWorkerNumber + 1);
	qs->query_hash = query_hash;
	pg_prng_seed(&qs->rng, seed);
	for (int j = 0; j < ASA_LANES; j++)
		qs->p[j] = 1.0 / ASA_LANES;
	return qs;
}

/* A top-level statement starts: forget the generators and posteriors of earlier ones. */
static void
asa_executor_start(QueryDesc *queryDesc, int eflags)
{
	if (asa_exec_nesting == 0)
		asa_num_query_states = asa_next_query_state = 0;
	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

static void
asa_executor_run(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once)
{
	asa_exec_nesting++;
	PG_TRY();
	{
		if (prev_executor_run)
			prev_executor_run(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
	}
	PG_FINALLY();
	{
		asa_exec_nesting--;
	}
	PG_END_TRY();
}

static void
asa_executor_finish(QueryDesc *queryDesc)
{
	asa_exec_nesting++;
	PG_TRY();
	{
		if (prev_executor_finish)
			prev_executor_finish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		asa_exec_nesting--;
	}
	PG_END_TRY();
}

/* ---------------------------------------------------------------------------
 * Release
 * ------------------------------------------------------------------------- */

bool
asa_noise_active(void)
{
	return asa_noise && asa_mi > 0;
}

static double
asa_effective_mi(void)
{
	return asa_noise_active() ? asa_mi : 0.0;
}

/*
 * PacNoiseInNull: NULL with probability popcount(~key_hash) / (64 * correction), or
 * without noise when no world received a row.
 */
static bool
asa_noise_in_null(uint64 key_hash, double mi, double correction, AsaQueryState *qs)
{
	uint64		range = (uint64) (ASA_LANES * correction);

	if (mi <= 0)
		return asa_popcount64(key_hash) * correction < 1.0;
	return asa_popcount64(~key_hash) > (int) (pg_prng_uint64(&qs->rng) % Max(range, 1));
}

/* PacNoisySampleFrom64Counters: vals already hold 0 for worlds outside key_hash. */
static double
asa_noisy_sample(const double *vals, double mi, uint64 query_hash, AsaQueryState *qs)
{
	double		y;
	double		noise_var;

	if (mi <= 0)
		return vals[0];
	y = vals[query_hash % ASA_LANES];
	if (asa_ptracking)
	{
		double		w_mean = 0;
		double		w_var = 0;
		double		log_p[ASA_LANES];
		double		max_log = -INFINITY;
		double		sum_exp = 0;
		double		log_sum;
		double		noisy;

		for (int k = 0; k < ASA_LANES; k++)
			w_mean += vals[k] * qs->p[k];
		for (int k = 0; k < ASA_LANES; k++)
			w_var += (vals[k] - w_mean) * (vals[k] - w_mean) * qs->p[k];
		noise_var = w_var / (2.0 * mi);
		if (noise_var <= 0 || !isfinite(noise_var))
			return y;
		noisy = y + pg_prng_double_normal(&qs->rng) * sqrt(noise_var);

		/* Bayesian update of the posterior over the secret world */
		for (int k = 0; k < ASA_LANES; k++)
		{
			double		diff = vals[k] - noisy;

			log_p[k] = log(qs->p[k] + 1e-300) - 0.5 * diff * diff / noise_var;
			max_log = Max(max_log, log_p[k]);
		}
		for (int k = 0; k < ASA_LANES; k++)
			sum_exp += exp(log_p[k] - max_log);
		log_sum = max_log + log(sum_exp);
		for (int k = 0; k < ASA_LANES; k++)
			qs->p[k] = exp(log_p[k] - log_sum);
		return noisy;
	}
	else
	{
		double		mean = 0;
		double		var = 0;

		for (int k = 0; k < ASA_LANES; k++)
			mean += vals[k];
		mean /= ASA_LANES;
		for (int k = 0; k < ASA_LANES; k++)
			var += (vals[k] - mean) * (vals[k] - mean);
		noise_var = var / (ASA_LANES - 1) / (2.0 * mi);	/* sample variance */
		if (noise_var <= 0 || !isfinite(noise_var))
			return y;
		return y + pg_prng_double_normal(&qs->rng) * sqrt(noise_var);
	}
}

typedef struct AsaDiversity
{
	uint64		total_update_count;
	uint64		suspicious;
	uint64		nonsuspicious;
} AsaDiversity;

/*
 * CheckPacSampleDiversity: a cell whose mask has 29-35 zero bits and whose worlds all hold
 * the same value looks like a single entity. Statistics accumulate over the cells of one
 * aggregate call (fn_extra of the final function persists across groups).
 */
static void
asa_check_diversity(FunctionCallInfo fcinfo, int slot, uint64 key_hash, const asa_float *counters,
					uint64 update_count, const char *name)
{
	AsaDiversity *d;
	int			zero_bits = ASA_LANES - asa_popcount64(key_hash);
	bool		suspicious = false;

	if (!fcinfo->flinfo->fn_extra)
		fcinfo->flinfo->fn_extra = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, 2 * sizeof(AsaDiversity));
	d = &((AsaDiversity *) fcinfo->flinfo->fn_extra)[slot];
	d->total_update_count += update_count;
	if (zero_bits >= 29 && zero_bits <= 35 && key_hash != 0)
	{
		bool		found = false;
		asa_float	first = 0;

		suspicious = true;
		for (int j = 0; j < ASA_LANES && suspicious; j++)
			if ((key_hash >> j) & 1)
			{
				if (!found)
				{
					first = counters[j];
					found = true;
				}
				else if (counters[j] != first)
					suspicious = false;
			}
	}
	if (suspicious)
		d->suspicious++;
	else
		d->nonsuspicious++;
	if (d->total_update_count >= ASA_SUSPICIOUS_THRESHOLD && d->suspicious > 2 * d->nonsuspicious)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						errmsg("%s detected absence of sample diversity -- which clearly is privacy unsafe", name),
						errdetail("Most groups aggregate rows of a single sampling entity."),
						errhint("Group by coarser columns, or SET simd_asa.sample_diversity_check = off.")));
}

bool
asa_release(FunctionCallInfo fcinfo, int slot, const asa_float *counters, uint64 key_hash, uint64 update_count,
			const AsaReleaseParams *rel, bool scale, const char *name, double *out)
{
	double		mi = asa_effective_mi();
	double		correction = asa_correction(rel);
	AsaQueryState *qs = asa_query_state(rel->query_hash);
	double		vals[ASA_LANES];

	if (asa_noise_in_null(key_hash, mi, correction, qs))
		return false;
	if (asa_diversity_check && fcinfo)
		asa_check_diversity(fcinfo, slot, key_hash, counters, update_count, name);
	for (int j = 0; j < ASA_LANES; j++)
		vals[j] = ((key_hash >> j) & 1) ? (asa_float) (counters[j] * (scale ? correction : 1.0)) : 0;
	*out = (asa_float) asa_noisy_sample(vals, mi, rel->query_hash, qs);	/* released as FLOAT, as in DuckDB */
	return true;
}

AsaReleaseParams
asa_agg_release_params(bool from_arg, const AsaReleaseParams *state)
{
	AsaReleaseParams rel = {asa_seed_query_hash(), 1.0};

	if (from_arg)
	{
		rel.query_hash = state ? state->query_hash : 0;
		rel.correction = asa_correction(state);
	}
	return rel;
}

/* Truncates towards zero (DuckDB's static_cast), clamped to [lo, hi]. */
static inline double
asa_clamp_trunc(double v, double lo, double hi)
{
	v = trunc(v);
	return v < lo ? lo : v > hi ? hi : v;
}

Datum
asa_release_datum(FunctionCallInfo fcinfo, double value)
{
	Oid			type = get_fn_expr_rettype(fcinfo->flinfo);

	switch (type)
	{
		case INT2OID:
			return Int16GetDatum((int16) asa_clamp_trunc(value, PG_INT16_MIN, PG_INT16_MAX));
		case INT4OID:
			return Int32GetDatum((int32) asa_clamp_trunc(value, PG_INT32_MIN, PG_INT32_MAX));
		case INT8OID:
			/* 2^63 is the first double above INT64_MAX */
			return Int64GetDatum(value >= 9223372036854775808.0 ? PG_INT64_MAX :
								 value <= -9223372036854775808.0 ? PG_INT64_MIN : (int64) trunc(value));
		case FLOAT4OID:
			return Float4GetDatum((float4) value);
		case FLOAT8OID:
			return Float8GetDatum(value);
		case NUMERICOID:
			return DirectFunctionCall1(float8_numeric, Float8GetDatum(value));
		default:
			elog(ERROR, "SIMD-ASA: unsupported result type %s for a noised release", format_type_be(type));
	}
	return (Datum) 0;			/* keep compiler quiet */
}

/* ---------------------------------------------------------------------------
 * SQL-callable scalars
 * ------------------------------------------------------------------------- */

/*
 * as_noised(real[] [, query_hash]) -> double precision: noised release of a counter array,
 * e.g. an expression evaluated per world. As DuckDB's as_noised, all worlds count as
 * populated (NULL elements as 0), so the release is never suppressed.
 */
static Datum
asa_noised_common(FunctionCallInfo fcinfo, uint64 query_hash)
{
	asa_float	counters[ASA_LANES] = {0};
	bool		nulls[ASA_LANES];
	AsaReleaseParams rel = {query_hash, 1.0};
	double		out;

	if (asa_read_array(PG_GETARG_ARRAYTYPE_P(0), counters, nulls) != ASA_LANES)
		PG_RETURN_NULL();
	if (!asa_release(NULL, 0, counters, ~UINT64CONST(0), 0, &rel, false, "as_noised", &out))
		PG_RETURN_NULL();
	PG_RETURN_FLOAT8(out);
}

PG_FUNCTION_INFO_V1(as_noised);
Datum
as_noised(PG_FUNCTION_ARGS)
{
	return asa_noised_common(fcinfo, asa_seed_query_hash());
}

PG_FUNCTION_INFO_V1(asa_noised);
Datum
asa_noised(PG_FUNCTION_ARGS)
{
	return asa_noised_common(fcinfo, ASA_GETARG_MASK(1));
}

/*
 * priv_filter(mask [, query_hash]) -> boolean: whether a row whose predicate holds in the
 * worlds of mask passes (FilterFromMask). Without noise: in at least half of the worlds;
 * with noise: with probability popcount(mask)/64.
 */
static Datum
asa_filter_common(FunctionCallInfo fcinfo, uint64 query_hash)
{
	int			popcount = asa_popcount64(ASA_GETARG_MASK(0));

	if (asa_effective_mi() <= 0)
		PG_RETURN_BOOL(popcount >= ASA_LANES / 2);
	PG_RETURN_BOOL(popcount > (int) (pg_prng_uint64(&asa_query_state(query_hash)->rng) % ASA_LANES));
}

PG_FUNCTION_INFO_V1(as_priv_filter);
Datum
as_priv_filter(PG_FUNCTION_ARGS)
{
	return asa_filter_common(fcinfo, asa_seed_query_hash());
}

PG_FUNCTION_INFO_V1(asa_filter);
Datum
asa_filter(PG_FUNCTION_ARGS)
{
	return asa_filter_common(fcinfo, ASA_GETARG_MASK(1));
}

/* priv_mean(real[]) -> double precision: mean of the non-NULL worlds (top-k ordering). */
PG_FUNCTION_INFO_V1(as_priv_mean);
Datum
as_priv_mean(PG_FUNCTION_ARGS)
{
	asa_float	vals[ASA_LANES];
	bool		nulls[ASA_LANES];
	int			n = asa_read_array(PG_GETARG_ARRAYTYPE_P(0), vals, nulls);
	double		sum = 0;
	int			valid = 0;

	for (int j = 0; j < n; j++)
		if (!nulls[j])
		{
			sum += vals[j];
			valid++;
		}
	if (valid == 0)
		PG_RETURN_NULL();
	PG_RETURN_FLOAT8((asa_float) (sum / valid));
}

/* ---------------------------------------------------------------------------
 * Settings
 * ------------------------------------------------------------------------- */

void
asa_noise_init(void)
{
	DefineCustomBoolVariable("simd_asa.noise",
							 "Add PAC noise to released aggregates.",
							 "Off: deterministic releases (world 0), as DuckDB with pac_mi = 0.",
							 &asa_noise, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("simd_asa.mi",
							 "Mutual-information bound of the PAC noise (pac_mi).",
							 "Lower values add more noise. 0 disables noise, as simd_asa.noise = off.",
							 &asa_mi, 1.0 / 128, 0.0, DBL_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("simd_asa.ptracking",
							 "Calibrate the noise of a statement's releases with a posterior over the worlds.",
							 "Persistent-secret p-tracking (pac_ptracking).",
							 &asa_ptracking, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("simd_asa.sample_diversity_check",
							 "Reject noised aggregates whose groups mostly hold a single sampling entity.",
							 "pac_sample_diversity_check.",
							 &asa_diversity_check, true, PGC_USERSET, 0, NULL, NULL, NULL);

	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = asa_executor_start;
	prev_executor_run = ExecutorRun_hook;
	ExecutorRun_hook = asa_executor_run;
	prev_executor_finish = ExecutorFinish_hook;
	ExecutorFinish_hook = asa_executor_finish;
}
