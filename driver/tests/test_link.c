/* SPDX-License-Identifier: MIT */
#include "fh/fh_link.h"
#include "fh_test.h"

static void test_reset_seeds_levels(void)
{
	struct fh_link l;

	fh_link_reset(&l, 0, -60);
	CHECK_EQ(fh_link_rssi(&l), -60);
	CHECK_EQ(fh_link_snr(&l), -60 - FH_NOISE_DBM);
	CHECK_EQ(fh_link_retry_permille(&l), 0);
	fh_link_reset(&l, 0, FH_RSSI_FLOOR);
	CHECK_EQ(fh_link_rssi(&l), FH_RSSI_FLOOR);
}

static void test_retry_rate_window(void)
{
	struct fh_link l;

	fh_link_reset(&l, 0, -60);
	for (int i = 0; i < 10; i++)
		fh_link_tx(&l, i % 10 >= 3);
	CHECK_EQ(fh_link_retry_permille(&l), 300);
	/* only the last 64 transmissions count */
	for (int i = 0; i < 64; i++)
		fh_link_tx(&l, true);
	CHECK_EQ(fh_link_retry_permille(&l), 0);
	for (int i = 0; i < 16; i++)
		fh_link_tx(&l, false);
	CHECK_EQ(fh_link_retry_permille(&l), 250);
}

static void test_beacon_loss(void)
{
	struct fh_link l;

	fh_link_reset(&l, 0, -60);
	for (int i = 0; i < 6; i++)
		fh_link_beacon(&l, (fh_ms_t)(100 * i), false, 0);
	CHECK(!fh_link_beacon_lost(&l, 7));
	fh_link_beacon(&l, 700, false, 0);
	CHECK(fh_link_beacon_lost(&l, 7));
	fh_link_beacon(&l, 800, true, -70);
	CHECK(!fh_link_beacon_lost(&l, 7));
	CHECK_EQ(l.last_rx, 800);
}

int main(void)
{
	printf("link\n");
	RUN(test_reset_seeds_levels);
	RUN(test_retry_rate_window);
	RUN(test_beacon_loss);
	TEST_DONE();
}
