/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "fh/fh_drv.h"
#include "fh_test.h"

/* Scripted radio: fixed levels per AP, fixed air times per phase. */
struct fake {
	int rssi[FH_MAX_APS];
	const struct fh_ap_info *aps;
	int n_ap;
	int probes;
};

#define PROBE_MS   15
#define AUTH_MS    4
#define REASSOC_MS 5
#define KEY_4WAY   12
#define KEY_EAP    110
#define PREAUTH_MS 50

static uint16_t f_probe(void *hw, fh_ms_t now, uint8_t chan, struct fh_scan_report *rep, int max, int *n)
{
	struct fake *f = hw;

	(void)now;
	*n = 0;
	f->probes++;
	for (int i = 0; i < f->n_ap && *n < max; i++)
		if (f->aps[i].chan == chan && f->rssi[i] > -90) {
			rep[*n].ap = (fh_ap_t)i;
			rep[*n].rssi = (int16_t)f->rssi[i];
			rep[*n].load_pct = 0;
			(*n)++;
		}
	return PROBE_MS;
}

static uint16_t f_auth(void *hw, fh_ms_t now, fh_ap_t ap, bool ft)
{
	(void)hw, (void)now, (void)ap;
	return ft ? AUTH_MS + 1 : AUTH_MS;
}

static uint16_t f_reassoc(void *hw, fh_ms_t now, fh_ap_t ap)
{
	(void)hw, (void)now, (void)ap;
	return REASSOC_MS;
}

static uint16_t f_key(void *hw, fh_ms_t now, fh_ap_t ap, enum fh_key_mode mode)
{
	(void)hw, (void)now, (void)ap;
	return mode == FH_KEY_FT ? 1 : (mode == FH_KEY_4WAY ? KEY_4WAY : KEY_EAP);
}

static uint16_t f_preauth(void *hw, fh_ms_t now, fh_ap_t ap)
{
	(void)hw, (void)now, (void)ap;
	return PREAUTH_MS;
}

static int f_neighbors(void *hw, fh_ap_t ap, fh_ap_t *out, int max)
{
	struct fake *f = hw;
	int n = 0;

	for (int i = 0; i < f->n_ap && n < max; i++)
		if (i != ap)
			out[n++] = (fh_ap_t)i;
	return n;
}

static const struct fh_hw_ops fake_ops = {
	.probe = f_probe,
	.auth = f_auth,
	.reassoc = f_reassoc,
	.key = f_key,
	.preauth = f_preauth,
	.neighbor_report = f_neighbors,
};

static const struct fh_ap_info aps[3] = {
	{ .bssid = 0xA0, .chan = 1, .subnet = 0 },
	{ .bssid = 0xA1, .chan = 6, .subnet = 0 },
	{ .bssid = 0xA2, .chan = 11, .subnet = 1 },
};

static struct fh_drv *setup(const struct fh_roam_ops *roam, struct fake *f, struct fh_predictor *pred)
{
	struct fh_drv *d = calloc(1, sizeof(*d));
	struct fh_drv_cfg cfg;

	memset(f, 0, sizeof(*f));
	for (int i = 0; i < FH_MAX_APS; i++)
		f->rssi[i] = -100;
	f->aps = aps;
	f->n_ap = 3;
	fh_drv_cfg_default(&cfg, roam);
	cfg.pred = pred;
	CHECK_EQ(fh_drv_init(d, &cfg, &fake_ops, f, aps, 3, 0x02AA), 0);
	CHECK_EQ(fh_drv_associate(d, 0, 0, -60), 0);
	return d;
}

/* Drive the data path at a level that follows level(t); collect events. */
struct trace {
	struct fh_event ev[64];
	int n;
};

static void drive(struct fh_drv *d, fh_ms_t from, fh_ms_t to, double (*level)(fh_ms_t), struct trace *tr)
{
	struct fh_event ev;

	for (fh_ms_t t = from; t < to; t++) {
		/* the scripted level applies to AP 0; after a handoff the new AP is strong */
		if (t % 20 == 0 && fh_drv_associated(d))
			fh_drv_rx(d, t, d->cur == 0 ? (int)level(t) : -55, true);
		fh_drv_tick(d, t);
		while (fh_evring_recv(&d->ev, &ev) == 0)
			if (tr->n < 64)
				tr->ev[tr->n++] = ev;
	}
}

