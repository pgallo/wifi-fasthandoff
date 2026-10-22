/* SPDX-License-Identifier: MIT */
#ifndef FH_KALMAN_H
#define FH_KALMAN_H

#include "fh/fh_types.h"

/*
 * Two-state Kalman filter over [RSSI, slope] with a constant-velocity model.
 * Measurements are raw per-frame RSSI (fast fading included); the filter
 * separates the trend that walking or driving produces from the fading.
 */
struct fh_kf {
	double r;          /* dBm */
	double v;          /* dB per second */
	double p00, p01, p11;
	double q;          /* process noise: slope random walk, (dB/s)^2 per second */
	double rvar;       /* measurement noise variance, dB^2 */
	fh_ms_t t;
	bool valid;
};

void fh_kf_init(struct fh_kf *kf, double q, double rvar);
void fh_kf_update(struct fh_kf *kf, fh_ms_t now, double z);
/* Predicted RSSI at now + horizon_ms. */
double fh_kf_forecast(const struct fh_kf *kf, fh_ms_t now, uint32_t horizon_ms);
double fh_kf_slope(const struct fh_kf *kf);

#endif
