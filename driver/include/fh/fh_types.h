/* SPDX-License-Identifier: MIT */
#ifndef FH_TYPES_H
#define FH_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t fh_ms_t; /* monotonic driver time in milliseconds */
typedef uint8_t fh_ap_t;  /* index into the driver's BSS table */

#define FH_AP_NONE      ((fh_ap_t)0xFF)
#define FH_MAX_APS      24
#define FH_MAX_CHANNELS 14
#define FH_RSSI_FLOOR   (-100)
#define FH_NOISE_DBM    (-95)

#define FH_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* errno-style codes; functions return them negated, like kernel APIs */
#define FH_ENOENT  2
#define FH_EAGAIN  11
#define FH_EINVAL  22
#define FH_ENOBUFS 105

/* Where the time of one handoff went. */
struct fh_ho_latency {
	uint16_t scan_ms;
	uint16_t auth_ms;
	uint16_t reassoc_ms;
	uint16_t key_ms;
};

static inline uint32_t fh_ho_total(const struct fh_ho_latency *l)
{
	return (uint32_t)l->scan_ms + l->auth_ms + l->reassoc_ms + l->key_ms;
}

#endif
