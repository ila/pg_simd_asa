/*
 * asa_count.h — SWAR count state, shared by as_count and as_avg.
 */
#ifndef ASA_COUNT_H
#define ASA_COUNT_H

#include "asa_common.h"

typedef struct AsaCountState
{
	uint64		total8[8];		/* SWAR: 64 packed uint8 counters */
	uint16		swar_fill;		/* updates in total8 since the last flush (0-254) */
	uint64		key_hash;		/* OR of all masks seen: worlds that received a row */
	uint64		update_count;	/* exact rows flushed from total8 */
	uint64		query_hash;		/* asa_* forms: the statement's query hash */
	uint64	   *totals;			/* lazily allocated uint64[64] */
} AsaCountState;

extern AsaCountState *asa_count_create(MemoryContext ctx);
extern void asa_count_update(AsaCountState *s, uint64 mask, MemoryContext ctx);
extern void asa_count_get_totals(const AsaCountState *s, uint64 *dst);
extern void asa_count_combine(AsaCountState *dst, const AsaCountState *src, MemoryContext ctx);
extern void asa_count_finalize(const AsaCountState *s, asa_float *out);
extern Size asa_count_serial_size(void);
extern void asa_count_write(AsaWriter *w, const AsaCountState *s);
extern AsaCountState *asa_count_read(AsaReader *r);

#endif							/* ASA_COUNT_H */
