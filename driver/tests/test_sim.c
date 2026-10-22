/* SPDX-License-Identifier: MIT */
/* End-to-end: simulated radio, driver, event channel and harness together. */
#include <stdlib.h>
#include <string.h>

#include "fh_run.h"
#include "fh_test.h"

static struct fh_model *model;
static struct fh_run_stats *st, *st2;

static void test_world_geometry_and_crn(void)
{
	struct fh_world w;

	fh_world_init(&w, FH_SC_CORRIDOR_WALK, 1, 2);
	CHECK_EQ(w.n_ap, 8);
	CHECK(w.n_wp == 4);
	/* the walker starts at one end of the corridor: the near AP is far stronger */
	int near = w.x < 200 ? 0 : 7, far = 7 - near;

	CHECK(fh_world_mean_rssi(&w, near) > fh_world_mean_rssi(&w, far) + 20);
	/* counter-based fading: same value whatever was drawn before */
	int a = fh_world_rssi(&w, 3, 1234, FH_K_DATA);

	for (int i = 0; i < 100; i++)
		(void)fh_world_rssi(&w, i % 8, (fh_ms_t)i, FH_K_PROBE);
	CHECK_EQ(fh_world_rssi(&w, 3, 1234, FH_K_DATA), a);
	CHECK(fh_per(25) < 0.001 && fh_per(0) > 0.99);

	fh_world_init(&w, FH_SC_CAMPUS_VEHICLE, 1, 2);
	CHECK_EQ(w.n_ap, 16);
	for (int i = 0; i < 16; i++)
		for (int j = 0; j < 16; j++) {
			double dx = w.ap[i].x - w.ap[j].x, dy = w.ap[i].y - w.ap[j].y;

			/* street neighbours never share a channel */
			if (i != j && dx * dx + dy * dy < 71.0 * 71.0)
				CHECK(w.ap[i].chan != w.ap[j].chan);
		}
	int steps = 0;

	while (!w.done && steps < 100000) {
		fh_world_step(&w, 0.01);
		steps++;
	}
	CHECK(w.done);
}

static void test_runs_are_deterministic(void)
{
	struct fh_run_cfg rc = {
		.sc = FH_SC_CAMPUS_VEHICLE, .layout_seed = 5, .walk_seed = 9, .algo = &fh_roam_adapted,
	};

	fh_model_init(model, &fh_pred_markov);
	fh_run(&rc, model, st);
	fh_model_init(model, &fh_pred_markov);
	fh_run(&rc, model, st2);
	CHECK(st->handoffs > 0);
	CHECK_EQ(st->handoffs, st2->handoffs);
	CHECK_EQ(st->lost, st2->lost);
	CHECK_EQ(st->n_lat, st2->n_lat);
	CHECK(memcmp(st->lat, st2->lat, sizeof(st->lat[0]) * st->n_lat) == 0);
	CHECK_EQ(st->ev_dropped, 0);
}

static double mean_latency(const struct fh_run_stats *s)
{
	double sum = 0;

	for (uint32_t i = 0; i < s->n_lat; i++)
		sum += fh_ho_total(&s->lat[i]);
	return s->n_lat ? sum / s->n_lat : 0;
}

static void test_adapted_beats_baseline(void)
{
	for (int sc = 0; sc < FH_SC__COUNT; sc++) {
		double lat_b = 0, lat_a = 0, loss_b = 0, loss_a = 0;
		uint32_t ho_b = 0, ho_a = 0, ft = 0;

		for (uint64_t r = 0; r < 3; r++) {
			struct fh_run_cfg rc = {
				.sc = (enum fh_scenario)sc, .layout_seed = 100 + r, .walk_seed = 200 + r,
				.algo = &fh_roam_baseline,
			};

			fh_model_init(model, &fh_pred_markov);
			fh_run(&rc, model, st);
			lat_b += mean_latency(st);
			loss_b += st->lost_ho;
			ho_b += st->handoffs;

			rc.algo = &fh_roam_adapted;
			fh_train(rc.sc, rc.layout_seed, 2, model);
			fh_run(&rc, model, st);
			lat_a += mean_latency(st);
			loss_a += st->lost_ho;
			ho_a += st->handoffs;
			ft += st->ft_used;
		}
		printf("    %-15s latency %6.1f -> %5.1f ms, lost/ho %5.2f -> %4.2f, ft %u/%u\n",
		       fh_scenario_name((enum fh_scenario)sc), lat_b / 3, lat_a / 3, loss_b / ho_b, loss_a / ho_a, ft,
		       ho_a);
		CHECK(lat_a < lat_b / 3);
		CHECK(loss_a / ho_a < loss_b / ho_b);
		CHECK(ft * 10 >= ho_a * 9);
	}
}

static void test_predictor_beats_strongest_on_corridor(void)
{
	uint64_t n = 0, hit_m = 0, hit_s = 0;

	for (uint64_t r = 0; r < 4; r++) {
		struct fh_run_cfg rc = {
			.sc = FH_SC_CORRIDOR_WALK, .layout_seed = 300 + r, .walk_seed = 400 + r,
			.algo = &fh_roam_adapted,
		};

		fh_model_init(model, &fh_pred_markov);
		fh_train(rc.sc, rc.layout_seed, 4, model);
		CHECK(model->drv.observed > 0);
		fh_run(&rc, model, st);
		n += st->pred_n;
		hit_m += st->pred_hit[FH_PRED_MARKOV];
		hit_s += st->pred_hit[FH_PRED_STRONGEST];
	}
	printf("    corridor top-1: markov %.1f%%, strongest %.1f%% (n=%llu)\n", 100.0 * hit_m / n,
	       100.0 * hit_s / n, (unsigned long long)n);
	CHECK(n >= 20);
	CHECK(hit_m > hit_s);
}

static void test_trace_is_jsonl(void)
{
	char buf[1 << 16];
	FILE *f = tmpfile();
	struct fh_run_cfg rc = {
		.sc = FH_SC_CAMPUS_VEHICLE, .layout_seed = 1, .walk_seed = 2, .algo = &fh_roam_adapted, .trace = f,
	};
	int lines = 0, done = 0, imminent = 0, meta = 0;

	CHECK(f != NULL);
	if (!f)
		return;
	fh_model_init(model, &fh_pred_markov);
	fh_run(&rc, model, st);
	rewind(f);
	while (fgets(buf, sizeof(buf), f)) {
		size_t len = strlen(buf);

		lines++;
		CHECK(buf[0] == '{' && len >= 2 && buf[len - 2] == '}');
		meta += strstr(buf, "\"type\":\"META\"") != NULL;
		done += strstr(buf, "\"type\":\"HANDOFF_DONE\"") != NULL;
		imminent += strstr(buf, "\"type\":\"HANDOFF_IMMINENT\"") != NULL;
	}
	fclose(f);
	CHECK_EQ(meta, 1);
	CHECK(done >= (int)st->handoffs);
	CHECK(imminent > 0);
	CHECK(lines > 100);
}

int main(void)
{
	model = malloc(sizeof(*model));
	st = malloc(sizeof(*st));
	st2 = malloc(sizeof(*st2));
	printf("sim\n");
	RUN(test_world_geometry_and_crn);
	RUN(test_runs_are_deterministic);
	RUN(test_adapted_beats_baseline);
	RUN(test_predictor_beats_strongest_on_corridor);
	RUN(test_trace_is_jsonl);
	free(model);
	free(st);
	free(st2);
	TEST_DONE();
}
