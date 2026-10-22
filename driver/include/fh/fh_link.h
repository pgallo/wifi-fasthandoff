/* SPDX-License-Identifier: MIT */
#ifndef FH_LINK_H
#define FH_LINK_H

#include "fh/fh_ewma.h"
#include "fh/fh_kalman.h"

/* Metrics for the link to the currently associated AP. */
struct fh_link {
	struct fh_ewma rssi;
	struct fh_ewma snr;
	struct fh_kf kf;
	uint64_t fail_bits; /* last 64 transmissions, bit set = needed a retry */
	uint8_t n_tx;
	uint8_t beacon_miss;
	fh_ms_t last_rx;
};

void fh_link_reset(struct fh_link *l, fh_ms_t now, int rssi_dbm);
void fh_link_rx(struct fh_link *l, fh_ms_t now, int rssi_dbm);
void fh_link_tx(struct fh_link *l, bool first_try_ok);
void fh_link_beacon(struct fh_link *l, fh_ms_t now, bool received, int rssi_dbm);

int fh_link_rssi(const struct fh_link *l);
int fh_link_snr(const struct fh_link *l);
unsigned fh_link_retry_permille(const struct fh_link *l);
bool fh_link_beacon_lost(const struct fh_link *l, unsigned limit);

#endif
