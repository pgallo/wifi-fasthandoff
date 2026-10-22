/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "fh/fh_keycache.h"

/* FNV-1a. A stand-in for the key derivation function, not cryptography. */
uint32_t fh_hash32(const void *buf, size_t len, uint32_t seed)
{
	const uint8_t *p = buf;
	uint32_t h = 2166136261u ^ seed;

	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

void fh_kc_init(struct fh_keycache *kc, uint64_t sta, uint32_t root_key, uint32_t lifetime_ms)
{
	memset(kc, 0, sizeof(*kc));
	kc->sta = sta;
	kc->root_key = root_key;
	kc->lifetime_ms = lifetime_ms;
}

uint32_t fh_kc_derive(const struct fh_keycache *kc, uint64_t bssid)
{
	uint64_t in[2] = { bssid, kc->sta };

	return fh_hash32(in, sizeof(in), kc->root_key);
}

static struct fh_pmksa *kc_find(const struct fh_keycache *kc, uint64_t bssid)
{
	for (int i = 0; i < FH_KC_SLOTS; i++)
		if (kc->slot[i].valid && kc->slot[i].bssid == bssid)
			return (struct fh_pmksa *)&kc->slot[i];
	return NULL;
}

void fh_kc_install(struct fh_keycache *kc, uint64_t bssid, fh_ms_t now, fh_ms_t ready_at)
{
	struct fh_pmksa *e = kc_find(kc, bssid);

	if (!e) {
		/* free slot first, otherwise the one closest to expiry */
		e = &kc->slot[0];
		for (int i = 0; i < FH_KC_SLOTS; i++) {
			if (!kc->slot[i].valid) {
				e = &kc->slot[i];
				break;
			}
			if (kc->slot[i].expires < e->expires)
				e = &kc->slot[i];
		}
	}
	e->bssid = bssid;
	e->key = fh_kc_derive(kc, bssid);
	e->ready_at = ready_at;
	e->expires = now + kc->lifetime_ms;
	e->valid = true;
}

bool fh_kc_present(const struct fh_keycache *kc, uint64_t bssid, fh_ms_t now)
{
	const struct fh_pmksa *e = kc_find(kc, bssid);

	return e && now < e->expires;
}

bool fh_kc_ready(const struct fh_keycache *kc, uint64_t bssid, fh_ms_t now)
{
	const struct fh_pmksa *e = kc_find(kc, bssid);

	return e && now >= e->ready_at && now < e->expires;
}

bool fh_kc_lookup(struct fh_keycache *kc, uint64_t bssid, fh_ms_t now)
{
	struct fh_pmksa *e = kc_find(kc, bssid);

	if (e && now >= e->expires)
		e->valid = false;
	if (!e || !e->valid || now < e->ready_at || e->key != fh_kc_derive(kc, bssid)) {
		kc->misses++;
		return false;
	}
	kc->hits++;
	return true;
}
