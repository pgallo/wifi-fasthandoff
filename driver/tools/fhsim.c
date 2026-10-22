/* SPDX-License-Identifier: MIT */
/*
 * fhsim: baseline vs adapted handoff over N seeded runs per scenario.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fh_run.h"

#define MAX_SAMPLES 16384

struct agg {
	uint64_t runs, handoffs, pingpongs, aborted, ft, lost_ho, pkts, lost, outage_ms, offchan_ms, duration_ms;
	uint64_t phase[4];
	uint32_t n;
	uint32_t total[MAX_SAMPLES];
	uint64_t pred_n, pred_hit[FH_PRED__COUNT];
};

struct opts {
	int runs;
	unsigned long long seed;
	int train;
	int scenario; /* -1: all */
	const struct fh_pred_ops *pred;
	const char *trace_dir;
	int trace_runs;
	bool check;
	bool curve;
};

static void usage(void)
{
	fprintf(stderr, "usage: fhsim [--runs N] [--seed S] [--scenario corridor-walk|campus-walk|campus-vehicle|all]\n"
			"             [--predictor markov|knn|strongest] [--train N]\n"
			"             [--trace-dir DIR] [--trace-runs K] [--check] [--curve]\n");
}

static int parse(int argc, char **argv, struct opts *o)
{
	*o = (struct opts){ .runs = 20, .seed = 1, .train = 6, .scenario = -1, .pred = &fh_pred_markov, .trace_runs = 5 };
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "--check") || !strcmp(a, "--curve")) {
			*(a[2] == 'c' && a[3] == 'h' ? &o->check : &o->curve) = true;
			continue;
		}
		if (!v)
			return -1;
		i++;
		if (!strcmp(a, "--runs"))
			o->runs = atoi(v);
		else if (!strcmp(a, "--seed"))
			o->seed = strtoull(v, NULL, 10);
		else if (!strcmp(a, "--train"))
			o->train = atoi(v);
		else if (!strcmp(a, "--trace-dir"))
			o->trace_dir = v;
		else if (!strcmp(a, "--trace-runs"))
			o->trace_runs = atoi(v);
		else if (!strcmp(a, "--predictor")) {
			if (!(o->pred = fh_pred_by_name(v)))
				return -1;
		} else if (!strcmp(a, "--scenario")) {
			o->scenario = strcmp(v, "all") ? fh_scenario_parse(v) : -1;
			if (o->scenario < -1)
				return -1;
		} else
			return -1;
	}
	return o->runs > 0 && o->train >= 0 ? 0 : -1;
}

static void accumulate(struct agg *g, const struct fh_run_stats *st)
{
	g->runs++;
	g->handoffs += st->handoffs;
	g->pingpongs += st->pingpongs;
	g->aborted += st->aborted;
	g->ft += st->ft_used;
	g->lost_ho += st->lost_ho;
	g->pkts += st->pkts;
	g->lost += st->lost;
	g->outage_ms += st->outage_ms;
	g->offchan_ms += st->offchan_ms;
	g->duration_ms += st->duration_ms;
	for (uint32_t i = 0; i < st->n_lat; i++) {
		g->phase[0] += st->lat[i].scan_ms;
		g->phase[1] += st->lat[i].auth_ms;
		g->phase[2] += st->lat[i].reassoc_ms;
		g->phase[3] += st->lat[i].key_ms;
		if (g->n < MAX_SAMPLES)
			g->total[g->n++] = fh_ho_total(&st->lat[i]);
	}
	g->pred_n += st->pred_n;
	for (int k = 0; k < FH_PRED__COUNT; k++)
		g->pred_hit[k] += st->pred_hit[k];
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return (x > y) - (x < y);
}

struct summary {
	double mean, p95, lost_per_ho, pingpong_per_run;
};

static struct summary summarise(struct agg *g)
{
	struct summary s = { 0 };
	uint64_t sum = 0;

	qsort(g->total, g->n, sizeof(g->total[0]), cmp_u32);
	for (uint32_t i = 0; i < g->n; i++)
		sum += g->total[i];
	if (g->n) {
		s.mean = (double)sum / g->n;
		s.p95 = g->total[(uint32_t)(0.95 * (g->n - 1) + 0.5)];
	}
	s.lost_per_ho = g->handoffs ? (double)g->lost_ho / (double)g->handoffs : 0.0;
	s.pingpong_per_run = g->runs ? (double)g->pingpongs / (double)g->runs : 0.0;
	return s;
}

static void print_row(const char *sc, const char *algo, struct agg *g, struct summary s)
{
	double n = g->n ? g->n : 1;

	printf("%-15s %-9s %7.1f %8.2f %7.2f %8.1f %8.0f %6.1f %5.1f %7.1f %5.1f %8.2f %8.2f %8.2f %5.0f\n", sc, algo,
	       (double)g->handoffs / g->runs, s.pingpong_per_run, (double)g->aborted / g->runs, s.mean, s.p95,
	       g->phase[0] / n, g->phase[1] / n, g->phase[2] / n, g->phase[3] / n, s.lost_per_ho,
	       100.0 * g->lost / (double)g->pkts, g->outage_ms / 1000.0 / g->runs,
	       g->handoffs ? 100.0 * g->ft / g->handoffs : 0.0);
}

static FILE *open_trace(const struct opts *o, enum fh_scenario sc, const char *algo, int run)
{
	char path[512];

	if (!o->trace_dir || run >= o->trace_runs)
		return NULL;
	snprintf(path, sizeof(path), "%s/%s-%s-%02d.jsonl", o->trace_dir, fh_scenario_name(sc), algo, run);
	FILE *f = fopen(path, "w");

	if (!f)
		fprintf(stderr, "fhsim: cannot write %s\n", path);
	return f;
}

