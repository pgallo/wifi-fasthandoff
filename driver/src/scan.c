/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "fh/fh_scan.h"

void fh_scan_init(struct fh_scan *s, uint16_t budget_ms, uint16_t window_ms, uint16_t max_age_ms)
{
	memset(s, 0, sizeof(*s));
	s->budget_ms = budget_ms;
	s->window_ms = window_ms;
	s->max_age_ms = max_age_ms;
}

void fh_scan_set_channels(struct fh_scan *s, const uint8_t *chans, int n)
{
	s->n_chan = 0;
	for (int i = 0; i < n && s->n_chan < FH_MAX_CHANNELS; i++) {
		bool dup = false;

		for (int j = 0; j < s->n_chan; j++)
			dup |= s->chan[j] == chans[i];
		if (!dup)
			s->chan[s->n_chan++] = chans[i];
	}
	s->next = 0;
}

void fh_scan_prioritise(struct fh_scan *s, const uint8_t *chans, int n)
{
	uint8_t out[FH_MAX_CHANNELS];
	int k = 0;

	for (int i = 0; i < n; i++)
		for (int j = 0; j < s->n_chan; j++)
			if (s->chan[j] == chans[i]) {
				bool dup = false;

				for (int m = 0; m < k; m++)
					dup |= out[m] == chans[i];
				if (!dup)
					out[k++] = chans[i];
			}
	for (int j = 0; j < s->n_chan; j++) {
		bool taken = false;

		for (int m = 0; m < k; m++)
			taken |= out[m] == s->chan[j];
		if (!taken)
			out[k++] = s->chan[j];
	}
	memcpy(s->chan, out, (size_t)k);
	s->next = 0;
}

int fh_scan_next(struct fh_scan *s, fh_ms_t now, uint16_t dwell_ms)
{
	if (s->n_chan == 0)
		return -FH_ENOENT;
	if (now - s->window_start >= s->window_ms) {
		s->window_start = now;
		s->used_ms = 0;
	}
	if (s->used_ms + dwell_ms > s->budget_ms)
		return -FH_EAGAIN;

	int ch = s->chan[s->next];

	s->next = (uint8_t)((s->next + 1) % s->n_chan);
	return ch;
}

void fh_scan_charge(struct fh_scan *s, uint16_t ms)
{
	s->used_ms = (uint16_t)(s->used_ms + ms);
	s->offchan_total_ms += ms;
}

void fh_scan_update(struct fh_scan *s, fh_ms_t now, fh_ap_t ap, uint8_t chan, int rssi, uint8_t load_pct)
{
	if (ap >= FH_MAX_APS)
		return;
	struct fh_cand *c = &s->cache[ap];

	if (c->valid && now - c->seen <= s->max_age_ms) {
		c->prev_rssi = c->rssi;
		c->prev_seen = c->seen;
		/* one probe response is one fading sample: average with the last */
		rssi = (rssi + c->rssi) / 2;
	} else {
		c->prev_seen = now;
		c->prev_rssi = (int16_t)rssi;
	}
	c->rssi = (int16_t)rssi;
	c->seen = now;
	c->chan = chan;
	c->load_pct = load_pct;
	c->valid = true;
}

void fh_scan_age(struct fh_scan *s, fh_ms_t now)
{
	for (int i = 0; i < FH_MAX_APS; i++)
		if (s->cache[i].valid && now - s->cache[i].seen > s->max_age_ms)
			s->cache[i].valid = false;
}

const struct fh_cand *fh_scan_get(const struct fh_scan *s, fh_ap_t ap, fh_ms_t now, uint16_t max_age_ms)
{
	if (ap >= FH_MAX_APS)
		return NULL;
	const struct fh_cand *c = &s->cache[ap];

	return (c->valid && now - c->seen <= max_age_ms) ? c : NULL;
}

int fh_cand_slope(const struct fh_cand *c)
{
	fh_ms_t dt = c->seen - c->prev_seen;

	if (dt < 200 || dt > 3000)
		return 0;
	return (int)((c->rssi - c->prev_rssi) * 1000 / (int)dt);
}

fh_ap_t fh_scan_best(const struct fh_scan *s, fh_ms_t now, uint16_t max_age_ms, fh_ap_t exclude,
		     unsigned load_weight_db, int *score_out)
{
	fh_ap_t best = FH_AP_NONE;
	int best_score = -1000;

	for (int i = 0; i < FH_MAX_APS; i++) {
		const struct fh_cand *c = fh_scan_get(s, (fh_ap_t)i, now, max_age_ms);

		if (!c || i == exclude)
			continue;
		int score = c->rssi - (int)(load_weight_db * c->load_pct / 100u);

		if (score > best_score) {
			best_score = score;
			best = (fh_ap_t)i;
		}
	}
	if (score_out)
		*score_out = best_score;
	return best;
}
