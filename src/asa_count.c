/*
 * asa_count.c — as_count(mask [, value]) -> real[64]
 *
 * Port of PacCountState (DuckDB: src/include/aggregates/as_count.hpp).
 *
 * World j counts the rows whose mask has bit j set. SWAR (SIMD Within A Register):
 * eight uint64 words each hold eight packed uint8 counters, so a single update is
 * eight branch-free word additions of (mask >> i) & 0x0101..01 instead of 64
 * conditional increments. Every 255 updates (before a byte can overflow) the packed
 * counters are cascaded into lazily allocated uint64 totals.
 *
 * Output lane j is 2 * count_j (the 2x compensates the 50% inclusion probability of
 * a uniform 64-bit mask) and 0 for worlds no row was assigned to — identical to
 * as_count in DuckDB. as_noised_count releases one noised count from the same state.
 */
#include "asa_common.h"
#include "asa_count.h"
#include "asa_noise.h"

/* Selects the lowest bit of each byte. */
#define ASA_COUNT_MASK UINT64CONST(0x0101010101010101)

/* Maps world (bit) j to its byte position inside total8: undoes the SWAR interleaving. */
static inline int
asa_count_swar_pos(int bit)
{
	return (bit % 8) * 8 + bit / 8;
}

static inline uint64 *
asa_count_ensure_totals(AsaCountState *s, MemoryContext ctx)
{
	if (!s->totals)
		s->totals = (uint64 *) MemoryContextAllocZero(ctx, ASA_LANES * sizeof(uint64));
	return s->totals;
}

/* Cascades the packed uint8 counters into dst (64 uint64 totals) and clears them. */
static void
asa_count_flush_swar_into(AsaCountState *s, uint64 *dst)
{
	const uint8 *src = (const uint8 *) s->total8;

	if (s->swar_fill == 0)
		return;
	for (int bit = 0; bit < ASA_LANES; bit++)
		dst[bit] += src[asa_count_swar_pos(bit)];
	memset(s->total8, 0, sizeof(s->total8));
	s->update_count += s->swar_fill;
	s->swar_fill = 0;
}

static ASA_AUTOVECTORIZE inline void
asa_count_update_swar(AsaCountState *s, uint64 mask)
{
	for (int j = 0; j < 8; j++) /* 8, not 64 iterations: every add updates 8 counters */
		s->total8[j] += (mask >> j) & ASA_COUNT_MASK;
}

AsaCountState *
asa_count_create(MemoryContext ctx)
{
	return (AsaCountState *) MemoryContextAllocZero(ctx, sizeof(AsaCountState));
}

void
asa_count_update(AsaCountState *s, uint64 mask, MemoryContext ctx)
{
	s->key_hash |= mask;
	asa_count_update_swar(s, mask);
	if (unlikely(++s->swar_fill >= 255))
		asa_count_flush_swar_into(s, asa_count_ensure_totals(s, ctx));
}

/* Adds the exact per-world counts (totals + unflushed SWAR bytes) to dst. */
void
asa_count_get_totals(const AsaCountState *s, uint64 *dst)
{
	const uint8 *src = (const uint8 *) s->total8;

	for (int bit = 0; bit < ASA_LANES; bit++)
		dst[bit] += (s->totals ? s->totals[bit] : 0) + (s->swar_fill ? src[asa_count_swar_pos(bit)] : 0);
}

void
asa_count_combine(AsaCountState *dst, const AsaCountState *src, MemoryContext ctx)
{
	uint64	   *totals = asa_count_ensure_totals(dst, ctx);

	asa_count_flush_swar_into(dst, totals);
	asa_count_get_totals(src, totals);
	dst->key_hash |= src->key_hash;
	dst->update_count += src->update_count + src->swar_fill;
	if (!dst->query_hash)
		dst->query_hash = src->query_hash;
}

/* Lane j: 2 * count_j for worlds that received a row, 0 otherwise. */
void
asa_count_finalize(const AsaCountState *s, asa_float *out)
{
	uint64		totals[ASA_LANES] = {0};

	if (s)
		asa_count_get_totals(s, totals);
	for (int j = 0; j < ASA_LANES; j++)
		out[j] = (s && ((s->key_hash >> j) & 1)) ? (asa_float) ((double) totals[j] * 2.0) : 0;
}

/* Serialized form: key_hash, update_count, query_hash, 64 exact totals (SWAR bytes folded in). */
#define ASA_COUNT_SERIAL_SIZE ((3 + ASA_LANES) * sizeof(uint64))

void
asa_count_write(AsaWriter *w, const AsaCountState *s)
{
	uint64		totals[ASA_LANES] = {0};
	uint64		update_count = s->update_count + s->swar_fill;

	asa_count_get_totals(s, totals);
	asa_write(w, &s->key_hash, sizeof(uint64));
	asa_write(w, &update_count, sizeof(uint64));
	asa_write(w, &s->query_hash, sizeof(uint64));
	asa_write(w, totals, sizeof(totals));
}

Size
asa_count_serial_size(void)
{
	return ASA_COUNT_SERIAL_SIZE;
}

