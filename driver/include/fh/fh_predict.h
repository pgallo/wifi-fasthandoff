/* SPDX-License-Identifier: MIT */
#ifndef FH_PREDICT_H
#define FH_PREDICT_H

#include "fh/fh_types.h"

/* What the driver knows when it has to guess the next AP. */
struct fh_pred_ctx {
	fh_ap_t prev; /* AP before the current one */
	fh_ap_t cur;
	int16_t rssi[FH_MAX_APS];  /* smoothed or cached RSSI, FH_RSSI_FLOOR if unseen */
	int16_t slope[FH_MAX_APS]; /* dB/s */
};

#define FH_MARKOV_N FH_MAX_APS
#define FH_KNN_SLOTS 384
#define FH_KNN_K 5

struct fh_markov {
	uint16_t c1[FH_MARKOV_N][FH_MARKOV_N];
	uint16_t c2[FH_MARKOV_N][FH_MARKOV_N][FH_MARKOV_N];
};

struct fh_knn_sample {
	int16_t rssi[FH_MAX_APS];
	int16_t slope[FH_MAX_APS];
	fh_ap_t cur;
	fh_ap_t label;
};

struct fh_knn {
	struct fh_knn_sample s[FH_KNN_SLOTS]; /* fixed ring, oldest overwritten */
	uint16_t head;
	uint16_t n;
};

struct fh_predictor;

/* Predictor operations; implementations are selected like driver ops. */
struct fh_pred_ops {
	const char *name;
	void (*reset)(struct fh_predictor *p);
	void (*observe)(struct fh_predictor *p, const struct fh_pred_ctx *c, fh_ap_t next);
	/* Writes up to k candidates, best first, never c->cur. Returns the count. */
	int (*rank)(const struct fh_predictor *p, const struct fh_pred_ctx *c, fh_ap_t *out, int k);
};

struct fh_predictor {
	const struct fh_pred_ops *ops;
	uint32_t observed;
	union {
		struct fh_markov markov;
		struct fh_knn knn;
	} u;
};

enum fh_pred_kind { FH_PRED_STRONGEST, FH_PRED_MARKOV, FH_PRED_KNN, FH_PRED__COUNT };

extern const struct fh_pred_ops fh_pred_strongest;
extern const struct fh_pred_ops fh_pred_markov;
extern const struct fh_pred_ops fh_pred_knn;
extern const struct fh_pred_ops *const fh_pred_table[FH_PRED__COUNT];

void fh_pred_init(struct fh_predictor *p, const struct fh_pred_ops *ops);
void fh_pred_ctx_clear(struct fh_pred_ctx *c, fh_ap_t prev, fh_ap_t cur);
const struct fh_pred_ops *fh_pred_by_name(const char *name);

#endif
