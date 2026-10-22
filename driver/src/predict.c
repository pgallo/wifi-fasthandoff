/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <string.h>

#include "fh/fh_predict.h"

#define NEG_INF (-1e300)

void fh_pred_ctx_clear(struct fh_pred_ctx *c, fh_ap_t prev, fh_ap_t cur)
{
	c->prev = prev;
	c->cur = cur;
	for (int i = 0; i < FH_MAX_APS; i++) {
		c->rssi[i] = FH_RSSI_FLOOR;
		c->slope[i] = 0;
	}
}

/* Write the k best indices of score[] (excluding NEG_INF) to out, best first. */
static int topk(const double *score, fh_ap_t *out, int k)
{
	bool used[FH_MAX_APS] = { false };
	int n = 0;

	while (n < k) {
		int best = -1;

		for (int i = 0; i < FH_MAX_APS; i++)
			if (!used[i] && score[i] > NEG_INF && (best < 0 || score[i] > score[best]))
				best = i;
		if (best < 0)
			break;
		used[best] = true;
		out[n++] = (fh_ap_t)best;
	}
	return n;
}

/* small bonus so that ties (or no history at all) fall back to signal strength */
static double signal_tiebreak(const struct fh_pred_ctx *c, int i)
{
	return 1e-4 * (double)(c->rssi[i] - FH_RSSI_FLOOR);
}

/* ---- strongest neighbour: the non-learning reference ---- */

static void strongest_reset(struct fh_predictor *p)
{
	(void)p;
}

static void strongest_observe(struct fh_predictor *p, const struct fh_pred_ctx *c, fh_ap_t next)
{
	(void)p;
	(void)c;
	(void)next;
}

static int strongest_rank(const struct fh_predictor *p, const struct fh_pred_ctx *c, fh_ap_t *out, int k)
{
	double score[FH_MAX_APS];

	(void)p;
	for (int i = 0; i < FH_MAX_APS; i++)
		score[i] = (i == c->cur || c->rssi[i] <= FH_RSSI_FLOOR) ? NEG_INF : c->rssi[i];
	return topk(score, out, k);
}

const struct fh_pred_ops fh_pred_strongest = {
	.name = "strongest",
	.reset = strongest_reset,
	.observe = strongest_observe,
	.rank = strongest_rank,
};

/*
 * ---- Markov chain over AP transitions ----
 * The state is the last transition (prev -> cur), which carries the direction
 * of travel; with no history for that state it backs off to P(next | cur).
 * The prior is combined with the cached RSSI of each candidate, scaled so
 * that MARKOV_SIGNAL_DB of signal difference is worth a factor e of prior.
 */
#define MARKOV_SIGNAL_DB 3.0

static void markov_reset(struct fh_predictor *p)
{
	memset(&p->u.markov, 0, sizeof(p->u.markov));
}

static void sat_inc(uint16_t *v)
{
	if (*v < UINT16_MAX)
		(*v)++;
}

static void markov_observe(struct fh_predictor *p, const struct fh_pred_ctx *c, fh_ap_t next)
{
	struct fh_markov *m = &p->u.markov;

	if (c->cur >= FH_MARKOV_N || next >= FH_MARKOV_N || next == c->cur)
		return;
	sat_inc(&m->c1[c->cur][next]);
	if (c->prev < FH_MARKOV_N)
		sat_inc(&m->c2[c->prev][c->cur][next]);
	p->observed++;
}

static int markov_rank(const struct fh_predictor *p, const struct fh_pred_ctx *c, fh_ap_t *out, int k)
{
	const struct fh_markov *m = &p->u.markov;
	double score[FH_MAX_APS];

	if (c->cur >= FH_MARKOV_N)
		return strongest_rank(p, c, out, k);

	const double alpha = 0.1; /* additive smoothing for the first-order table */
	const double beta = 2.0;  /* how many pseudo-counts the backoff is worth */
	double n1 = 0, n2 = 0;
	bool has_prev = c->prev < FH_MARKOV_N;

	int top = FH_RSSI_FLOOR;

	for (int j = 0; j < FH_MAX_APS; j++)
		if (j != c->cur && c->rssi[j] > top)
			top = c->rssi[j];
	for (int j = 0; j < FH_MARKOV_N; j++) {
		n1 += m->c1[c->cur][j];
		if (has_prev)
			n2 += m->c2[c->prev][c->cur][j];
	}
	for (int j = 0; j < FH_MAX_APS; j++) {
		if (j == c->cur) {
			score[j] = NEG_INF;
			continue;
		}
		double p1 = (m->c1[c->cur][j] + alpha) / (n1 + alpha * FH_MARKOV_N);
		double pr = p1;

		if (n2 > 0)
			pr = (m->c2[c->prev][c->cur][j] + beta * p1) / (n2 + beta);
		/* one HMM-style step: transition prior times a signal likelihood */
		score[j] = log(pr) + (c->rssi[j] - top) / MARKOV_SIGNAL_DB;
	}
	return topk(score, out, k);
}

