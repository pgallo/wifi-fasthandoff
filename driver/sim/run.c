/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fh_run.h"

#define PKT_PERIOD_MS    20  /* 50 packets/s downlink, a voice-like flow */
#define BEACON_PERIOD_MS 100
#define LINK_SAMPLE_MS   250
#define RETRY_LIMIT      4
#define HEAR_DBM         (-92)
#define GT_DELAY_MS      1000
#define MAX_RUN_MS       2000000u

void fh_model_init(struct fh_model *m, const struct fh_pred_ops *primary)
{
	fh_pred_init(&m->drv, primary);
	for (int k = 0; k < FH_PRED__COUNT; k++)
		fh_pred_init(&m->eval[k], fh_pred_table[k]);
}

/* Goodput the link offers, for the content layer's trace. */
static unsigned rate_kbps(double snr, unsigned load_pct)
{
	static const struct {
		double snr;
		unsigned kbps;
	} table[] = { { 25, 54000 }, { 22, 48000 }, { 18, 36000 }, { 14, 24000 },
		      { 10, 12000 }, { 7, 6000 },   { 4, 2000 } };

	for (size_t i = 0; i < FH_ARRAY_SIZE(table); i++)
		if (snr >= table[i].snr)
			return table[i].kbps / 2u * (100u - load_pct) / 100u; /* MAC efficiency ~50% */
	return 0;
}

static int ground_truth(const struct fh_world *w, unsigned load_weight)
{
	int best = 0;
	double bs = -1e9;

	for (int i = 0; i < w->n_ap; i++) {
		double s = fh_world_mean_rssi(w, i) - load_weight * w->ap[i].load_pct / 100.0;

		if (s > bs) {
			bs = s;
			best = i;
		}
	}
	return best;
}

static void trace_event(FILE *f, const struct fh_event *ev)
{
	fprintf(f, "{\"type\":\"%s\",\"t\":%u,\"from\":%d,\"to\":%d,\"rssi\":%d", fh_ev_name(ev->type), ev->ts,
		ev->from == FH_AP_NONE ? -1 : ev->from, ev->to == FH_AP_NONE ? -1 : ev->to, ev->rssi);
	switch (ev->type) {
	case FH_EV_HANDOFF_IMMINENT:
		fprintf(f, ",\"eta_ms\":%u", ev->eta_ms);
		break;
	case FH_EV_LINK_DEGRADED:
		fprintf(f, ",\"retry_permille\":%u", ev->retry_permille);
		break;
	case FH_EV_HANDOFF_DONE:
		fprintf(f,
			",\"subnet\":%u,\"subnet_changed\":%s,\"aborted\":%s,\"ft\":%s,"
			"\"latency\":{\"scan\":%u,\"auth\":%u,\"reassoc\":%u,\"key\":%u,\"total\":%u}",
			ev->subnet, (ev->flags & FH_EVF_SUBNET_CHANGE) ? "true" : "false",
			(ev->flags & FH_EVF_ABORTED) ? "true" : "false", (ev->flags & FH_EVF_FT) ? "true" : "false",
			ev->lat.scan_ms, ev->lat.auth_ms, ev->lat.reassoc_ms, ev->lat.key_ms, fh_ho_total(&ev->lat));
		break;
	default:
		break;
	}
	fputs("}\n", f);
}

struct pending {
	fh_ms_t due;
	fh_ap_t top1[FH_PRED__COUNT];
	bool used;
};

