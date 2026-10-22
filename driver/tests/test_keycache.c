/* SPDX-License-Identifier: MIT */
#include "fh/fh_keycache.h"
#include "fh_test.h"

static void test_derivation_binds_bssid_and_station(void)
{
	struct fh_keycache a, b;

	fh_kc_init(&a, 0x1111, 0xABCD, 30000);
	fh_kc_init(&b, 0x2222, 0xABCD, 30000);
	CHECK_EQ(fh_kc_derive(&a, 0x10), fh_kc_derive(&a, 0x10));
	CHECK(fh_kc_derive(&a, 0x10) != fh_kc_derive(&a, 0x11));
	CHECK(fh_kc_derive(&a, 0x10) != fh_kc_derive(&b, 0x10));
	CHECK(fh_hash32("abc", 3, 0) != fh_hash32("abd", 3, 0));
}

static void test_preauth_ready_time(void)
{
	struct fh_keycache kc;

	fh_kc_init(&kc, 1, 2, 30000);
	fh_kc_install(&kc, 0xAA, 1000, 1060);
	CHECK(fh_kc_present(&kc, 0xAA, 1000));
	CHECK(!fh_kc_ready(&kc, 0xAA, 1059));
	CHECK(!fh_kc_lookup(&kc, 0xAA, 1059));
	CHECK(fh_kc_lookup(&kc, 0xAA, 1060));
	CHECK_EQ(kc.hits, 1);
	CHECK_EQ(kc.misses, 1);
}

static void test_expiry(void)
{
	struct fh_keycache kc;

	fh_kc_init(&kc, 1, 2, 5000);
	fh_kc_install(&kc, 0xAA, 0, 0);
	CHECK(fh_kc_lookup(&kc, 0xAA, 4999));
	CHECK(!fh_kc_present(&kc, 0xAA, 5000));
	CHECK(!fh_kc_lookup(&kc, 0xAA, 5000));
	CHECK(!fh_kc_present(&kc, 0xAA, 0)); /* the expired entry was released */
}

static void test_tampered_key_is_rejected(void)
{
	struct fh_keycache kc;

	fh_kc_init(&kc, 1, 2, 30000);
	fh_kc_install(&kc, 0xAA, 0, 0);
	kc.slot[0].key ^= 1;
	CHECK(!fh_kc_lookup(&kc, 0xAA, 10));
}

static void test_capacity_replaces_oldest(void)
{
	struct fh_keycache kc;
	int present = 0;

	fh_kc_init(&kc, 1, 2, 30000);
	for (uint64_t i = 0; i < FH_KC_SLOTS + 4; i++)
		fh_kc_install(&kc, 0x100 + i, (fh_ms_t)(i * 10), (fh_ms_t)(i * 10));
	for (uint64_t i = 0; i < FH_KC_SLOTS + 4; i++)
		present += fh_kc_present(&kc, 0x100 + i, 500);
	CHECK_EQ(present, FH_KC_SLOTS);
	CHECK(!fh_kc_present(&kc, 0x100, 500));
	CHECK(fh_kc_present(&kc, 0x100 + FH_KC_SLOTS + 3, 500));
}

int main(void)
{
	printf("keycache\n");
	RUN(test_derivation_binds_bssid_and_station);
	RUN(test_preauth_ready_time);
	RUN(test_expiry);
	RUN(test_tampered_key_is_rejected);
	RUN(test_capacity_replaces_oldest);
	TEST_DONE();
}