const struct fh_pred_ops fh_pred_markov = {
	.name = "markov",
	.reset = markov_reset,
	.observe = markov_observe,
	.rank = markov_rank,
};

/*
 * ---- KNN over RSSI fingerprints ----
 * A fingerprint is the vector of RSSI and RSSI slope over all known APs at
 * prediction time: it encodes where the client is and which way it moves.
 */

#define KNN_SLOPE_W    4
#define KNN_CUR_MISS   2500
#define KNN_SLOPE_CLIP 20

static int clip_slope(int s)
{
	return s > KNN_SLOPE_CLIP ? KNN_SLOPE_CLIP : (s < -KNN_SLOPE_CLIP ? -KNN_SLOPE_CLIP : s);
}

static void knn_reset(struct fh_predictor *p)
{
	memset(&p->u.knn, 0, sizeof(p->u.knn));
}

static void knn_observe(struct fh_predictor *p, const struct fh_pred_ctx *c, fh_ap_t next)
{
	struct fh_knn *kn = &p->u.knn;

	if (next >= FH_MAX_APS || next == c->cur)
		return;
	struct fh_knn_sample *s = &kn->s[kn->head];

	memcpy(s->rssi, c->rssi, sizeof(s->rssi));
	for (int i = 0; i < FH_MAX_APS; i++)
		s->slope[i] = (int16_t)clip_slope(c->slope[i]);
	s->cur = c->cur;
	s->label = next;
	kn->head = (uint16_t)((kn->head + 1) % FH_KNN_SLOTS);
	if (kn->n < FH_KNN_SLOTS)
		kn->n++;
	p->observed++;
}

static int32_t knn_dist(const struct fh_knn_sample *s, const struct fh_pred_ctx *c)
{
	int32_t d = s->cur == c->cur ? 0 : KNN_CUR_MISS;

	for (int i = 0; i < FH_MAX_APS; i++) {
		int32_t dr = c->rssi[i] - s->rssi[i];
		int32_t ds = clip_slope(c->slope[i]) - s->slope[i];

		d += dr * dr + KNN_SLOPE_W * ds * ds;
	}
	return d;
}

static int knn_rank(const struct fh_predictor *p, const struct fh_pred_ctx *c, fh_ap_t *out, int k)
{
	const struct fh_knn *kn = &p->u.knn;
	int32_t nd[FH_KNN_K];
	int ni[FH_KNN_K];
	int nn = 0;

	if (kn->n == 0)
		return strongest_rank(p, c, out, k);

	/* insertion into a K-sized sorted array: no allocation, O(n K) */
	for (int i = 0; i < kn->n; i++) {
		if (kn->s[i].label == c->cur)
			continue;
		int32_t d = knn_dist(&kn->s[i], c);
		int pos = nn < FH_KNN_K ? nn : FH_KNN_K;

		while (pos > 0 && nd[pos - 1] > d)
			pos--;
		if (pos >= FH_KNN_K)
			continue;
		int last = nn < FH_KNN_K ? nn : FH_KNN_K - 1;

		for (int j = last; j > pos; j--) {
			nd[j] = nd[j - 1];
			ni[j] = ni[j - 1];
		}
		nd[pos] = d;
		ni[pos] = i;
		if (nn < FH_KNN_K)
			nn++;
	}

	double score[FH_MAX_APS];

	for (int j = 0; j < FH_MAX_APS; j++)
		score[j] = j == c->cur ? NEG_INF : signal_tiebreak(c, j);
	for (int j = 0; j < nn; j++)
		score[kn->s[ni[j]].label] += 1.0 / (1.0 + sqrt((double)nd[j]));
	return topk(score, out, k);
}

const struct fh_pred_ops fh_pred_knn = {
	.name = "knn",
	.reset = knn_reset,
	.observe = knn_observe,
	.rank = knn_rank,
};

const struct fh_pred_ops *const fh_pred_table[FH_PRED__COUNT] = {
	[FH_PRED_STRONGEST] = &fh_pred_strongest,
	[FH_PRED_MARKOV] = &fh_pred_markov,
	[FH_PRED_KNN] = &fh_pred_knn,
};

void fh_pred_init(struct fh_predictor *p, const struct fh_pred_ops *ops)
{
	p->ops = ops;
	p->observed = 0;
	ops->reset(p);
}

const struct fh_pred_ops *fh_pred_by_name(const char *name)
{
	for (int i = 0; i < FH_PRED__COUNT; i++)
		if (strcmp(fh_pred_table[i]->name, name) == 0)
			return fh_pred_table[i];
	return NULL;
}