void fh_run(const struct fh_run_cfg *cfg, struct fh_model *m, struct fh_run_stats *st)
{
	struct fh_world w;
	struct fh_ap_info aps[FH_MAX_APS];
	struct fh_drv_cfg dcfg;
	struct fh_drv *d = calloc(1, sizeof(*d));
	struct pending pend[8] = { 0 };
	fh_ms_t losses[64];
	unsigned n_loss = 0;
	bool in_ho = false;
	const bool predictive = cfg->algo->fast_transition;

	memset(st, 0, sizeof(*st));
	if (!d)
		return;
	fh_world_init(&w, cfg->sc, cfg->layout_seed, cfg->walk_seed);
	fh_world_ap_info(&w, aps);
	struct fh_simhw hw = { .w = &w, .seed = fh_mix64(cfg->walk_seed ^ 0xA11CEull) };
	const uint64_t pkt_seed = fh_mix64(cfg->walk_seed ^ 0xDA7Aull);

	fh_drv_cfg_default(&dcfg, cfg->algo);
	dcfg.pred = predictive ? &m->drv : NULL;
	fh_drv_init(d, &dcfg, &fh_simhw_ops, &hw, aps, w.n_ap, 0x02AA00000001ull);

	int first = ground_truth(&w, 0);

	fh_drv_associate(d, (fh_ap_t)first, 0, (int)lround(fh_world_mean_rssi(&w, first)));
	fh_scan_update(&d->scan, 0, (fh_ap_t)first, aps[first].chan, (int)lround(fh_world_mean_rssi(&w, first)),
		       w.ap[first].load_pct);

	if (cfg->trace)
		fprintf(cfg->trace,
			"{\"type\":\"META\",\"scenario\":\"%s\",\"algo\":\"%s\",\"layout_seed\":%llu,"
			"\"walk_seed\":%llu,\"n_ap\":%d,\"ap\":%d,\"subnet\":%u}\n",
			fh_scenario_name(cfg->sc), cfg->algo->name, (unsigned long long)cfg->layout_seed,
			(unsigned long long)cfg->walk_seed, w.n_ap, first, aps[first].subnet);

	fh_ms_t t;

	for (t = 0; !w.done && t < MAX_RUN_MS; t++) {
		if (t % 10 == 0)
			fh_world_step(&w, 0.010);
		bool up = fh_drv_associated(d);

		if (!up)
			st->outage_ms++;

		if (t % PKT_PERIOD_MS == 0) {
			bool ok = false;

			st->pkts++;
			if (up) {
				int r = fh_world_rssi(&w, d->cur, t, FH_K_DATA);
				double per = r >= HEAR_DBM ? fh_per(r - FH_NOISE_DBM) : 1.0;
				bool first_ok = fh_crn_unit(pkt_seed, d->cur, t, 0) >= per;

				ok = first_ok;
				for (uint32_t a = 1; !ok && a <= RETRY_LIMIT; a++)
					ok = fh_crn_unit(pkt_seed, d->cur, t, a) >= per;
				if (r >= HEAR_DBM)
					fh_drv_rx(d, t, r, first_ok);
			}
			if (!ok) {
				st->lost++;
				losses[n_loss++ % FH_ARRAY_SIZE(losses)] = t;
				if (in_ho)
					st->lost_ho++;
			}
		}
		if (up && t % BEACON_PERIOD_MS == 50) {
			int r = fh_world_rssi(&w, d->cur, t, FH_K_BEACON);
			bool rx = r >= HEAR_DBM && fh_crn_unit(pkt_seed, d->cur, t, 99) >= fh_per(r - FH_NOISE_DBM + 4);

			fh_drv_beacon(d, t, rx, r);
		}

		fh_drv_tick(d, t);

		/* user-space side: drain the event channel */
		struct fh_event ev;
		int rc;

		while ((rc = fh_evring_recv(&d->ev, &ev)) != -FH_EAGAIN) {
			if (rc == -FH_ENOBUFS) {
				st->ev_dropped++;
				continue;
			}
			st->ev_seen++;
			if (cfg->trace)
				trace_event(cfg->trace, &ev);
			if (ev.type == FH_EV_HANDOFF_START) {
				in_ho = true;
				/* losses in the second before the break count against the handoff */
				for (unsigned i = 0; i < n_loss && i < FH_ARRAY_SIZE(losses); i++)
					if (losses[i] + 1000 >= ev.ts && losses[i] < ev.ts)
						st->lost_ho++;
			} else if (ev.type == FH_EV_HANDOFF_DONE) {
				in_ho = false;
				if (ev.to == ev.from)
					continue;
				if (st->n_lat < FH_RUN_MAX_LAT)
					st->lat[st->n_lat++] = ev.lat;
				if (!predictive)
					continue;
				/* d->ctx still holds the context captured for this handoff */
				struct pending *p = NULL;

				for (size_t i = 0; i < FH_ARRAY_SIZE(pend); i++)
					if (!pend[i].used)
						p = &pend[i];
				for (int k = 0; k < FH_PRED__COUNT; k++) {
					fh_ap_t top = FH_AP_NONE;

					m->eval[k].ops->rank(&m->eval[k], &d->ctx, &top, 1);
					if (p)
						p->top1[k] = top;
					m->eval[k].ops->observe(&m->eval[k], &d->ctx, ev.to);
				}
				if (p) {
					p->due = ev.ts + GT_DELAY_MS;
					p->used = true;
				}
			}
		}

		for (size_t i = 0; i < FH_ARRAY_SIZE(pend); i++) {
			if (!pend[i].used || pend[i].due != t)
				continue;
			int gt = ground_truth(&w, dcfg.load_weight_db);

			st->pred_n++;
			for (int k = 0; k < FH_PRED__COUNT; k++)
				st->pred_hit[k] += pend[i].top1[k] == gt;
			pend[i].used = false;
		}

		if (cfg->trace && t % LINK_SAMPLE_MS == 0) {
			bool assoc = fh_drv_associated(d);
			double mean = assoc ? fh_world_mean_rssi(&w, d->cur) : FH_RSSI_FLOOR;

			fprintf(cfg->trace, "{\"type\":\"LINK\",\"t\":%u,\"up\":%s,\"ap\":%d,\"rssi\":%d,\"rate_kbps\":%u}\n",
				t, assoc ? "true" : "false", assoc ? d->cur : -1, (int)lround(mean),
				assoc ? rate_kbps(mean - FH_NOISE_DBM, w.ap[d->cur].load_pct) : 0u);
		}
	}

	st->duration_ms = t;
	st->handoffs = d->st.handoffs;
	st->pingpongs = d->st.pingpongs;
	st->aborted = d->st.aborted;
	st->reactive = d->st.reactive;
	st->ft_used = d->st.ft_used;
	st->full_eap = d->st.full_eap;
	st->preauths = d->st.preauths;
	st->offchan_ms = d->scan.offchan_total_ms;
	if (cfg->trace)
		fprintf(cfg->trace, "{\"type\":\"END\",\"t\":%u}\n", t);
	free(d);
}

void fh_train(enum fh_scenario sc, uint64_t layout_seed, int n_walks, struct fh_model *m)
{
	struct fh_run_stats *st = malloc(sizeof(*st));

	if (!st)
		return;
	for (int i = 0; i < n_walks; i++) {
		struct fh_run_cfg rc = {
			.sc = sc,
			.layout_seed = layout_seed,
			.walk_seed = fh_mix64(layout_seed * 1000003u + 7919u * (uint64_t)(i + 1)),
			.algo = &fh_roam_adapted,
		};

		fh_run(&rc, m, st);
	}
	free(st);
}
