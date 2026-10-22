/* SPDX-License-Identifier: MIT */
#ifndef FH_RUN_H
#define FH_RUN_H

#include <stdio.h>

#include "fh_world.h"

#define FH_RUN_MAX_LAT 512

struct fh_run_stats {
	uint32_t duration_ms;
	uint32_t handoffs, pingpongs, aborted, reactive, ft_used, full_eap, preauths;
	uint32_t pkts, lost, lost_ho;
	uint32_t outage_ms, offchan_ms;
	uint32_t n_lat;
	struct fh_ho_latency lat[FH_RUN_MAX_LAT];
	uint32_t pred_n, pred_hit[FH_PRED__COUNT];
	uint32_t ev_seen, ev_dropped;
};

/* The driver's own predictor plus side-by-side copies used to report accuracy. */
struct fh_model {
	struct fh_predictor drv;
	struct fh_predictor eval[FH_PRED__COUNT];
};

struct fh_run_cfg {
	enum fh_scenario sc;
	uint64_t layout_seed;
	uint64_t walk_seed;
	const struct fh_roam_ops *algo;
	FILE *trace; /* optional JSONL event and link trace */
};

void fh_model_init(struct fh_model *m, const struct fh_pred_ops *primary);
void fh_run(const struct fh_run_cfg *cfg, struct fh_model *m, struct fh_run_stats *st);
/* Train on n walks over the same layout: the transition history of earlier days. */
void fh_train(enum fh_scenario sc, uint64_t layout_seed, int n_walks, struct fh_model *m);

#endif
