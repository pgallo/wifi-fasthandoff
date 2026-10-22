/* SPDX-License-Identifier: MIT */
#include "fh/fh_ewma.h"
#include "fh_test.h"

static void test_first_sample_initialises(void)
{
	struct fh_ewma e;

	fh_ewma_init(&e, 3);
	CHECK(!e.valid);
	fh_ewma_add(&e, -62);
	CHECK(e.valid);
	CHECK_EQ(fh_ewma_read(&e), -62);
	CHECK_EQ(fh_ewma_read_q(&e), -62 * 1024);
}

static void test_constant_input_is_exact(void)
{
	struct fh_ewma e;

	fh_ewma_init(&e, 3);
	for (int i = 0; i < 1000; i++)
		fh_ewma_add(&e, -71);
	CHECK_EQ(fh_ewma_read_q(&e), -71 * 1024);
}

static void test_step_response(void)
{
	struct fh_ewma e;

	fh_ewma_init(&e, 3);
	fh_ewma_add(&e, -50);
	fh_ewma_add(&e, -80);
	/* -50 + (-30) / 8 = -53.75 */
	CHECK_EQ(fh_ewma_read_q(&e), -50 * 1024 - 30 * 1024 / 8);
	CHECK_EQ(fh_ewma_read(&e), -54);
	for (int i = 0; i < 200; i++)
		fh_ewma_add(&e, -80);
	CHECK_EQ(fh_ewma_read(&e), -80);
}

static void test_weight_shift(void)
{
	struct fh_ewma slow, fast;

	fh_ewma_init(&slow, 4);
	fh_ewma_init(&fast, 1);
	fh_ewma_add(&slow, 0);
	fh_ewma_add(&fast, 0);
	fh_ewma_add(&slow, 32);
	fh_ewma_add(&fast, 32);
	CHECK_EQ(fh_ewma_read(&slow), 2);
	CHECK_EQ(fh_ewma_read(&fast), 16);
}

static void test_rounding_is_symmetric(void)
{
	struct fh_ewma e = { .q = -50 * 1024 - 512, .wshift = 3, .valid = true };

	CHECK_EQ(fh_ewma_read(&e), -51);
	e.q = 50 * 1024 + 512;
	CHECK_EQ(fh_ewma_read(&e), 51);
	e.q = -50 * 1024 - 511;
	CHECK_EQ(fh_ewma_read(&e), -50);
}

int main(void)
{
	printf("ewma\n");
	RUN(test_first_sample_initialises);
	RUN(test_constant_input_is_exact);
	RUN(test_step_response);
	RUN(test_weight_shift);
	RUN(test_rounding_is_symmetric);
	TEST_DONE();
}
