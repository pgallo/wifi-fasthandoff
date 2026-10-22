/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <string.h>

#include "fh_world.h"

#define FH_PI 3.14159265358979323846

static const char *const sc_names[FH_SC__COUNT] = { "corridor-walk", "campus-walk", "campus-vehicle" };
static const uint8_t plan[3] = { 1, 6, 11 }; /* non-overlapping 2.4 GHz channels */

const char *fh_scenario_name(enum fh_scenario sc)
{
	return (unsigned)sc < FH_SC__COUNT ? sc_names[sc] : "?";
}

int fh_scenario_parse(const char *name)
{
	for (int i = 0; i < FH_SC__COUNT; i++)
		if (strcmp(name, sc_names[i]) == 0)
			return i;
	return -FH_EINVAL;
}

uint64_t fh_mix64(uint64_t x)
{
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBull;
	x ^= x >> 31;
	return x;
}

void fh_rng_seed(struct fh_rng *r, uint64_t seed)
{
	r->s = fh_mix64(seed ^ 0x6A09E667F3BCC909ull);
}

uint64_t fh_rng_next(struct fh_rng *r)
{
	r->s += 0x9E3779B97F4A7C15ull;
	return fh_mix64(r->s);
}

double fh_rng_unit(struct fh_rng *r)
{
	return (double)(fh_rng_next(r) >> 11) * 0x1.0p-53;
}

static double box_muller(double u1, double u2)
{
	if (u1 < 1e-300)
		u1 = 1e-300;
	return sqrt(-2.0 * log(u1)) * cos(2.0 * FH_PI * u2);
}

double fh_rng_gauss(struct fh_rng *r)
{
	double u1 = fh_rng_unit(r);

	return box_muller(u1, fh_rng_unit(r));
}

double fh_crn_unit(uint64_t seed, uint32_t a, uint32_t b, uint32_t c)
{
	uint64_t h = fh_mix64(seed + 0x9E3779B97F4A7C15ull * (a + 1u));

	h = fh_mix64(h ^ (0xC2B2AE3D27D4EB4Full * (b + 1u)));
	h = fh_mix64(h ^ (0x165667B19E3779F9ull * (c + 1u)));
	return (double)(h >> 11) * 0x1.0p-53;
}

double fh_crn_gauss(uint64_t seed, uint32_t a, uint32_t b, uint32_t c)
{
	return box_muller(fh_crn_unit(seed, a, b, 2u * c), fh_crn_unit(seed, a, b, 2u * c + 1u));
}

static void add_ap(struct fh_world *w, double x, double y, uint8_t chan, uint8_t subnet, struct fh_rng *layout)
{
	struct fh_sim_ap *a = &w->ap[w->n_ap];

	a->x = x;
	a->y = y;
	a->chan = chan;
	a->subnet = subnet;
	a->bssid = 0x02005E000000ull | (uint64_t)(w->n_ap + 1); /* locally administered */
	a->load_pct = (uint8_t)(10 + fh_rng_unit(layout) * 60);
	w->n_ap++;
}

static void add_wp(struct fh_world *w, double x, double y)
{
	if (w->n_wp < FH_MAX_WAYPOINTS) {
		w->wx[w->n_wp] = x;
		w->wy[w->n_wp] = y;
		w->n_wp++;
	}
}

static void corridor(struct fh_world *w, struct fh_rng *layout)
{
	const double len = 400.0;

	w->spacing = 50.0;
	w->ple = 3.5;
	w->shadow_sigma = 4.0;
	w->shadow_dcorr = 8.0;
	for (int i = 0; i < 8; i++)
		add_ap(w, 25.0 + w->spacing * i, (i % 2) ? 3.0 : -3.0, plan[i % 3], (uint8_t)(i / 3), layout);

	/* walk to and fro with random turnaround points at least 150 m apart */
	double x = fh_rng_unit(&w->rng) < 0.5 ? 0.0 : len;

	add_wp(w, x, 0.0);
	for (int leg = 0; leg < 3; leg++) {
		double nx;

		do
			nx = fh_rng_unit(&w->rng) * len;
		while (fabs(nx - x) < 150.0);
		add_wp(w, nx, 0.0);
		x = nx;
	}
	w->speed = 1.4 * (0.85 + 0.3 * fh_rng_unit(&w->rng));
}

