/*
 * asa_noise.h — PAC noise release of world counters (asa_noise.c).
 */
#ifndef ASA_NOISE_H
#define ASA_NOISE_H

#include "asa_common.h"

/* True when releases are noised: simd_asa.noise = on and simd_asa.mi > 0. */
extern bool asa_noise_active(void);

/*
 * Query hash of a noised aggregate: for the rewrite's asa_noised_* forms (from_arg) the
 * last argument, which their transition functions store in the state; else derived from
 * simd_asa.seed.
 */
extern uint64 asa_agg_query_hash(bool from_arg, uint64 state_query_hash);

/*
 * Transition function of an asa_* form: the base transition, then the query hash (argument
 * QARG) stored in the state's FIELD.
 */
#define ASA_DEFINE_QTRANS(NAME, BASE, STATE, FIELD, QARG) \
	PG_FUNCTION_INFO_V1(NAME); \
	Datum \
	NAME(PG_FUNCTION_ARGS) \
	{ \
		Datum		result = BASE(fcinfo); \
		if (!fcinfo->isnull) \
			((STATE *) DatumGetPointer(result))->FIELD = (uint64) PG_GETARG_INT64(QARG); \
		return result; \
	}

/*
 * Releases one value of a noised aggregate from its 64 world counters (2x-scaled, as the
 * as_* aggregates return them). key_hash: the worlds that received a row; update_count:
 * the rows aggregated. slot distinguishes the releases of one aggregate call (as_noised_avg
 * releases a sum and a count) for the sample-diversity check. Returns false for a NULL
 * release.
 */
extern bool asa_release(FunctionCallInfo fcinfo, int slot, const asa_float *counters, uint64 key_hash,
						uint64 update_count, uint64 query_hash, const char *name, double *out);

/* Converts a released value to the result type of the calling function. */
extern Datum asa_release_datum(FunctionCallInfo fcinfo, double value);

#endif							/* ASA_NOISE_H */
