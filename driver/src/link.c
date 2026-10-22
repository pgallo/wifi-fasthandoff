/* SPDX-License-Identifier: MIT */
#include "fh/fh_link.h"

#define RSSI_WSHIFT 3 /* 1/8 per frame; at 50 frames/s about 160 ms memory */
#define KF_Q        2.0
#define KF_RVAR     16.0

void fh_link_reset(struct fh_link *l, fh_ms_t now, int rssi_dbm)
{
	fh_ewma_init(&l->rssi, RSSI_WSHIFT);
	fh_ewma_init(&l->snr, RSSI_WSHIFT);
	fh_kf_init(&l->kf, KF_Q, KF_RVAR);
	l->fail_bits = 0;
	l->n_tx = 0;
	l->beacon_miss = 0;
	l->last_rx = now;
	if (rssi_dbm > FH_RSSI_FLOOR)
		fh_link_rx(l, now, rssi_dbm);
}

void fh_link_rx(struct fh_link *l, fh_ms_t now, int rssi_dbm)
{
	fh_ewma_add(&l->rssi, rssi_dbm);
	fh_ewma_add(&l->snr, rssi_dbm - FH_NOISE_DBM);
	fh_kf_update(&l->kf, now, (double)rssi_dbm);
	l->last_rx = now;
}

void fh_link_tx(struct fh_link *l, bool first_try_ok)
{
	l->fail_bits = (l->fail_bits << 1) | (first_try_ok ? 0u : 1u);
	if (l->n_tx < 64)
		l->n_tx++;
}

void fh_link_beacon(struct fh_link *l, fh_ms_t now, bool received, int rssi_dbm)
{
	if (!received) {
		if (l->beacon_miss < 255)
			l->beacon_miss++;
		return;
	}
	l->beacon_miss = 0;
	fh_link_rx(l, now, rssi_dbm);
}

int fh_link_rssi(const struct fh_link *l)
{
	return l->rssi.valid ? fh_ewma_read(&l->rssi) : FH_RSSI_FLOOR;
}

int fh_link_snr(const struct fh_link *l)
{
	return l->snr.valid ? fh_ewma_read(&l->snr) : 0;
}

unsigned fh_link_retry_permille(const struct fh_link *l)
{
	if (l->n_tx == 0)
		return 0;
	uint64_t mask = l->n_tx >= 64 ? ~0ull : ((1ull << l->n_tx) - 1);

	return (unsigned)__builtin_popcountll(l->fail_bits & mask) * 1000u / l->n_tx;
}

bool fh_link_beacon_lost(const struct fh_link *l, unsigned limit)
{
	return l->beacon_miss >= limit;
}