static void campus(struct fh_world *w, struct fh_rng *layout, bool vehicle)
{
	const int g = 4;

	w->spacing = 70.0;
	w->ple = 3.0;
	w->shadow_sigma = 5.0;
	w->shadow_dcorr = 15.0;
	for (int i = 0; i < g; i++)
		for (int j = 0; j < g; j++)
			add_ap(w, w->spacing * i + 6.0, w->spacing * j + 6.0, plan[(i + j) % 3],
			       (uint8_t)((i / 2) * 2 + j / 2), layout);

	/* random route along the street grid, no immediate U-turns */
	int ci = (int)(fh_rng_unit(&w->rng) * g), cj = (int)(fh_rng_unit(&w->rng) * g);
	int pi = -1, pj = -1;
	const int legs = vehicle ? 14 : 8;

	add_wp(w, ci * w->spacing, cj * w->spacing);
	for (int leg = 0; leg < legs; leg++) {
		static const int di[4] = { 1, -1, 0, 0 }, dj[4] = { 0, 0, 1, -1 };
		int opts[4], n = 0;

		for (int k = 0; k < 4; k++) {
			int ni = ci + di[k], nj = cj + dj[k];

			if (ni >= 0 && ni < g && nj >= 0 && nj < g && !(ni == pi && nj == pj))
				opts[n++] = k;
		}
		int k = opts[(int)(fh_rng_unit(&w->rng) * n)];

		pi = ci;
		pj = cj;
		ci += di[k];
		cj += dj[k];
		add_wp(w, ci * w->spacing, cj * w->spacing);
	}
	w->speed = (vehicle ? 8.0 : 1.4) * (0.85 + 0.3 * fh_rng_unit(&w->rng));
}

void fh_world_init(struct fh_world *w, enum fh_scenario sc, uint64_t layout_seed, uint64_t walk_seed)
{
	struct fh_rng layout;

	memset(w, 0, sizeof(*w));
	w->sc = sc;
	w->tx_dbm = 18.0;
	w->pl0_db = 40.0;
	w->rician_k = 4.0;
	w->chan_seed = fh_mix64(walk_seed ^ 0xC0FFEEull);
	fh_rng_seed(&layout, layout_seed);
	fh_rng_seed(&w->rng, walk_seed);

	if (sc == FH_SC_CORRIDOR_WALK)
		corridor(w, &layout);
	else
		campus(w, &layout, sc == FH_SC_CAMPUS_VEHICLE);

	for (int i = 0; i < w->n_ap; i++)
		w->ap[i].shadow_db = w->shadow_sigma * fh_rng_gauss(&w->rng);
	w->x = w->wx[0];
	w->y = w->wy[0];
	w->wp = 1;
}

void fh_world_step(struct fh_world *w, double dt_s)
{
	double left = w->speed * dt_s, moved = 0.0;

	while (left > 0.0 && w->wp < w->n_wp) {
		double dx = w->wx[w->wp] - w->x, dy = w->wy[w->wp] - w->y;
		double seg = sqrt(dx * dx + dy * dy);

		if (seg <= left) {
			w->x = w->wx[w->wp];
			w->y = w->wy[w->wp];
			w->wp++;
			left -= seg;
			moved += seg;
		} else {
			w->x += dx / seg * left;
			w->y += dy / seg * left;
			moved += left;
			left = 0.0;
		}
	}
	w->done = w->wp >= w->n_wp;

	/* Gudmundson model: correlation decays with distance travelled */
	double rho = exp(-moved / w->shadow_dcorr);
	double inno = sqrt(1.0 - rho * rho) * w->shadow_sigma;

	for (int i = 0; i < w->n_ap; i++)
		w->ap[i].shadow_db = rho * w->ap[i].shadow_db + inno * fh_rng_gauss(&w->rng);
}

double fh_world_mean_rssi(const struct fh_world *w, int ap)
{
	double dx = w->ap[ap].x - w->x, dy = w->ap[ap].y - w->y;
	double d = sqrt(dx * dx + dy * dy);

	if (d < 1.0)
		d = 1.0;
	return w->tx_dbm - w->pl0_db - 10.0 * w->ple * log10(d) + w->ap[ap].shadow_db;
}

int fh_world_rssi(const struct fh_world *w, int ap, fh_ms_t t, uint32_t kind)
{
	/* Rician fast fading, unit mean power */
	double k = w->rician_k;
	double los = sqrt(k / (k + 1.0)), sc = sqrt(1.0 / (2.0 * (k + 1.0)));
	double re = los + sc * fh_crn_gauss(w->chan_seed, (uint32_t)ap, t, FH_K_FADE * 8 + kind);
	double im = sc * fh_crn_gauss(w->chan_seed, (uint32_t)ap, t, FH_K_FADE * 8 + kind + 64);
	double p = re * re + im * im;

	if (p < 1e-6)
		p = 1e-6;
	return (int)lround(fh_world_mean_rssi(w, ap) + 10.0 * log10(p));
}

double fh_per(double snr_db)
{
	return 1.0 / (1.0 + exp((snr_db - 8.0) / 1.5));
}

void fh_world_ap_info(const struct fh_world *w, struct fh_ap_info *out)
{
	for (int i = 0; i < w->n_ap; i++) {
		out[i].bssid = w->ap[i].bssid;
		out[i].chan = w->ap[i].chan;
		out[i].subnet = w->ap[i].subnet;
	}
}