AsaCountState *
asa_count_read(AsaReader *r)
{
	AsaCountState *s = asa_count_create(CurrentMemoryContext);

	s->totals = (uint64 *) palloc(ASA_LANES * sizeof(uint64));
	asa_read(r, &s->key_hash, sizeof(uint64));
	asa_read(r, &s->update_count, sizeof(uint64));
	asa_read(r, &s->query_hash, sizeof(uint64));
	asa_read(r, s->totals, ASA_LANES * sizeof(uint64));
	return s;
}

/* ---------------------------------------------------------------------------
 * SQL-callable support functions
 * ------------------------------------------------------------------------- */

static Datum
asa_count_trans_common(FunctionCallInfo fcinfo, bool skip)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_count");
	AsaCountState *s = PG_ARGISNULL(0) ? NULL : (AsaCountState *) PG_GETARG_POINTER(0);

	if (skip)
	{
		if (!s)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(s);
	}
	if (!s)
		s = asa_count_create(aggctx);
	asa_count_update(s, ASA_GETARG_MASK(1), aggctx);
	PG_RETURN_POINTER(s);
}

/* as_count(mask): counts rows with a non-NULL mask. */
PG_FUNCTION_INFO_V1(as_count_trans);
Datum
as_count_trans(PG_FUNCTION_ARGS)
{
	return asa_count_trans_common(fcinfo, PG_ARGISNULL(1));
}

/* as_count(mask, value): counts rows where both mask and value are non-NULL. */
PG_FUNCTION_INFO_V1(as_count_any_trans);
Datum
as_count_any_trans(PG_FUNCTION_ARGS)
{
	return asa_count_trans_common(fcinfo, PG_ARGISNULL(1) || PG_ARGISNULL(2));
}

ASA_DEFINE_QTRANS(as_count_qtrans, as_count_trans, AsaCountState, query_hash, 2)
ASA_DEFINE_QTRANS(as_count_any_qtrans, as_count_any_trans, AsaCountState, query_hash, 3)

PG_FUNCTION_INFO_V1(as_count_final);
Datum
as_count_final(PG_FUNCTION_ARGS)
{
	asa_float	out[ASA_LANES];

	asa_count_finalize(PG_ARGISNULL(0) ? NULL : (AsaCountState *) PG_GETARG_POINTER(0), out);
	PG_RETURN_ARRAYTYPE_P(asa_make_array(out));
}

/*
 * as_noised_count: PAC release of the counts (as_count.cpp PacCountFinalize). The query
 * hash is the last argument for the rewrite's asa_noised_count, else the seed's.
 */
static Datum
asa_count_noised_final(FunctionCallInfo fcinfo, bool from_arg)
{
	AsaCountState *s = PG_ARGISNULL(0) ? NULL : (AsaCountState *) PG_GETARG_POINTER(0);
	asa_float	out[ASA_LANES];
	double		value;

	asa_count_finalize(s, out);
	if (!asa_release(fcinfo, 0, out, s ? s->key_hash : 0, s ? s->update_count + s->swar_fill : 0,
					 asa_agg_query_hash(from_arg, s ? s->query_hash : 0), "as_noised_count", &value))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(asa_release_datum(fcinfo, value));
}

PG_FUNCTION_INFO_V1(as_count_noised_final);
Datum
as_count_noised_final(PG_FUNCTION_ARGS)
{
	return asa_count_noised_final(fcinfo, false);
}

PG_FUNCTION_INFO_V1(as_count_qnoised_final);
Datum
as_count_qnoised_final(PG_FUNCTION_ARGS)
{
	return asa_count_noised_final(fcinfo, true);
}

PG_FUNCTION_INFO_V1(as_count_combine);
Datum
as_count_combine(PG_FUNCTION_ARGS)
{
	MemoryContext aggctx = asa_agg_context(fcinfo, "as_count_combine");
	AsaCountState *dst = PG_ARGISNULL(0) ? NULL : (AsaCountState *) PG_GETARG_POINTER(0);
	AsaCountState *src = PG_ARGISNULL(1) ? NULL : (AsaCountState *) PG_GETARG_POINTER(1);

	if (!src)
	{
		if (!dst)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(dst);
	}
	if (!dst)
		dst = asa_count_create(aggctx);
	asa_count_combine(dst, src, aggctx);
	PG_RETURN_POINTER(dst);
}

PG_FUNCTION_INFO_V1(as_count_serial);
Datum
as_count_serial(PG_FUNCTION_ARGS)
{
	AsaWriter	w;

	asa_writer_init(&w, ASA_COUNT_SERIAL_SIZE);
	asa_count_write(&w, (AsaCountState *) PG_GETARG_POINTER(0));
	PG_RETURN_BYTEA_P(asa_writer_finish(&w));
}

PG_FUNCTION_INFO_V1(as_count_deserial);
Datum
as_count_deserial(PG_FUNCTION_ARGS)
{
	AsaReader	r;

	asa_reader_init(&r, PG_GETARG_BYTEA_PP(0));
	PG_RETURN_POINTER(asa_count_read(&r));
}
