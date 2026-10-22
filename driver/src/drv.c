/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <string.h>

#include "fh/fh_drv.h"

#define SCAN_BUDGET_MS  40   /* off-channel time per window */
#define SCAN_WINDOW_MS  500
#define SCAN_MAX_AGE_MS 2000
#define SCAN_DWELL_EST  20
#define KEY_LIFETIME_MS 30000
#define DEGRADED_RETRY  150  /* permille */
#define DEGRADED_SNR    12

static const uint8_t band_channels[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

void fh_drv_cfg_default(struct fh_drv_cfg *cfg, const struct fh_roam_ops *roam)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->roam = roam;
	cfg->base_dbm = -75;
	cfg->bgscan_dbm = -65;
	cfg->prep_dbm = -70;
	cfg->roam_dbm = -72;
	cfg->emergency_dbm = -85;
	cfg->hyst_db = 5;
	cfg->load_weight_db = 10;
	cfg->beacon_loss_n = 7;
	cfg->ttt_ms = 240;
	cfg->horizon_ms = 1000;
	cfg->holdoff_ms = 1000;
	cfg->fresh_ms = 600;
	cfg->pingpong_ms = 5000;
}

int fh_drv_init(struct fh_drv *d, const struct fh_drv_cfg *cfg, const struct fh_hw_ops *hw, void *hw_priv,
		const struct fh_ap_info *aps, int n_ap, uint64_t sta)
{
	if (!cfg || !cfg->roam || !hw || !aps || n_ap <= 0 || n_ap > FH_MAX_APS)
		return -FH_EINVAL;
	memset(d, 0, sizeof(*d));
	d->cfg = *cfg;
	d->hw = hw;
	d->hw_priv = hw_priv;
	memcpy(d->ap, aps, sizeof(*aps) * (size_t)n_ap);
	d->n_ap = (uint8_t)n_ap;
	d->sta = sta;
	fh_sm_init(&d->sm, 0);
	fh_scan_init(&d->scan, SCAN_BUDGET_MS, SCAN_WINDOW_MS, SCAN_MAX_AGE_MS);
	fh_kc_init(&d->kc, sta, fh_hash32(&sta, sizeof(sta), 0x5eedu), KEY_LIFETIME_MS);
	fh_evring_init(&d->ev);
	fh_evring_subscribe(&d->ev, FH_EVGRP_ALL);
	d->cur = d->prev = d->target = FH_AP_NONE;
	return 0;
}

static void post(struct fh_drv *d, uint8_t type, fh_ms_t now, fh_ap_t from, fh_ap_t to, uint8_t flags,
		 uint16_t eta_ms)
{
	struct fh_event ev = {
		.type = type,
		.flags = flags,
		.from = from,
		.to = to,
		.ts = now,
		.eta_ms = eta_ms,
		.rssi = (int16_t)fh_link_rssi(&d->link),
		.retry_permille = (uint16_t)fh_link_retry_permille(&d->link),
		.subnet = to < d->n_ap ? d->ap[to].subnet : 0,
	};

	if (type == FH_EV_HANDOFF_DONE)
		ev.lat = d->lat;
	(void)fh_evring_post(&d->ev, &ev);
}

static void refresh_neighbours(struct fh_drv *d)
{
	fh_ap_t nb[FH_MAX_APS];
	uint8_t ch[FH_MAX_APS];

	if (!d->cfg.roam->bgscan || !d->hw->neighbor_report)
		return;
	int n = d->hw->neighbor_report(d->hw_priv, d->cur, nb, FH_MAX_APS);
	int k = 0;

	for (int i = 0; i < n; i++)
		if (nb[i] < d->n_ap)
			ch[k++] = d->ap[nb[i]].chan;
	fh_scan_set_channels(&d->scan, ch, k);
}

