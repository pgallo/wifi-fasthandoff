/* SPDX-License-Identifier: MIT */
#include "fh/fh_kalman.h"

void fh_kf_init(struct fh_kf *kf, double q, double rvar)
{
	kf->r = 0.0;
	kf->v = 0.0;
	kf->p00 = kf->p01 = kf->p11 = 0.0;
	kf->q = q;
	kf->rvar = rvar;
	kf->t = 0;
	kf->valid = false;
}

void fh_kf_update(struct fh_kf *kf, fh_ms_t now, double z)
{
	if (!kf->valid) {
		kf->r = z;
		kf->v = 0.0;
		kf->p00 = kf->rvar;
		kf->p01 = 0.0;
		kf->p11 = 4.0; /* prior: slope within a few dB/s */
		kf->t = now;
		kf->valid = true;
		return;
	}

	/* predict: x = F x, P = F P F' + Q, F = [1 dt; 0 1] */
	double dt = (double)(now - kf->t) / 1000.0;
	double dt2 = dt * dt;

	kf->r += kf->v * dt;
	kf->p00 += 2.0 * dt * kf->p01 + dt2 * kf->p11 + kf->q * dt2 * dt / 3.0;
	kf->p01 += dt * kf->p11 + kf->q * dt2 / 2.0;
	kf->p11 += kf->q * dt;
	kf->t = now;

	/* update with H = [1 0] */
	double s = kf->p00 + kf->rvar;
	double k0 = kf->p00 / s;
	double k1 = kf->p01 / s;
	double y = z - kf->r;

	kf->r += k0 * y;
	kf->v += k1 * y;
	kf->p11 -= k1 * kf->p01;
	kf->p01 *= (1.0 - k0);
	kf->p00 *= (1.0 - k0);
}

double fh_kf_forecast(const struct fh_kf *kf, fh_ms_t now, uint32_t horizon_ms)
{
	double ahead = ((double)(now - kf->t) + (double)horizon_ms) / 1000.0;

	return kf->r + kf->v * ahead;
}

double fh_kf_slope(const struct fh_kf *kf)
{
	return kf->v;
}
