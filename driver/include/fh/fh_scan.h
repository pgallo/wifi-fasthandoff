/* SPDX-License-Identifier: MIT */
#ifndef FH_SCAN_H
#define FH_SCAN_H

#include "fh/fh_types.h"

/* One entry of the candidate cache, indexed by AP. */
struct fh_cand {
	int16_t rssi;
	int16_t prev_rssi;
	fh_ms_t seen;
	fh_ms_t prev_seen;
	uint8_t chan;
	uint8_t load_pct;
	bool valid;
};

/*
 * Selective background scanning. The channel list comes from neighbour
 * reports (802.11k style), not from a sweep of the band. Off-channel time is
 * limited to budget_ms per window_ms so the data path keeps its share.
 */
struct fh_scan {
	struct fh_cand cache[FH_MAX_APS];
	uint8_t chan[FH_MAX_CHANNELS];
	uint8_t n_chan;
	uint8_t next;
	fh_ms_t window_start;
	uint16_t used_ms;
	uint16_t budget_ms;
	uint16_t window_ms;
	uint16_t max_age_ms;
	uint32_t offchan_total_ms;
};

void fh_scan_init(struct fh_scan *s, uint16_t budget_ms, uint16_t window_ms, uint16_t max_age_ms);
void fh_scan_set_channels(struct fh_scan *s, const uint8_t *chans, int n);
/* Move the given channels to the front, keeping the relative order of the rest. */
void fh_scan_prioritise(struct fh_scan *s, const uint8_t *chans, int n);
/* Next channel to visit, or -FH_EAGAIN when the window budget is spent. */
int fh_scan_next(struct fh_scan *s, fh_ms_t now, uint16_t dwell_ms);
void fh_scan_charge(struct fh_scan *s, uint16_t ms);
void fh_scan_update(struct fh_scan *s, fh_ms_t now, fh_ap_t ap, uint8_t chan, int rssi, uint8_t load_pct);
void fh_scan_age(struct fh_scan *s, fh_ms_t now);
const struct fh_cand *fh_scan_get(const struct fh_scan *s, fh_ap_t ap, fh_ms_t now, uint16_t max_age_ms);
/* Slope in dB/s from the last two observations, 0 when unknown. */
int fh_cand_slope(const struct fh_cand *c);
/* Best fresh candidate by rssi - load_weight * load, excluding one AP. */
fh_ap_t fh_scan_best(const struct fh_scan *s, fh_ms_t now, uint16_t max_age_ms, fh_ap_t exclude,
		     unsigned load_weight_db, int *score_out);

#endif
