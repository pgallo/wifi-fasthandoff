/* SPDX-License-Identifier: MIT */
#include <math.h>

#include "fh/fh_drv.h"

/*
 * Baseline: a reactive client with a hard RSSI threshold. Below the
 * threshold it sweeps the band and moves to the strongest AP it heard.
 */
static void baseline_evaluate(struct fh_drv *d, fh_ms_t now, struct fh_decision *dec)
{
	if (d->sm.state != FH_ST_CONNECTED)
		return;
	bool lost = fh_link_beacon_lost(&d->link, d->cfg.beacon_loss_n);
	int rssi = fh_link_rssi(&d->link);

	if (lost || (rssi < d->cfg.base_dbm && now - d->last_scan >= d->cfg.holdoff_ms)) {
		dec->verdict = FH_GO;
		dec->target = FH_AP_NONE;
		dec->reactive = lost;
	}
}

static fh_ap_t baseline_select(struct fh_drv *d, fh_ms_t now)
{
	bool lost = fh_link_beacon_lost(&d->link, d->cfg.beacon_loss_n);
	int best_rssi = lost ? -1000 : fh_link_rssi(&d->link);
	fh_ap_t best = FH_AP_NONE;
	uint16_t age = (uint16_t)(now - d->ho_start); /* only what this scan heard */

	for (int i = 0; i < d->n_ap; i++) {
		const struct fh_cand *c = fh_scan_get(&d->scan, (fh_ap_t)i, now, age);

		if (c && i != d->cur && c->seen >= d->ho_start && c->rssi > best_rssi) {
			best_rssi = c->rssi;
			best = (fh_ap_t)i;
		}
	}
	return best;
}

const struct fh_roam_ops fh_roam_baseline = {
	.name = "baseline",
	.bgscan = false,
	.fast_transition = false,
	.evaluate = baseline_evaluate,
	.select = baseline_select,
};

/*
 * Adapted: predictive, with hysteresis and time-to-trigger.
 *  - a Kalman forecast of the serving RSSI one horizon ahead arms the
 *    handoff early (HANDOFF_IMMINENT) and starts pre-authentication;
 *  - the handoff fires only when a cached candidate beats the serving AP by
 *    hyst_db (after a load penalty) for ttt_ms without interruption;
 *  - a steep negative slope halves the time-to-trigger (vehicle speeds).
 */
#define PINGPONG_EXTRA_DB 6
#define MIN_DWELL_MS      2000

static int cur_load(const struct fh_drv *d)
{
	return d->cur < d->n_ap ? d->scan.cache[d->cur].load_pct : 0;
}

static void adapted_evaluate(struct fh_drv *d, fh_ms_t now, struct fh_decision *dec)
{
	const struct fh_drv_cfg *cfg = &d->cfg;
	int rssi = fh_link_rssi(&d->link);
	double slope = fh_kf_slope(&d->link.kf);
	int fc = (int)lround(fh_kf_forecast(&d->link.kf, now, cfg->horizon_ms));
	int worst = rssi < fc ? rssi : fc;
	int best_score;
	fh_ap_t best = fh_scan_best(&d->scan, now, d->scan.max_age_ms, d->cur, cfg->load_weight_db, &best_score);

	if (fh_link_beacon_lost(&d->link, cfg->beacon_loss_n) || rssi < cfg->emergency_dbm) {
		dec->verdict = FH_GO;
		dec->target = best;
		dec->reactive = true;
		return;
	}

	/* the forecast decides when to act; the comparison uses measured levels */
	int cur_score = rssi - (int)(cfg->load_weight_db * (unsigned)cur_load(d) / 100u);

	if (d->sm.state == FH_ST_CONNECTED) {
		if (worst < cfg->prep_dbm) {
			dec->verdict = FH_PREPARE;
			if (rssi <= cfg->roam_dbm)
				dec->eta_ms = 0;
			else if (slope < -0.2)
				dec->eta_ms = (uint16_t)fmin(10000.0, (rssi - cfg->roam_dbm) * 1000.0 / -slope);
			else
				dec->eta_ms = cfg->horizon_ms;
		}
		return;
	}

	/* FH_ST_ROAM_PREP */
	if (best != FH_AP_NONE && cfg->roam->fast_transition)
		fh_drv_preauth(d, now, best);

	int hyst = cfg->hyst_db;

	/* going straight back to the AP we just left needs a larger margin */
	if (best == d->prev && d->hist_n > 0 &&
	    now - d->hist[(d->hist_n - 1u) % FH_HO_HIST].ts < cfg->pingpong_ms)
		hyst += PINGPONG_EXTRA_DB;

	/* settle on a new AP before considering another planned move */
	bool settled = d->hist_n == 0 || now - d->hist[(d->hist_n - 1u) % FH_HO_HIST].ts >= MIN_DWELL_MS;
	/* and only move to an AP that is itself clear of the roam region */
	bool good = best != FH_AP_NONE && d->scan.cache[best].rssi >= cfg->roam_dbm;
	bool cond = settled && good && worst < cfg->roam_dbm && best_score >= cur_score + hyst;

	if (cond) {
		if (!d->ttt_start)
			d->ttt_start = now;
		uint32_t ttt = slope < -4.0 ? cfg->ttt_ms / 2u : cfg->ttt_ms;
		uint32_t held = now - d->ttt_start;
		bool keys = fh_kc_ready(&d->kc, d->ap[best].bssid, now);

		/* make before break: wait briefly for the key if pre-auth is in flight */
		if (held >= ttt && (keys || held >= ttt + 300)) {
			dec->verdict = FH_GO;
			dec->target = best;
			return;
		}
	} else {
		d->ttt_start = 0;
	}

	if (worst > cfg->prep_dbm + 3) {
		if (!d->recover_start)
			d->recover_start = now;
		else if (now - d->recover_start >= 2000)
			dec->verdict = FH_CANCEL;
	} else {
		d->recover_start = 0;
	}
}

static fh_ap_t adapted_select(struct fh_drv *d, fh_ms_t now)
{
	const struct fh_drv_cfg *cfg = &d->cfg;
	int best_score;
	fh_ap_t best = fh_scan_best(&d->scan, now, cfg->fresh_ms, d->cur, cfg->load_weight_db, &best_score);
	const struct fh_cand *t = fh_scan_get(&d->scan, d->target, now, cfg->fresh_ms);

	bool reactive = d->ho_flags & FH_EVF_REACTIVE;

	/* keep the decided (and pre-authenticated) target unless it is clearly worse */
	if (t && d->target != d->cur && (reactive || t->rssi >= cfg->roam_dbm)) {
		int score = t->rssi - (int)(cfg->load_weight_db * t->load_pct / 100u);

		if (score >= best_score - 3)
			return d->target;
	}
	/* a planned move to an AP that is already weak is not worth the break */
	if (!reactive && best != FH_AP_NONE && d->scan.cache[best].rssi < cfg->roam_dbm)
		return FH_AP_NONE;
	return best;
}

const struct fh_roam_ops fh_roam_adapted = {
	.name = "adapted",
	.bgscan = true,
	.fast_transition = true,
	.evaluate = adapted_evaluate,
	.select = adapted_select,
};