int fh_drv_associate(struct fh_drv *d, fh_ap_t ap, fh_ms_t now, int rssi_dbm)
{
	if (ap >= d->n_ap || fh_sm_set(&d->sm, FH_ST_CONNECTED, now))
		return -FH_EINVAL;
	d->cur = ap;
	d->prev = FH_AP_NONE;
	fh_link_reset(&d->link, now, rssi_dbm);
	/* the initial full authentication leaves a cached key for this AP */
	fh_kc_install(&d->kc, d->ap[ap].bssid, now, now);
	refresh_neighbours(d);
	return 0;
}

bool fh_drv_associated(const struct fh_drv *d)
{
	return fh_state_associated(d->sm.state);
}

void fh_drv_rx(struct fh_drv *d, fh_ms_t now, int rssi_dbm, bool first_try_ok)
{
	if (!fh_drv_associated(d))
		return;
	fh_link_rx(&d->link, now, rssi_dbm);
	fh_link_tx(&d->link, first_try_ok);
}

void fh_drv_beacon(struct fh_drv *d, fh_ms_t now, bool received, int rssi_dbm)
{
	if (fh_drv_associated(d))
		fh_link_beacon(&d->link, now, received, rssi_dbm);
}

void fh_drv_build_ctx(const struct fh_drv *d, fh_ms_t now, struct fh_pred_ctx *c)
{
	fh_pred_ctx_clear(c, d->prev, d->cur);
	for (int i = 0; i < d->n_ap; i++) {
		const struct fh_cand *cand = fh_scan_get(&d->scan, (fh_ap_t)i, now, d->scan.max_age_ms);

		if (cand) {
			c->rssi[i] = cand->rssi;
			c->slope[i] = (int16_t)fh_cand_slope(cand);
		}
	}
	if (d->cur < d->n_ap) {
		c->rssi[d->cur] = (int16_t)fh_link_rssi(&d->link);
		c->slope[d->cur] = (int16_t)lround(fh_kf_slope(&d->link.kf));
	}
}

/* Capture the context and rank the likely next APs. */
static void predict_next(struct fh_drv *d, fh_ms_t now)
{
	fh_drv_build_ctx(d, now, &d->ctx);
	d->ctx_valid = true;
	if (d->cfg.pred)
		d->n_predicted = (uint8_t)d->cfg.pred->ops->rank(d->cfg.pred, &d->ctx, d->predicted, 3);
	else
		d->n_predicted = (uint8_t)fh_pred_strongest.rank(NULL, &d->ctx, d->predicted, 3);
}

void fh_drv_preauth(struct fh_drv *d, fh_ms_t now, fh_ap_t ap)
{
	if (ap >= d->n_ap || ap == d->cur || !d->hw->preauth || fh_kc_present(&d->kc, d->ap[ap].bssid, now))
		return;
	uint16_t ms = d->hw->preauth(d->hw_priv, now, ap);

	fh_kc_install(&d->kc, d->ap[ap].bssid, now, now + ms);
	d->st.preauths++;
}

static void do_prepare(struct fh_drv *d, fh_ms_t now, const struct fh_decision *dec)
{
	uint8_t ch[3];

	if (fh_sm_set(&d->sm, FH_ST_ROAM_PREP, now))
		return;
	predict_next(d, now);
	for (int i = 0; i < d->n_predicted; i++)
		ch[i] = d->ap[d->predicted[i]].chan;
	fh_scan_prioritise(&d->scan, ch, d->n_predicted);
	if (d->cfg.roam->fast_transition)
		for (int i = 0; i < d->n_predicted && i < 2; i++)
			fh_drv_preauth(d, now, d->predicted[i]);
	d->ttt_start = 0;
	d->recover_start = 0;
	d->st.imminent++;
	post(d, FH_EV_HANDOFF_IMMINENT, now, d->cur, d->n_predicted ? d->predicted[0] : FH_AP_NONE, 0,
	     dec->eta_ms);
}

static uint16_t probe_channel(struct fh_drv *d, fh_ms_t now, uint8_t chan)
{
	struct fh_scan_report rep[FH_MAX_APS];
	int n = 0;
	uint16_t ms = d->hw->probe(d->hw_priv, now, chan, rep, FH_MAX_APS, &n);

	for (int i = 0; i < n; i++)
		fh_scan_update(&d->scan, now, rep[i].ap, chan, rep[i].rssi, rep[i].load_pct);
	return ms;
}

