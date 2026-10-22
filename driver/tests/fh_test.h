/* SPDX-License-Identifier: MIT */
/* A minimal test harness: checks count failures, RUN prints one line per test. */
#ifndef FH_TEST_H
#define FH_TEST_H

#include <math.h>
#include <stdio.h>

static int fh_t_checks;
static int fh_t_failures;

#define CHECK(cond)                                                                      \
	do {                                                                             \
		fh_t_checks++;                                                           \
		if (!(cond)) {                                                           \
			fh_t_failures++;                                                 \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
		}                                                                        \
	} while (0)

#define CHECK_EQ(a, b)                                                                              \
	do {                                                                                        \
		long long fh_a_ = (long long)(a), fh_b_ = (long long)(b);                           \
		fh_t_checks++;                                                                      \
		if (fh_a_ != fh_b_) {                                                               \
			fh_t_failures++;                                                            \
			fprintf(stderr, "%s:%d: %s == %s failed (%lld != %lld)\n", __FILE__, __LINE__, \
				#a, #b, fh_a_, fh_b_);                                              \
		}                                                                                   \
	} while (0)

#define CHECK_NEAR(a, b, eps)                                                                       \
	do {                                                                                        \
		double fh_a_ = (double)(a), fh_b_ = (double)(b);                                    \
		fh_t_checks++;                                                                      \
		if (fabs(fh_a_ - fh_b_) > (eps)) {                                                  \
			fh_t_failures++;                                                            \
			fprintf(stderr, "%s:%d: %s ~ %s failed (%g vs %g, eps %g)\n", __FILE__, __LINE__, \
				#a, #b, fh_a_, fh_b_, (double)(eps));                               \
		}                                                                                   \
	} while (0)

#define RUN(fn)                                                                  \
	do {                                                                     \
		int fh_before_ = fh_t_failures;                                  \
		fn();                                                            \
		printf("  %-44s %s\n", #fn, fh_before_ == fh_t_failures ? "ok" : "FAIL"); \
	} while (0)

#define TEST_DONE()                                                                      \
	do {                                                                             \
		printf("%d checks, %d failures\n", fh_t_checks, fh_t_failures);          \
		return fh_t_failures ? 1 : 0;                                            \
	} while (0)

#endif
