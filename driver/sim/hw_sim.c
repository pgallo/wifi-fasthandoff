/* SPDX-License-Identifier: MIT */
#include <math.h>

#include "fh_world.h"

/*
 * Air-time model for management exchanges. Constants are in the range that
 * handoff measurement studies report for 802.11b/g clients; what matters
 * here is their relative size (a band sweep is hundreds of ms, a full EAP
 * exchange about a hundred, a cached-key transition a few).
 */
#define SENSITIVITY_DBM  (-90)
#define CHAN_SWITCH_MS   2
#define MIN_CHAN_TIME_MS 8
#define MAX_CHAN_TIME_MS 18
#define FRAME_MS         2
#define RETRY_TIMEOUT_MS 4
#define EAP_EXCHANGES    10
#define BACKEND_RTT_MS   8

static double snr_of(const struct fh_simhw *h, fh_ap_t ap, fh_ms_t now)
{
	return fh_world_rssi(h->w, ap, now, FH_K_TX) - FH_NOISE_DBM;
}

/* One management frame exchange with link-layer retries. */
static uint16_t frame(const struct fh_simhw *h, fh_ap_t ap, fh_ms_t now, uint32_t salt)
{
	double per = fh_per(snr_of(h, ap, now));
	uint16_t ms = FRAME_MS;

	for (uint32_t tries = 0; tries < 7; tries++) {
		if (fh_crn_unit(h->seed, ap, now, FH_K_TX * 64 + salt * 8 + tries) >= per)
			break;
		ms += RETRY_TIMEOUT_MS;
	}
	return ms;
}

static uint16_t jitter(const struct fh_simhw *h, fh_ap_t ap, fh_ms_t now, uint32_t salt, double span)
{
	return (uint16_t)(fh_crn_unit(h->seed, ap, now, FH_K_JITTER * 64 + salt) * span);
}

static uint16_t sim_probe(void *hw, fh_ms_t now, uint8_t chan, struct fh_scan_report *rep, int max, int *n_rep)
{
	const struct fh_simhw *h = hw;
	int n = 0;

	for (int i = 0; i < h->w->n_ap && n < max; i++) {
		if (h->w->ap[i].chan != chan)
			continue;
		int r = fh_world_rssi(h->w, i, now, FH_K_PROBE);

		if (r < SENSITIVITY_DBM || fh_crn_unit(h->seed, (uint32_t)i, now, FH_K_PROBE) < fh_per(r - FH_NOISE_DBM))
			continue;
		rep[n].ap = (fh_ap_t)i;
		rep[n].rssi = (int16_t)r;
		rep[n].load_pct = h->w->ap[i].load_pct;
		n++;
	}
	*n_rep = n;
	return (uint16_t)(CHAN_SWITCH_MS + (n ? MAX_CHAN_TIME_MS : MIN_CHAN_TIME_MS) + jitter(h, chan, now, 1, 3.0));
}

static uint16_t sim_auth(void *hw, fh_ms_t now, fh_ap_t ap, bool ft)
{
	const struct fh_simhw *h = hw;

	return (uint16_t)(frame(h, ap, now, 1) + frame(h, ap, now, 2) + (ft ? 1 : 0));
}

static uint16_t sim_reassoc(void *hw, fh_ms_t now, fh_ap_t ap)
{
	const struct fh_simhw *h = hw;

	return (uint16_t)(frame(h, ap, now, 3) + frame(h, ap, now, 4));
}

static uint16_t four_way(const struct fh_simhw *h, fh_ap_t ap, fh_ms_t now)
{
	uint16_t ms = 4; /* key derivation and installation */

	for (uint32_t i = 0; i < 4; i++)
		ms = (uint16_t)(ms + frame(h, ap, now, 5 + i));
	return ms;
}

static uint16_t sim_key(void *hw, fh_ms_t now, fh_ap_t ap, enum fh_key_mode mode)
{
	const struct fh_simhw *h = hw;

	switch (mode) {
	case FH_KEY_FT:
		return 1; /* keys travelled in the auth/reassoc frames; install only */
	case FH_KEY_4WAY:
		return four_way(h, ap, now);
	case FH_KEY_FULL_EAP:
	default: {
		uint16_t ms = four_way(h, ap, now);

		for (uint32_t i = 0; i < EAP_EXCHANGES; i++)
			ms = (uint16_t)(ms + 1 + BACKEND_RTT_MS + jitter(h, ap, now, 10 + i, 6.0));
		return ms;
	}
	}
}

static uint16_t sim_preauth(void *hw, fh_ms_t now, fh_ap_t ap)
{
	const struct fh_simhw *h = hw;

	return (uint16_t)(40 + jitter(h, ap, now, 30, 30.0));
}

static int sim_neighbors(void *hw, fh_ap_t ap, fh_ap_t *out, int max)
{
	const struct fh_simhw *h = hw;
	const struct fh_world *w = h->w;
	int n = 0;

	if (ap >= w->n_ap)
		return 0;
	for (int i = 0; i < w->n_ap && n < max; i++) {
		double dx = w->ap[i].x - w->ap[ap].x, dy = w->ap[i].y - w->ap[ap].y;

		if (i != ap && sqrt(dx * dx + dy * dy) <= 1.6 * w->spacing)
			out[n++] = (fh_ap_t)i;
	}
	return n;
}

const struct fh_hw_ops fh_simhw_ops = {
	.probe = sim_probe,
	.auth = sim_auth,
	.reassoc = sim_reassoc,
	.key = sim_key,
	.preauth = sim_preauth,
	.neighbor_report = sim_neighbors,
};
