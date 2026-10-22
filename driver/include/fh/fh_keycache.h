/* SPDX-License-Identifier: MIT */
#ifndef FH_KEYCACHE_H
#define FH_KEYCACHE_H

#include "fh/fh_types.h"

/*
 * Key cache for fast transitions, modelled abstractly: a top-level key held
 * by the station is bound to each target BSSID with a hash (a stand-in for
 * the 802.11r key hierarchy). An entry becomes usable at ready_at, which is
 * how pre-authentication over the distribution system is represented.
 */
#define FH_KC_SLOTS 16

struct fh_pmksa {
	uint64_t bssid;
	uint32_t key;
	fh_ms_t ready_at;
	fh_ms_t expires;
	bool valid;
};

struct fh_keycache {
	struct fh_pmksa slot[FH_KC_SLOTS];
	uint64_t sta;
	uint32_t root_key;
	uint32_t lifetime_ms;
	uint32_t hits;
	uint32_t misses;
};

uint32_t fh_hash32(const void *buf, size_t len, uint32_t seed);

void fh_kc_init(struct fh_keycache *kc, uint64_t sta, uint32_t root_key, uint32_t lifetime_ms);
uint32_t fh_kc_derive(const struct fh_keycache *kc, uint64_t bssid);
void fh_kc_install(struct fh_keycache *kc, uint64_t bssid, fh_ms_t now, fh_ms_t ready_at);
/* true if an entry exists, ready or pending */
bool fh_kc_present(const struct fh_keycache *kc, uint64_t bssid, fh_ms_t now);
/* true if an entry is ready now (no hit/miss accounting) */
bool fh_kc_ready(const struct fh_keycache *kc, uint64_t bssid, fh_ms_t now);
/* true if a ready, unexpired, consistent entry exists */
bool fh_kc_lookup(struct fh_keycache *kc, uint64_t bssid, fh_ms_t now);

#endif
