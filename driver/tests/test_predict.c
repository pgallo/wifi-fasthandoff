/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "fh/fh_predict.h"
#include "fh_test.h"
#include "fh_world.h"

/* All candidates equally strong: only learned history can separate them. */
static void flat_ctx(struct fh_pred_ctx *c, fh_ap_t prev, fh_ap_t cur, int n_ap)
{
	fh_pred_ctx_clear(c, prev, cur);
	for (int i = 0; i < n_ap; i++)
		c->rssi[i] = -70;
}

static fh_ap_t top1(const struct fh_predictor *p, const struct fh_pred_ctx *c)
{
	fh_ap_t out = FH_AP_NONE;

	p->ops->rank(p, c, &out, 1);
	return out;
}

static void test_markov_learns_direction(void)
{
	struct fh_predictor *p = malloc(sizeof(*p));
	struct fh_pred_ctx c;

	fh_pred_init(p, &fh_pred_markov);
	/* corridor 0..7 walked in both directions */
	for (int rep = 0; rep < 3; rep++)
		for (int i = 1; i < 7; i++) {
			flat_ctx(&c, (fh_ap_t)(i - 1), (fh_ap_t)i, 8);
			p->ops->observe(p, &c, (fh_ap_t)(i + 1));
			flat_ctx(&c, (fh_ap_t)(i + 1), (fh_ap_t)i, 8);
			p->ops->observe(p, &c, (fh_ap_t)(i - 1));
		}
	flat_ctx(&c, 2, 3, 8);
	CHECK_EQ(top1(p, &c), 4);
	flat_ctx(&c, 4, 3, 8);
	CHECK_EQ(top1(p, &c), 2);
	CHECK_EQ(p->observed, 36);
	free(p);
}

static void test_markov_backs_off_to_first_order(void)
{
	struct fh_predictor *p = malloc(sizeof(*p));
	struct fh_pred_ctx c;

	fh_pred_init(p, &fh_pred_markov);
	for (int i = 0; i < 3; i++) {
		flat_ctx(&c, 2, 3, 8);
		p->ops->observe(p, &c, 4);
	}
	flat_ctx(&c, 4, 3, 8);
	p->ops->observe(p, &c, 2);
	/* unknown previous AP: use P(next | cur) */
	flat_ctx(&c, FH_AP_NONE, 3, 8);
	CHECK_EQ(top1(p, &c), 4);
	flat_ctx(&c, 7, 3, 8);
	CHECK_EQ(top1(p, &c), 4);
	free(p);
}

static void test_markov_signal_overrides_weak_prior(void)
{
	struct fh_predictor *p = malloc(sizeof(*p));
	struct fh_pred_ctx c;

	fh_pred_init(p, &fh_pred_markov);
	flat_ctx(&c, 2, 3, 8);
	p->ops->observe(p, &c, 4); /* one observation only */
	flat_ctx(&c, 2, 3, 8);
	c.rssi[4] = -85;
	c.rssi[6] = -55; /* 30 dB stronger: evidence wins */
	CHECK_EQ(top1(p, &c), 6);
	/* with no history at all, the ranking is by signal */
	fh_pred_init(p, &fh_pred_markov);
	CHECK_EQ(top1(p, &c), 6);
	free(p);
}

static void test_strongest_ranks_by_rssi(void)
{
	struct fh_pred_ctx c;
	fh_ap_t out[4];

	fh_pred_ctx_clear(&c, FH_AP_NONE, 1);
	c.rssi[1] = -40; /* current AP is never a candidate */
	c.rssi[2] = -70;
	c.rssi[5] = -60;
	int n = fh_pred_strongest.rank(NULL, &c, out, 4);

	CHECK_EQ(n, 2);
	CHECK_EQ(out[0], 5);
	CHECK_EQ(out[1], 2);
}