static const struct fh_event *find(const struct trace *tr, uint8_t type)
{
	for (int i = 0; i < tr->n; i++)
		if (tr->ev[i].type == type)
			return &tr->ev[i];
	return NULL;
}

static double weak(fh_ms_t t)
{
	(void)t;
	return -80.0;
}

static double fading_away(fh_ms_t t)
{
	return -60.0 - 5.0 * t / 1000.0; /* -5 dB/s */
}

static double slow_fade(fh_ms_t t)
{
	return -60.0 - 2.0 * t / 1000.0; /* -2 dB/s, walking pace */
}

static void test_baseline_full_scan_latency(void)
{
	struct fake f;
	struct fh_drv *d = setup(&fh_roam_baseline, &f, NULL);
	struct trace tr = { .n = 0 };

	f.rssi[1] = -55;
	drive(d, 0, 3000, weak, &tr);

	const struct fh_event *start = find(&tr, FH_EV_HANDOFF_START), *done = find(&tr, FH_EV_HANDOFF_DONE);

	CHECK(start && done);
	CHECK(find(&tr, FH_EV_HANDOFF_IMMINENT) == NULL);
	if (start && done) {
		CHECK_EQ(done->to, 1);
		CHECK_EQ(done->lat.scan_ms, 11 * PROBE_MS); /* whole band */
		CHECK_EQ(done->lat.auth_ms, AUTH_MS);
		CHECK_EQ(done->lat.reassoc_ms, REASSOC_MS);
		CHECK_EQ(done->lat.key_ms, KEY_EAP); /* never seen this AP: full EAP */
		CHECK_EQ(done->ts - start->ts, fh_ho_total(&done->lat));
		CHECK(start->ts >= 1000); /* holdoff after association */
	}
	CHECK_EQ(d->cur, 1);
	CHECK_EQ(d->sm.state, FH_ST_CONNECTED);
	CHECK_EQ(d->st.full_eap, 1);
	free(d);
}

static void test_adapted_predicts_preauths_and_uses_ft(void)
{
	struct fake f;
	struct fh_drv *d = setup(&fh_roam_adapted, &f, NULL);
	struct trace tr = { .n = 0 };
	uint32_t transitions_before = d->sm.transitions;

	f.rssi[1] = -55;
	f.rssi[2] = -80;
	drive(d, 0, 9000, slow_fade, &tr);

	const struct fh_event *imm = find(&tr, FH_EV_HANDOFF_IMMINENT);
	const struct fh_event *start = find(&tr, FH_EV_HANDOFF_START);
	const struct fh_event *done = find(&tr, FH_EV_HANDOFF_DONE);

	CHECK(imm && start && done);
	if (imm && start && done) {
		CHECK(imm->ts < start->ts);
		CHECK_EQ(imm->to, 1);
		CHECK(imm->eta_ms > 0);
		CHECK_EQ(done->to, 1);
		CHECK_EQ(done->lat.scan_ms, PROBE_MS);  /* one confirming probe */
		CHECK_EQ(done->lat.key_ms, 1);          /* cached key */
		CHECK(done->flags & FH_EVF_FT);
		CHECK(!(done->flags & FH_EVF_SUBNET_CHANGE));
		CHECK_EQ(done->ts - start->ts, fh_ho_total(&done->lat));
		/* it moves while the serving AP is still usable */
		CHECK(start->rssi > -80);
	}
	CHECK(d->st.preauths >= 1);
	/* CONNECTED -> ROAM_PREP -> SCANNING -> AUTH -> REASSOC -> KEY -> CONNECTED */
	CHECK_EQ(d->sm.transitions - transitions_before, 6);
	free(d);
}

