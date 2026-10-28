/*
 * asa_common.c — module entry point, GUCs, array helpers and membership hashing.
 *
 * Membership hashing ports priv_hash() from the DuckDB extension
 * (src/aggregates/as_aggregate.cpp): priv_hash(asa_hash(k)) here returns the same
 * mask as priv_hash(hash(k)) in DuckDB with privacy_seed = simd_asa.seed,
 * pac_hash_repair = simd_asa.hash_repair and pac_mi = 0 (deterministic seed), for
 * BIGINT/INTEGER keys.
 */
#include "asa_common.h"

#include <limits.h>

#include "catalog/pg_type.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* Same constants as the DuckDB extension, so masks are identical across systems. */
#define ASA_MAGIC_HASH UINT64CONST(2983746509182734091)
/* 64-bit prime with exactly 32 bits set (irregular pattern), used by the hash repair. */
#define ASA_HASH_PRIME UINT64CONST(0xB2833106536E95DF)

static int	asa_seed = 42;
static bool asa_hash_repair = false;	/* DuckDB's pac_hash_repair also defaults to false */

void		_PG_init(void);

void
_PG_init(void)
{
	DefineCustomIntVariable("simd_asa.seed",
							"Seed XOR-ed into every membership mask by priv_hash().",
							"Changing the seed draws a fresh assignment of sampling entities to worlds.",
							&asa_seed, 42, INT_MIN, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("simd_asa.hash_repair",
							 "Repair priv_hash() masks to exactly 32 set bits.",
							 "Every sampling entity then belongs to exactly half of the 64 worlds.",
							 &asa_hash_repair, false, PGC_USERSET, 0, NULL, NULL, NULL);
	asa_rewrite_init();			/* defines simd_asa.rewrite / simd_asa.entity: before reserving */
#if PG_VERSION_NUM >= 150000
	MarkGUCPrefixReserved("simd_asa");
#else
	EmitWarningsOnPlaceholders("simd_asa");
#endif
}

/* ---------------------------------------------------------------------------
 * Arrays
 * ------------------------------------------------------------------------- */

ArrayType *
asa_make_array(const asa_float *vals)
{
	Size		nbytes = ARR_OVERHEAD_NONULLS(1) + ASA_LANES * sizeof(float4);
	ArrayType  *arr = (ArrayType *) palloc0(nbytes);

	SET_VARSIZE(arr, nbytes);
	arr->ndim = 1;
	arr->dataoffset = 0;
	arr->elemtype = FLOAT4OID;
	ARR_DIMS(arr)[0] = ASA_LANES;
	ARR_LBOUND(arr)[0] = 1;
	memcpy(ARR_DATA_PTR(arr), vals, ASA_LANES * sizeof(float4));
	return arr;
}

ArrayType *
asa_make_array_nulls(const asa_float *vals, const bool *nulls)
{
	Datum		datums[ASA_LANES];
	bool		any_null = false;
	int			dims[1] = {ASA_LANES};
	int			lbs[1] = {1};

	for (int j = 0; j < ASA_LANES; j++)
	{
		datums[j] = Float4GetDatum(vals[j]);
		any_null |= nulls[j];
	}
	if (!any_null)
		return asa_make_array(vals);
	return construct_md_array(datums, (bool *) nulls, 1, dims, lbs, FLOAT4OID, sizeof(float4), true,
							  TYPALIGN_INT);
}

int
asa_read_array(ArrayType *arr, asa_float *vals, bool *nulls)
{
	int			n;
	const float4 *data;
	const bits8 *bitmap;

	if (ARR_ELEMTYPE(arr) != FLOAT4OID)
		elog(ERROR, "SIMD-ASA: expected a real[] counter array");
	if (ARR_NDIM(arr) == 0)
		return 0;
	if (ARR_NDIM(arr) != 1)
		ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						errmsg("SIMD-ASA: counter arrays must be one-dimensional")));
	n = Min(ARR_DIMS(arr)[0], ASA_LANES);
	data = (const float4 *) ARR_DATA_PTR(arr);
	bitmap = ARR_NULLBITMAP(arr);
	for (int j = 0; j < n; j++)
	{
		if (bitmap && !(bitmap[j / 8] & (1 << (j % 8))))
		{
			nulls[j] = true;
			vals[j] = 0;
		}
		else
		{
			nulls[j] = false;
			vals[j] = *data++;	/* float4 elements are contiguous: fixed length, int-aligned */
		}
	}
	return n;
}

/* ---------------------------------------------------------------------------
 * Serialization helpers
 * ------------------------------------------------------------------------- */

void
asa_writer_init(AsaWriter *w, Size payload_size)
{
	w->buf = (bytea *) palloc(VARHDRSZ + payload_size);
	w->pos = VARDATA(w->buf);
}

bytea *
asa_writer_finish(AsaWriter *w)
{
	SET_VARSIZE(w->buf, w->pos - (char *) w->buf);
	return w->buf;
}

void
asa_reader_init(AsaReader *r, bytea *buf)
{
	r->pos = VARDATA_ANY(buf);
	r->end = r->pos + VARSIZE_ANY_EXHDR(buf);
}

/* ---------------------------------------------------------------------------
 * Membership hashing
 * ------------------------------------------------------------------------- */

/* DuckDB's MurmurHash64 (duckdb/common/types/hash.hpp), i.e. DuckDB hash() on integers. */
static inline uint64
asa_murmur64(uint64 x)
{
	x ^= x >> 32;
	x *= UINT64CONST(0xd6e8feb86659fd93);
	x ^= x >> 32;
	x *= UINT64CONST(0xd6e8feb86659fd93);
	x ^= x >> 32;
	return x;
}

/* Repairs a 64-bit hash to exactly 32 set bits (PacRepairHash32 in DuckDB). */
static uint64
asa_repair_hash32(uint64 num)
{
	for (int round = 0; round < 16; ++round)
	{
		uint64		next = ASA_HASH_PRIME * (num ^ ASA_HASH_PRIME);
		uint64		flip = (uint64) ((int64) (32 - asa_popcount64(num)) >> 63);
		int			pop;

		num ^= flip;			/* conditionally negate without branching */
		pop = asa_popcount64(num);
		if (pop >= 26)
		{
			for (int iter = 0; iter < 10; ++iter)
			{
				uint64		mask;

				if (pop == 32)
					return num;
				mask = UINT64CONST(1) << ((next >> (6 * iter)) & 63);
				if ((num & mask) == 0)
				{
					num |= mask;
					++pop;
				}
			}
		}
		num = next;				/* fallback to next hash if repair fails */
	}
	return UINT64CONST(0xAAAAAAAAAAAAAAAA); /* 1010...10 pattern: exactly 32 bits set */
}

PG_FUNCTION_INFO_V1(asa_hash_int8);
Datum
asa_hash_int8(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) asa_murmur64((uint64) PG_GETARG_INT64(0)));
}

PG_FUNCTION_INFO_V1(asa_hash_int4);
Datum
asa_hash_int4(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) asa_murmur64((uint64) (uint32) PG_GETARG_INT32(0)));
}

PG_FUNCTION_INFO_V1(asa_priv_hash);
Datum
asa_priv_hash(PG_FUNCTION_ARGS)
{
	uint64		seed = (uint64) (int64) asa_seed;
	uint64		query_hash = (seed * ASA_MAGIC_HASH) ^ ASA_MAGIC_HASH;
	uint64		xored = ASA_GETARG_MASK(0) ^ query_hash;

	PG_RETURN_INT64((int64) (asa_hash_repair ? asa_repair_hash32(xored) : xored));
}
