/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "fh/fh_evring.h"
#include "fh_test.h"

static struct fh_event ev(uint8_t type, fh_ms_t ts)
{
	struct fh_event e;

	memset(&e, 0, sizeof(e));
	e.type = type;
	e.ts = ts;
	return e;
}

static void test_fifo_order_and_empty(void)
{
	static struct fh_evring r;
	struct fh_event out;

	fh_evring_init(&r);
	fh_evring_subscribe(&r, FH_EVGRP_ALL);
	CHECK_EQ(fh_evring_recv(&r, &out), -FH_EAGAIN);
	for (fh_ms_t i = 0; i < 5; i++) {
		struct fh_event e = ev(FH_EV_HANDOFF_START, i);

		CHECK_EQ(fh_evring_post(&r, &e), 0);
	}
	for (fh_ms_t i = 0; i < 5; i++) {
		CHECK_EQ(fh_evring_recv(&r, &out), 0);
		CHECK_EQ(out.ts, i);
	}
	CHECK_EQ(fh_evring_recv(&r, &out), -FH_EAGAIN);
}

static void test_wraps_around(void)
{
	static struct fh_evring r;
	struct fh_event out;
	fh_ms_t next = 0;

	fh_evring_init(&r);
	fh_evring_subscribe(&r, FH_EVGRP_ALL);
	for (fh_ms_t i = 0; i < 1000; i++) {
		struct fh_event e = ev(FH_EV_HANDOFF_DONE, i);

		CHECK_EQ(fh_evring_post(&r, &e), 0);
		if (i % 3 == 2)
			while (fh_evring_recv(&r, &out) == 0)
				CHECK_EQ(out.ts, next++);
	}
	while (fh_evring_recv(&r, &out) == 0)
		CHECK_EQ(out.ts, next++);
	CHECK_EQ(next, 1000);
	CHECK_EQ(r.dropped, 0);
}

static void test_overflow_reports_enobufs_once(void)
{
	static struct fh_evring r;
	struct fh_event out;
	int got = 0;

	fh_evring_init(&r);
	fh_evring_subscribe(&r, FH_EVGRP_ALL);
	for (fh_ms_t i = 0; i < FH_EVRING_SLOTS + 6; i++) {
		struct fh_event e = ev(FH_EV_LINK_DEGRADED, i);
		int rc = fh_evring_post(&r, &e);

		CHECK_EQ(rc, i < FH_EVRING_SLOTS ? 0 : -FH_ENOBUFS);
	}
	CHECK_EQ(r.dropped, 6);
	CHECK_EQ(fh_evring_recv(&r, &out), -FH_ENOBUFS);
	while (fh_evring_recv(&r, &out) == 0) {
		CHECK_EQ(out.ts, (fh_ms_t)got); /* oldest kept, newest dropped */
		got++;
	}
	CHECK_EQ(got, FH_EVRING_SLOTS);
}

static void test_group_filter(void)
{
	static struct fh_evring r;
	struct fh_event out;
	struct fh_event a = ev(FH_EV_HANDOFF_IMMINENT, 1), b = ev(FH_EV_HANDOFF_DONE, 2);

	fh_evring_init(&r);
	fh_evring_subscribe(&r, FH_EVGRP(FH_EV_HANDOFF_DONE));
	CHECK_EQ(fh_evring_post(&r, &a), 0);
	CHECK_EQ(fh_evring_post(&r, &b), 0);
	CHECK_EQ(fh_evring_recv(&r, &out), 0);
	CHECK_EQ(out.type, FH_EV_HANDOFF_DONE);
	CHECK_EQ(fh_evring_recv(&r, &out), -FH_EAGAIN);
}

static void test_names(void)
{
	CHECK(strcmp(fh_ev_name(FH_EV_HANDOFF_IMMINENT), "HANDOFF_IMMINENT") == 0);
	CHECK(strcmp(fh_ev_name(FH_EV_HANDOFF_DONE), "HANDOFF_DONE") == 0);
	CHECK(strcmp(fh_ev_name(FH_EV_LINK_DEGRADED), "LINK_DEGRADED") == 0);
	CHECK(strcmp(fh_ev_name(99), "UNKNOWN") == 0);
}

int main(void)
{
	printf("evring\n");
	RUN(test_fifo_order_and_empty);
	RUN(test_wraps_around);
	RUN(test_overflow_reports_enobufs_once);
	RUN(test_group_filter);
	RUN(test_names);
	TEST_DONE();
}