static void start_handoff(struct fh_drv *d, fh_ms_t now, const struct fh_decision *dec)
{
	uint8_t ch[FH_MAX_CHANNELS];
	int n = 0;

	if (!d->ctx_valid)
		predict_next(d, now);
	d->target = dec->target;
	d->ho_start = now;
	memset(&d->lat, 0, sizeof(d->lat));
	d->ho_flags = dec->reactive ? FH_EVF_REACTIVE : 0;

	if (!d->cfg.roam->bgscan) {
		/* classic reactive client: sweep the whole band */
		memcpy(ch, band_channels, sizeof(band_channels));
		n = (int)sizeof(band_channels);
	} else if (dec->target < d->n_ap && fh_scan_get(&d->scan, dec->target, now, d->cfg.fresh_ms)) {
		/* measurement is recent: one confirming probe on the target channel */
		ch[n++] = d->ap[dec->target].chan;
	} else {
		memcpy(ch, d->scan.chan, d->scan.n_chan);
		n = d->scan.n_chan;
		if (n == 0) {
			memcpy(ch, band_channels, sizeof(band_channels));
			n = (int)sizeof(band_channels);
		}
	}

	/* remember the old AP's last level so the candidate cache stays complete */
	if (d->cur < d->n_ap)
		fh_scan_update(&d->scan, now, d->cur, d->ap[d->cur].chan, fh_link_rssi(&d->link),
			       d->scan.cache[d->cur].load_pct);

	fh_sm_set(&d->sm, FH_ST_SCANNING, now);
	post(d, FH_EV_HANDOFF_START, now, d->cur, dec->target, d->ho_flags, 0);

	uint32_t el = 0;

	for (int i = 0; i < n; i++)
		el += probe_channel(d, now, ch[i]);
	d->lat.scan_ms = (uint16_t)el;
	d->phase_end = now + el;
	d->last_scan = now;
	d->ttt_start = 0;
}

static void finish(struct fh_drv *d, fh_ms_t now)
{
	fh_ap_t from = d->cur, to = d->target;
	const struct fh_cand *c = fh_scan_get(&d->scan, to, now, UINT16_MAX);
	uint8_t flags = d->ho_flags;

	fh_sm_set(&d->sm, FH_ST_CONNECTED, now);
	d->prev = from;
	d->cur = to;
	fh_link_reset(&d->link, now, c ? c->rssi : FH_RSSI_FLOOR);
	if (d->key_mode == FH_KEY_FULL_EAP) {
		fh_kc_install(&d->kc, d->ap[to].bssid, now, now);
		d->st.full_eap++;
	}
	if (d->key_mode == FH_KEY_FT) {
		flags |= FH_EVF_FT;
		d->st.ft_used++;
	}
	if (d->ap[from].subnet != d->ap[to].subnet)
		flags |= FH_EVF_SUBNET_CHANGE;
	d->st.handoffs++;
	if (d->ho_flags & FH_EVF_REACTIVE)
		d->st.reactive++;

	/* A -> B followed quickly by B -> A */
	if (d->hist_n > 0) {
		const unsigned last = (d->hist_n - 1u) % FH_HO_HIST;

		if (d->hist[last].from == to && d->hist[last].to == from && now - d->hist[last].ts <= d->cfg.pingpong_ms)
			d->st.pingpongs++;
	}
	d->hist[d->hist_n % FH_HO_HIST].from = from;
	d->hist[d->hist_n % FH_HO_HIST].to = to;
	d->hist[d->hist_n % FH_HO_HIST].ts = now;
	d->hist_n++;

	/* online learning: the driver trains its own predictor */
	if (d->cfg.pred && d->ctx_valid)
		d->cfg.pred->ops->observe(d->cfg.pred, &d->ctx, to);

	post(d, FH_EV_HANDOFF_DONE, now, from, to, flags, 0);
	d->ctx_valid = false;
	d->n_predicted = 0;
	d->ttt_start = 0;
	d->recover_start = 0;
	refresh_neighbours(d);
}

