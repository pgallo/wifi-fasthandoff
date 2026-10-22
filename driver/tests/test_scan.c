/* SPDX-License-Identifier: MIT */
#include "fh/fh_scan.h"
#include "fh_test.h"

static void test_budget_limits_offchannel_time(void)
{
	struct fh_scan s;
	static const uint8_t ch[] = { 1, 6, 11 };

	fh_scan_init(&s, 40, 500, 2000);
	fh_scan_set_channels(&s, ch, 3);
	CHECK_EQ(fh_scan_next(&s, 1000, 20), 1);
	fh_scan_charge(&s, 20);
	CHECK_EQ(fh_scan_next(&s, 1020, 20), 6);
	fh_scan_charge(&s, 20);
	CHECK_EQ(fh_scan_next(&s, 1040, 20), -FH_EAGAIN);
	CHECK_EQ(fh_scan_next(&s, 1499, 20), -FH_EAGAIN);
	CHECK_EQ(fh_scan_next(&s, 1500, 20), 11); /* new window */
	fh_scan_charge(&s, 20);
	CHECK_EQ(fh_scan_next(&s, 1520, 20), 1);  /* round robin */
	CHECK_EQ(s.offchan_total_ms, 60);
}

static void test_channel_list_dedup_and_priority(void)
{
	struct fh_scan s;
	static const uint8_t ch[] = { 1, 6, 6, 11, 1 };
	static const uint8_t pri[] = { 11, 11 };

	fh_scan_init(&s, 40, 500, 2000);
	CHECK_EQ(fh_scan_next(&s, 0, 20), -FH_ENOENT);
	fh_scan_set_channels(&s, ch, 5);
	CHECK_EQ(s.n_chan, 3);
	fh_scan_prioritise(&s, pri, 2);
	CHECK_EQ(s.n_chan, 3);
	CHECK_EQ(s.chan[0], 11);
	CHECK_EQ(s.chan[1], 1);
	CHECK_EQ(s.chan[2], 6);
}

static void test_aging_and_freshness(void)
{
	struct fh_scan s;

	fh_scan_init(&s, 40, 500, 2000);
	fh_scan_update(&s, 1000, 3, 6, -70, 20);
	CHECK(fh_scan_get(&s, 3, 1500, 600) != NULL);
	CHECK(fh_scan_get(&s, 3, 1700, 600) == NULL);
	fh_scan_age(&s, 3000);
	CHECK(s.cache[3].valid);
	fh_scan_age(&s, 3001);
	CHECK(!s.cache[3].valid);
	CHECK(fh_scan_get(&s, FH_AP_NONE, 3001, 600) == NULL);
}

static void test_smoothing_and_slope(void)
{
	struct fh_scan s;

	fh_scan_init(&s, 40, 500, 2000);
	fh_scan_update(&s, 0, 2, 1, -60, 0);
	CHECK_EQ(fh_cand_slope(&s.cache[2]), 0);
	fh_scan_update(&s, 1000, 2, 1, -70, 0);
	CHECK_EQ(s.cache[2].rssi, -65); /* averaged with the previous sample */
	CHECK_EQ(fh_cand_slope(&s.cache[2]), -5);
	/* after the entry expired a new sample starts over */
	fh_scan_update(&s, 5000, 2, 1, -80, 0);
	CHECK_EQ(s.cache[2].rssi, -80);
}

static void test_best_uses_load_penalty(void)
{
	struct fh_scan s;
	int score = 0;

	fh_scan_init(&s, 40, 500, 2000);
	fh_scan_update(&s, 0, 1, 1, -60, 80); /* strong but busy: -68 */
	fh_scan_update(&s, 0, 2, 6, -63, 0);  /* -63 */
	fh_scan_update(&s, 0, 3, 11, -50, 0); /* excluded: current AP */
	CHECK_EQ(fh_scan_best(&s, 100, 600, 3, 10, &score), 2);
	CHECK_EQ(score, -63);
	CHECK_EQ(fh_scan_best(&s, 100, 600, 3, 0, &score), 1);
	CHECK_EQ(fh_scan_best(&s, 1000, 600, 3, 10, &score), FH_AP_NONE);
}

int main(void)
{
	printf("scan\n");
	RUN(test_budget_limits_offchannel_time);
	RUN(test_channel_list_dedup_and_priority);
	RUN(test_aging_and_freshness);
	RUN(test_smoothing_and_slope);
	RUN(test_best_uses_load_penalty);
	TEST_DONE();
}