static void test_knn_separates_fingerprints(void)
{
	struct fh_predictor *p = malloc(sizeof(*p));
	struct fh_pred_ctx c;
	struct fh_rng rng;

	fh_pred_init(p, &fh_pred_knn);
	fh_rng_seed(&rng, 3);
	/* at AP 1, AP 2 getting stronger means we go to 2, AP 0 getting stronger means 0 */
	for (int i = 0; i < 40; i++) {
		fh_pred_ctx_clear(&c, FH_AP_NONE, 1);
		c.rssi[1] = (int16_t)(-70 + 2 * fh_rng_gauss(&rng));
		c.rssi[0] = (int16_t)(-75 + 2 * fh_rng_gauss(&rng));
		c.rssi[2] = (int16_t)(-75 + 2 * fh_rng_gauss(&rng));
		bool toward2 = i % 2;

		c.slope[2] = toward2 ? 4 : -4;
		c.slope[0] = toward2 ? -4 : 4;
		p->ops->observe(p, &c, toward2 ? 2 : 0);
	}
	fh_pred_ctx_clear(&c, FH_AP_NONE, 1);
	c.rssi[1] = -70;
	c.rssi[0] = -74;
	c.rssi[2] = -76;
	c.slope[2] = 5;
	c.slope[0] = -3;
	CHECK_EQ(top1(p, &c), 2);
	c.slope[2] = -5;
	c.slope[0] = 3;
	CHECK_EQ(top1(p, &c), 0);
	CHECK_EQ(p->u.knn.n, 40);
	free(p);
}

static void test_rank_excludes_current_and_respects_k(void)
{
	struct fh_predictor *p = malloc(sizeof(*p));
	struct fh_pred_ctx c;
	fh_ap_t out[8];

	for (int k = 0; k < FH_PRED__COUNT; k++) {
		fh_pred_init(p, fh_pred_table[k]);
		flat_ctx(&c, 0, 3, 6);
		int n = p->ops->rank(p, &c, out, 3);

		CHECK(n <= 3 && n >= 1);
		for (int i = 0; i < n; i++)
			CHECK(out[i] != 3);
	}
	CHECK(fh_pred_by_name("knn") == &fh_pred_knn);
	CHECK(fh_pred_by_name("nope") == NULL);
	free(p);
}

/*
 * Accuracy on a synthetic mobility trace: a ring of 8 APs, the walker keeps
 * its direction with probability 0.85. Signal is uninformative, so the
 * ranking has to come from the transition history.
 */
static void test_markov_accuracy_on_persistent_walk(void)
{
	struct fh_predictor *m = malloc(sizeof(*m));
	struct fh_predictor *s = malloc(sizeof(*s));
	struct fh_rng rng;
	struct fh_pred_ctx c;
	int prev = 0, cur = 1, dir = 1, hits_m = 0, hits_s = 0, n = 0;

	fh_pred_init(m, &fh_pred_markov);
	fh_pred_init(s, &fh_pred_strongest);
	fh_rng_seed(&rng, 11);
	for (int step = 0; step < 2000; step++) {
		if (fh_rng_unit(&rng) > 0.85)
			dir = -dir;
		int next = (cur + dir + 8) % 8;

		flat_ctx(&c, (fh_ap_t)prev, (fh_ap_t)cur, 8);
		if (step >= 1000) {
			n++;
			hits_m += top1(m, &c) == next;
			hits_s += top1(s, &c) == next;
		}
		m->ops->observe(m, &c, (fh_ap_t)next);
		prev = cur;
		cur = next;
	}
	double acc_m = (double)hits_m / n, acc_s = (double)hits_s / n;

	printf("    top-1 accuracy: markov %.1f%%, strongest %.1f%% (n=%d)\n", 100 * acc_m, 100 * acc_s, n);
	CHECK(acc_m > 0.80); /* the Bayes-optimal rate here is 0.85 */
	CHECK(acc_m > acc_s + 0.3);
	free(m);
	free(s);
}

int main(void)
{
	printf("predict\n");
	RUN(test_markov_learns_direction);
	RUN(test_markov_backs_off_to_first_order);
	RUN(test_markov_signal_overrides_weak_prior);
	RUN(test_strongest_ranks_by_rssi);
	RUN(test_knn_separates_fingerprints);
	RUN(test_rank_excludes_current_and_respects_k);
	RUN(test_markov_accuracy_on_persistent_walk);
	TEST_DONE();
}