/* Put the driver in ROAM_PREP with a steady serving level and no slope. */
static struct fh_drv *prep_at(struct fake *f, int serving_dbm, fh_ms_t now)
{
	struct fh_drv *d = setup(&fh_roam_adapted, f, NULL);

	fh_link_reset(&d->link, 0, serving_dbm);
	for (fh_ms_t t = 20; t <= now; t += 20)
		fh_link_rx(&d->link, t, serving_dbm);
	fh_sm_set(&d->sm, FH_ST_ROAM_PREP, now);
	return d;
}

static void set_cand(struct fh_drv *d, fh_ap_t ap, int rssi, fh_ms_t now)
{
	struct fh_cand *c = &d->scan.cache[ap];

	c->valid = true;
	c->rssi = c->prev_rssi = (int16_t)rssi;
	c->seen = c->prev_seen = now;
	c->chan = aps[ap].chan;
	c->load_pct = 0;
}

static enum fh_verdict eval(struct fh_drv *d, fh_ms_t now, struct fh_decision *dec)
{
	memset(dec, 0, sizeof(*dec));
	dec->target = FH_AP_NONE;
	fh_roam_adapted.evaluate(d, now, dec);
	return dec->verdict;
}

static void test_hysteresis_blocks_small_gain(void)
{
	struct fake f;
	struct fh_decision dec;
	struct fh_drv *d = prep_at(&f, -75, 10000);

	for (fh_ms_t t = 10000; t < 12000; t += 10) {
		set_cand(d, 1, -71, t); /* 4 dB better: below the 5 dB margin */
		CHECK(eval(d, t, &dec) != FH_GO);
	}
	set_cand(d, 1, -70, 12000); /* exactly the margin */
	CHECK(eval(d, 12000, &dec) != FH_GO);
	CHECK(eval(d, 12000 + 240, &dec) == FH_GO);
	CHECK_EQ(dec.target, 1);
	free(d);
}

static void test_time_to_trigger_and_reset(void)
{
	struct fake f;
	struct fh_decision dec;
	struct fh_drv *d = prep_at(&f, -75, 10000);

	set_cand(d, 1, -60, 10000);
	CHECK(eval(d, 10000, &dec) == FH_STAY); /* condition starts holding, pre-auth starts */
	CHECK(fh_kc_present(&d->kc, aps[1].bssid, 10000));
	CHECK(eval(d, 10200, &dec) == FH_STAY);
	set_cand(d, 1, -78, 10210); /* candidate drops: time-to-trigger restarts */
	CHECK(eval(d, 10210, &dec) == FH_STAY);
	set_cand(d, 1, -60, 10220);
	CHECK(eval(d, 10220, &dec) == FH_STAY);
	CHECK(eval(d, 10459, &dec) == FH_STAY);
	CHECK(eval(d, 10460, &dec) == FH_GO);
	free(d);
}

static void test_waits_for_preauth_key(void)
{
	struct fake f;
	struct fh_decision dec;
	struct fh_drv *d = prep_at(&f, -75, 10000);

	f.rssi[1] = -60;
	set_cand(d, 1, -60, 10000);
	eval(d, 10000, &dec);
	/* make before break: the key is not ready yet, so even after ttt wait */
	for (int i = 0; i < FH_KC_SLOTS; i++)
		if (d->kc.slot[i].bssid == aps[1].bssid)
			d->kc.slot[i].ready_at = 10400;
	CHECK(eval(d, 10300, &dec) == FH_STAY);
	CHECK(eval(d, 10400, &dec) == FH_GO);
	free(d);
}