static void phase_step(struct fh_drv *d, fh_ms_t now)
{
	if (now < d->phase_end)
		return;

	switch (d->sm.state) {
	case FH_ST_SCANNING: {
		fh_ap_t t = d->cfg.roam->select(d, now);

		if (t >= d->n_ap || t == d->cur) {
			fh_sm_set(&d->sm, FH_ST_CONNECTED, now);
			d->st.aborted++;
			d->ctx_valid = false;
			d->n_predicted = 0;
			post(d, FH_EV_HANDOFF_DONE, now, d->cur, d->cur, d->ho_flags | FH_EVF_ABORTED, 0);
			return;
		}
		d->target = t;
		bool cached = fh_kc_lookup(&d->kc, d->ap[t].bssid, now);

		d->key_mode = cached ? (d->cfg.roam->fast_transition ? FH_KEY_FT : FH_KEY_4WAY) : FH_KEY_FULL_EAP;
		fh_sm_set(&d->sm, FH_ST_AUTH, now);
		d->lat.auth_ms = d->hw->auth(d->hw_priv, now, t, d->key_mode == FH_KEY_FT);
		d->phase_end = now + d->lat.auth_ms;
		break;
	}
	case FH_ST_AUTH:
		fh_sm_set(&d->sm, FH_ST_REASSOC, now);
		d->lat.reassoc_ms = d->hw->reassoc(d->hw_priv, now, d->target);
		d->phase_end = now + d->lat.reassoc_ms;
		break;
	case FH_ST_REASSOC:
		fh_sm_set(&d->sm, FH_ST_KEY, now);
		d->lat.key_ms = d->hw->key(d->hw_priv, now, d->target, d->key_mode);
		d->phase_end = now + d->lat.key_ms;
		break;
	case FH_ST_KEY:
		finish(d, now);
		break;
	default:
		break;
	}
}

void fh_drv_tick(struct fh_drv *d, fh_ms_t now)
{
	if (d->sm.state == FH_ST_DISCONNECTED)
		return;
	if (!fh_drv_associated(d)) {
		phase_step(d, now);
		return;
	}

	fh_scan_age(&d->scan, now);

	if ((fh_link_retry_permille(&d->link) > DEGRADED_RETRY || fh_link_snr(&d->link) < DEGRADED_SNR) &&
	    now - d->last_degraded >= 1000) {
		d->last_degraded = now;
		d->st.degraded++;
		post(d, FH_EV_LINK_DEGRADED, now, d->cur, FH_AP_NONE, 0, 0);
	}

	struct fh_decision dec = { .verdict = FH_STAY, .target = FH_AP_NONE };

	d->cfg.roam->evaluate(d, now, &dec);
	switch (dec.verdict) {
	case FH_PREPARE:
		do_prepare(d, now, &dec);
		break;
	case FH_CANCEL:
		if (!fh_sm_set(&d->sm, FH_ST_CONNECTED, now)) {
			d->st.cancelled++;
			d->ctx_valid = false;
			d->n_predicted = 0;
		}
		break;
	case FH_GO:
		start_handoff(d, now, &dec);
		return;
	case FH_STAY:
		break;
	}

	/* background scan: one channel at a time, within the off-channel budget */
	if (d->cfg.roam->bgscan && now >= d->offchan_until &&
	    (d->sm.state == FH_ST_ROAM_PREP || fh_link_rssi(&d->link) < d->cfg.bgscan_dbm)) {
		int ch = fh_scan_next(&d->scan, now, SCAN_DWELL_EST);

		if (ch > 0) {
			uint16_t ms = probe_channel(d, now, (uint8_t)ch);

			fh_scan_charge(&d->scan, ms);
			d->offchan_until = now + ms;
		}
	}
}
