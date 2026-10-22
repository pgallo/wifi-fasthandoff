/* SPDX-License-Identifier: MIT */
#ifndef FH_EWMA_H
#define FH_EWMA_H

#include "fh/fh_types.h"

/*
 * Integer EWMA in fixed point, the way drivers smooth RSSI without touching
 * the FPU. The weight of a new sample is 1 / 2^wshift; the internal value
 * keeps FH_EWMA_FRAC fractional bits so small steps are not lost.
 */
#define FH_EWMA_FRAC 10

struct fh_ewma {
	int32_t q;
	uint8_t wshift;
	bool valid;
};

static inline void fh_ewma_init(struct fh_ewma *e, unsigned wshift)
{
	e->q = 0;
	e->wshift = (uint8_t)wshift;
	e->valid = false;
}

static inline void fh_ewma_add(struct fh_ewma *e, int32_t v)
{
	int32_t s = v * (1 << FH_EWMA_FRAC);

	if (!e->valid) {
		e->q = s;
		e->valid = true;
		return;
	}
	/* division, not >>, so negative values truncate symmetrically */
	e->q += (s - e->q) / (1 << e->wshift);
}

/* Value in 1/1024 units. */
static inline int32_t fh_ewma_read_q(const struct fh_ewma *e)
{
	return e->q;
}

/* Value rounded to the nearest integer, half away from zero. */
static inline int fh_ewma_read(const struct fh_ewma *e)
{
	const int32_t half = 1 << (FH_EWMA_FRAC - 1);

	return (int)((e->q >= 0 ? e->q + half : e->q - half) / (1 << FH_EWMA_FRAC));
}

#endif