static void test_pingpong_margin_and_dwell(void)
{
	struct fake f;
	struct fh_decision dec;
	struct fh_drv *d = prep_at(&f, -75, 10000);

	/* we came from AP 1 a moment ago */
	d->prev = 1;
	d->hist[0].from = 1;
	d->hist[0].to = 0;
	d->hist[0].ts = 9000;
	d->hist_n = 1;
	set_cand(d, 1, -60, 10000);
	CHECK(eval(d, 10000, &dec) == FH_STAY);
	CHECK(eval(d, 10900, &dec) == FH_STAY); /* inside the minimum dwell */

	d->hist[0].ts = 7000; /* dwell over, still inside the ping-pong window */
	set_cand(d, 1, -65, 10900);
	CHECK(eval(d, 10900, &dec) == FH_STAY);
	CHECK(eval(d, 11300, &dec) == FH_STAY); /* 10 dB is not enough to go back */
	set_cand(d, 1, -63, 11300);
	CHECK(eval(d, 11300, &dec) == FH_STAY);
	CHECK(eval(d, 11600, &dec) == FH_GO); /* 12 dB is */
	free(d);
}

static void test_beacon_loss_forces_reactive_handoff(void)
{
	struct fake f;
	struct fh_decision dec;
	struct fh_drv *d = prep_at(&f, -70, 10000);

	fh_sm_set(&d->sm, FH_ST_CONNECTED, 10000);
	for (int i = 0; i < 7; i++)
		fh_drv_beacon(d, 10000 + 100 * (fh_ms_t)i, false, 0);
	CHECK(eval(d, 10700, &dec) == FH_GO);
	CHECK(dec.reactive);
	free(d);
}

static void test_eta_from_slope(void)
{
	struct fake f;
	struct fh_decision dec;
	struct fh_drv *d = setup(&fh_roam_adapted, &f, NULL);

	fh_link_reset(&d->link, 0, -60);
	fh_ms_t t;

	/* -5 dB/s: around -68 the forecast one second ahead is below -70 */
	for (t = 20; t <= 1800; t += 20)
		fh_link_rx(&d->link, t, (int)fading_away(t));
	CHECK(eval(d, t, &dec) == FH_PREPARE);
	/* (rssi - roam_dbm) / slope: about (-68 + 72) / 5 s, the filter lags a little */
	printf("    eta %u ms, slope %.2f dB/s\n", dec.eta_ms, fh_kf_slope(&d->link.kf));
	CHECK(dec.eta_ms >= 400 && dec.eta_ms <= 2000);
	free(d);
}

static void test_aborted_scan_returns_to_old_ap(void)
{
	struct fake f;
	struct fh_drv *d = setup(&fh_roam_baseline, &f, NULL);
	struct trace tr = { .n = 0 };

	/* nothing better around */
	drive(d, 0, 1500, weak, &tr);
	const struct fh_event *done = find(&tr, FH_EV_HANDOFF_DONE);

	CHECK(done != NULL);
	if (done) {
		CHECK(done->flags & FH_EVF_ABORTED);
		CHECK_EQ(done->to, 0);
	}
	CHECK_EQ(d->cur, 0);
	CHECK_EQ(d->st.aborted, 1);
	CHECK(fh_drv_associated(d));
	free(d);
}

static void test_driver_trains_its_predictor(void)
{
	struct fake f;
	struct fh_predictor *p = malloc(sizeof(*p));
	struct trace tr = { .n = 0 };

	fh_pred_init(p, &fh_pred_markov);
	struct fh_drv *d = setup(&fh_roam_adapted, &f, p);

	f.rssi[1] = -55;
	drive(d, 0, 6000, fading_away, &tr);
	CHECK_EQ(d->st.handoffs, 1);
	CHECK_EQ(p->observed, 1);
	CHECK_EQ(p->u.markov.c1[0][1], 1);
	free(d);
	free(p);
}

int main(void)
{
	printf("drv\n");
	RUN(test_baseline_full_scan_latency);
	RUN(test_adapted_predicts_preauths_and_uses_ft);
	RUN(test_hysteresis_blocks_small_gain);
	RUN(test_time_to_trigger_and_reset);
	RUN(test_waits_for_preauth_key);
	RUN(test_pingpong_margin_and_dwell);
	RUN(test_beacon_loss_forces_reactive_handoff);
	RUN(test_eta_from_slope);
	RUN(test_aborted_scan_returns_to_old_ap);
	RUN(test_driver_trains_its_predictor);
	TEST_DONE();
}
