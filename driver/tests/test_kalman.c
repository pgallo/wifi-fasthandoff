/* SPDX-License-Identifier: MIT */
#include "fh/fh_kalman.h"
#include "fh_test.h"
#include "fh_world.h"

static void test_first_update_initialises(void)
{
	struct fh_kf kf;

	fh_kf_init(&kf, 2.0, 16.0);
	fh_kf_update(&kf, 100, -60.0);
	CHECK(kf.valid);
	CHECK_NEAR(kf.r, -60.0, 1e-9);
	CHECK_NEAR(fh_kf_slope(&kf), 0.0, 1e-9);
	CHECK_NEAR(fh_kf_forecast(&kf, 100, 1000), -60.0, 1e-9);
}

static void test_tracks_noisy_ramp(void)
{
	struct fh_kf kf;
	struct fh_rng rng;

	fh_kf_init(&kf, 2.0, 16.0);
	fh_rng_seed(&rng, 42);
	/* walking away from an AP: -3 dB/s, 4 dB of fading noise, one frame per 20 ms */
	for (fh_ms_t t = 0; t <= 6000; t += 20) {
		double truth = -50.0 - 3.0 * t / 1000.0;

		fh_kf_update(&kf, t, truth + 4.0 * fh_rng_gauss(&rng));
	}
	CHECK_NEAR(fh_kf_slope(&kf), -3.0, 0.8);
	CHECK_NEAR(fh_kf_forecast(&kf, 6000, 1000), -50.0 - 3.0 * 7.0, 2.5);
}

static void test_stationary_noise_has_no_slope(void)
{
	struct fh_kf kf;
	struct fh_rng rng;
	double raw_err = 0.0, kf_err = 0.0;
	int n = 0;

	fh_kf_init(&kf, 2.0, 16.0);
	fh_rng_seed(&rng, 7);
	for (fh_ms_t t = 0; t <= 10000; t += 20) {
		double z = -65.0 + 4.0 * fh_rng_gauss(&rng);

		fh_kf_update(&kf, t, z);
		if (t > 2000) {
			raw_err += (z + 65.0) * (z + 65.0);
			kf_err += (kf.r + 65.0) * (kf.r + 65.0);
			n++;
		}
	}
	CHECK_NEAR(fh_kf_slope(&kf), 0.0, 1.0);
	CHECK_NEAR(kf.r, -65.0, 2.0);
	/* the filter must remove most of the fading */
	CHECK(kf_err / n < 0.25 * raw_err / n);
}

static void test_forecast_extrapolates_from_last_update(void)
{
	struct fh_kf kf;

	fh_kf_init(&kf, 2.0, 1.0);
	for (fh_ms_t t = 0; t <= 4000; t += 20)
		fh_kf_update(&kf, t, -60.0 - 5.0 * t / 1000.0);
	double at_last = fh_kf_forecast(&kf, 4000, 0);
	double later = fh_kf_forecast(&kf, 4500, 500);

	CHECK_NEAR(at_last, -80.0, 0.5);
	CHECK_NEAR(later, -85.0, 0.8);
}

int main(void)
{
	printf("kalman\n");
	RUN(test_first_update_initialises);
	RUN(test_tracks_noisy_ramp);
	RUN(test_stationary_noise_has_no_slope);
	RUN(test_forecast_extrapolates_from_last_update);
	TEST_DONE();
}
