/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "fh/fh_sm.h"
#include "fh_test.h"

static void test_handoff_path(void)
{
	struct fh_sm sm;
	static const enum fh_state path[] = {
		FH_ST_CONNECTED, FH_ST_ROAM_PREP, FH_ST_SCANNING, FH_ST_AUTH,
		FH_ST_REASSOC,   FH_ST_KEY,       FH_ST_CONNECTED,
	};

	fh_sm_init(&sm, 0);
	for (size_t i = 0; i < FH_ARRAY_SIZE(path); i++)
		CHECK_EQ(fh_sm_set(&sm, path[i], (fh_ms_t)(10 * i)), 0);
	CHECK_EQ(sm.state, FH_ST_CONNECTED);
	CHECK_EQ(sm.transitions, FH_ARRAY_SIZE(path));
	CHECK_EQ(sm.entered, 60);
	CHECK_EQ(sm.rejected, 0);
}

static void test_rejects_shortcuts(void)
{
	struct fh_sm sm;

	fh_sm_init(&sm, 0);
	CHECK_EQ(fh_sm_set(&sm, FH_ST_KEY, 1), -FH_EINVAL);
	CHECK_EQ(sm.state, FH_ST_DISCONNECTED);
	fh_sm_set(&sm, FH_ST_CONNECTED, 2);
	CHECK_EQ(fh_sm_set(&sm, FH_ST_AUTH, 3), -FH_EINVAL);   /* auth needs a scan result */
	CHECK_EQ(fh_sm_set(&sm, FH_ST_CONNECTED, 4), -FH_EINVAL); /* no self loops */
	CHECK_EQ(sm.state, FH_ST_CONNECTED);
	CHECK_EQ(sm.rejected, 3);
}

static void test_table_properties(void)
{
	int n = 0;

	for (int a = 0; a < FH_ST__COUNT; a++) {
		CHECK(!fh_sm_allowed((enum fh_state)a, (enum fh_state)a));
		if (a != FH_ST_DISCONNECTED)
			CHECK(fh_sm_allowed((enum fh_state)a, FH_ST_DISCONNECTED));
		for (int b = 0; b < FH_ST__COUNT; b++)
			n += fh_sm_allowed((enum fh_state)a, (enum fh_state)b);
	}
	CHECK_EQ(n, 17);
	CHECK(fh_sm_allowed(FH_ST_SCANNING, FH_ST_CONNECTED)); /* aborted scan */
	CHECK(fh_sm_allowed(FH_ST_ROAM_PREP, FH_ST_CONNECTED)); /* cancelled */
	CHECK(!fh_sm_allowed(FH_ST_ROAM_PREP, FH_ST_AUTH));
	CHECK(!fh_sm_allowed((enum fh_state)42, FH_ST_CONNECTED));
	CHECK(fh_state_associated(FH_ST_ROAM_PREP));
	CHECK(!fh_state_associated(FH_ST_KEY));
	CHECK(strcmp(fh_state_name(FH_ST_REASSOC), "REASSOC") == 0);
}

int main(void)
{
	printf("sm\n");
	RUN(test_handoff_path);
	RUN(test_rejects_shortcuts);
	RUN(test_table_properties);
	TEST_DONE();
}