/* Predictor accuracy as a function of how much transition history it has seen. */
static void learning_curve(const struct opts *o, struct fh_model *model, struct fh_run_stats *st)
{
	static const int walks[] = { 0, 1, 2, 4, 8, 12 };

	printf("learning curve: top-1 accuracy vs training walks (runs=%d)\n", o->runs);
	printf("%-15s %6s %9s", "scenario", "walks", "decisions");
	for (int k = 0; k < FH_PRED__COUNT; k++)
		printf(" %10s", fh_pred_table[k]->name);
	printf("\n");
	for (int sc = 0; sc < FH_SC__COUNT; sc++) {
		if (o->scenario >= 0 && sc != o->scenario)
			continue;
		for (size_t w = 0; w < FH_ARRAY_SIZE(walks); w++) {
			uint64_t n = 0, hit[FH_PRED__COUNT] = { 0 };

			for (int r = 0; r < o->runs; r++) {
				uint64_t layout = o->seed * 1000u + (uint64_t)r;
				struct fh_run_cfg rc = {
					.sc = (enum fh_scenario)sc,
					.layout_seed = layout,
					.walk_seed = fh_mix64(layout ^ 0x7E57ull),
					.algo = &fh_roam_adapted,
				};

				fh_model_init(model, o->pred);
				fh_train(rc.sc, layout, walks[w], model);
				fh_run(&rc, model, st);
				n += st->pred_n;
				for (int k = 0; k < FH_PRED__COUNT; k++)
					hit[k] += st->pred_hit[k];
			}
			printf("%-15s %6d %9llu", fh_scenario_name(sc), walks[w], (unsigned long long)n);
			for (int k = 0; k < FH_PRED__COUNT; k++)
				printf(" %9.1f%%", n ? 100.0 * hit[k] / n : 0.0);
			printf("\n");
		}
	}
}

int main(int argc, char **argv)
{
	struct opts o;

	if (parse(argc, argv, &o)) {
		usage();
		return 2;
	}

	static struct agg base[FH_SC__COUNT], adapt[FH_SC__COUNT];
	struct fh_model *model = malloc(sizeof(*model));
	struct fh_run_stats *st = malloc(sizeof(*st));
	int failures = 0;

	if (!model || !st)
		return 1;
	if (o.curve) {
		learning_curve(&o, model, st);
		free(model);
		free(st);
		return 0;
	}

	printf("fhsim: runs=%d seed=%llu train_walks=%d predictor=%s\n\n", o.runs, o.seed, o.train, o.pred->name);
	printf("%-15s %-9s %7s %8s %7s %8s %8s %6s %5s %7s %5s %8s %8s %8s %5s\n", "scenario", "algo", "ho/run",
	       "pingpong", "aborted", "lat_mean", "lat_p95", "scan", "auth", "reassoc", "key", "lost/ho", "loss%",
	       "outage_s", "ft%");

	for (int sc = 0; sc < FH_SC__COUNT; sc++) {
		if (o.scenario >= 0 && sc != o.scenario)
			continue;
		for (int r = 0; r < o.runs; r++) {
			uint64_t layout = o.seed * 1000u + (uint64_t)r;
			struct fh_run_cfg rc = {
				.sc = (enum fh_scenario)sc,
				.layout_seed = layout,
				.walk_seed = fh_mix64(layout ^ 0x7E57ull),
			};

			rc.algo = &fh_roam_baseline;
			rc.trace = open_trace(&o, rc.sc, "baseline", r);
			fh_model_init(model, o.pred);
			fh_run(&rc, model, st);
			accumulate(&base[sc], st);
			if (rc.trace)
				fclose(rc.trace);

			rc.algo = &fh_roam_adapted;
			fh_model_init(model, o.pred);
			fh_train(rc.sc, layout, o.train, model);
			rc.trace = open_trace(&o, rc.sc, "adapted", r);
			fh_run(&rc, model, st);
			accumulate(&adapt[sc], st);
			if (rc.trace)
				fclose(rc.trace);
		}
		struct summary sb = summarise(&base[sc]), sa = summarise(&adapt[sc]);

		print_row(fh_scenario_name(sc), "baseline", &base[sc], sb);
		print_row(fh_scenario_name(sc), "adapted", &adapt[sc], sa);
		if (sa.mean >= sb.mean || sa.p95 >= sb.p95 || sa.lost_per_ho >= sb.lost_per_ho ||
		    sa.pingpong_per_run > sb.pingpong_per_run) {
			fprintf(stderr, "fhsim: adapted is not better than baseline on %s\n", fh_scenario_name(sc));
			failures++;
		}
	}

	printf("\nnext-AP predictor, top-1 accuracy (prediction at HANDOFF_IMMINENT, truth: best AP 1 s after the "
	       "handoff)\n");
	printf("%-15s %9s", "scenario", "decisions");
	for (int k = 0; k < FH_PRED__COUNT; k++)
		printf(" %10s", fh_pred_table[k]->name);
	printf("\n");
	for (int sc = 0; sc < FH_SC__COUNT; sc++) {
		if (!adapt[sc].runs)
			continue;
		printf("%-15s %9llu", fh_scenario_name(sc), (unsigned long long)adapt[sc].pred_n);
		for (int k = 0; k < FH_PRED__COUNT; k++)
			printf(" %9.1f%%", adapt[sc].pred_n ? 100.0 * adapt[sc].pred_hit[k] / adapt[sc].pred_n : 0.0);
		printf("\n");
	}

	free(model);
	free(st);
	if (o.check && failures) {
		fprintf(stderr, "fhsim: check failed on %d scenario(s)\n", failures);
		return 1;
	}
	return 0;
}
