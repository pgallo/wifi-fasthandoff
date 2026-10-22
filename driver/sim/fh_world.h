/* SPDX-License-Identifier: MIT */
#ifndef FH_WORLD_H
#define FH_WORLD_H

#include "fh/fh_drv.h"

enum fh_scenario { FH_SC_CORRIDOR_WALK, FH_SC_CAMPUS_WALK, FH_SC_CAMPUS_VEHICLE, FH_SC__COUNT };

const char *fh_scenario_name(enum fh_scenario sc);
int fh_scenario_parse(const char *name);

/* ---- random numbers ---- */
struct fh_rng {
	uint64_t s;
};

uint64_t fh_mix64(uint64_t x);
void fh_rng_seed(struct fh_rng *r, uint64_t seed);
uint64_t fh_rng_next(struct fh_rng *r);
double fh_rng_unit(struct fh_rng *r);
double fh_rng_gauss(struct fh_rng *r);

/*
 * Counter-based draws: the value depends only on (seed, a, b, c), not on how
 * many draws happened before. Fading and frame errors use these, so baseline
 * and adapted runs see the same channel even though they query it differently.
 */
enum { FH_K_DATA = 1, FH_K_BEACON, FH_K_PROBE, FH_K_FADE, FH_K_TX, FH_K_JITTER };
double fh_crn_unit(uint64_t seed, uint32_t a, uint32_t b, uint32_t c);
double fh_crn_gauss(uint64_t seed, uint32_t a, uint32_t b, uint32_t c);

/* ---- radio environment and mobility ---- */
struct fh_sim_ap {
	double x, y;
	double shadow_db; /* spatially correlated shadowing, AR(1) over distance moved */
	uint64_t bssid;
	uint8_t chan;
	uint8_t subnet;
	uint8_t load_pct;
};

#define FH_MAX_WAYPOINTS 64

struct fh_world {
	enum fh_scenario sc;
	uint64_t chan_seed;
	struct fh_sim_ap ap[FH_MAX_APS];
	int n_ap;
	double spacing;
	double tx_dbm, pl0_db, ple;
	double shadow_sigma, shadow_dcorr;
	double rician_k;
	double wx[FH_MAX_WAYPOINTS], wy[FH_MAX_WAYPOINTS];
	int n_wp, wp;
	double x, y, speed;
	bool done;
	struct fh_rng rng;
};

/* layout_seed fixes AP loads; walk_seed fixes the route, shadowing and fading. */
void fh_world_init(struct fh_world *w, enum fh_scenario sc, uint64_t layout_seed, uint64_t walk_seed);
void fh_world_step(struct fh_world *w, double dt_s);
double fh_world_mean_rssi(const struct fh_world *w, int ap);
int fh_world_rssi(const struct fh_world *w, int ap, fh_ms_t t, uint32_t kind);
double fh_per(double snr_db);
void fh_world_ap_info(const struct fh_world *w, struct fh_ap_info *out);

/* ---- hardware ops backed by the world ---- */
struct fh_simhw {
	const struct fh_world *w;
	uint64_t seed;
};

extern const struct fh_hw_ops fh_simhw_ops;

#endif
