/* SPDX-License-Identifier: MIT */
#ifndef FH_EVRING_H
#define FH_EVRING_H

#include <stdatomic.h>

#include "fh/fh_types.h"

/*
 * Driver to user-space event channel. A single-producer single-consumer ring
 * with multicast-group style subscription. On overflow the newest event is
 * dropped and the next receive reports -FH_ENOBUFS once, the way a netlink
 * socket tells a slow reader that it lost messages.
 */
enum fh_ev_type {
	FH_EV_LINK_DEGRADED = 1,
	FH_EV_HANDOFF_IMMINENT = 2,
	FH_EV_HANDOFF_START = 3,
	FH_EV_HANDOFF_DONE = 4,
};

#define FH_EVGRP(t)  (1u << (t))
#define FH_EVGRP_ALL (FH_EVGRP(1) | FH_EVGRP(2) | FH_EVGRP(3) | FH_EVGRP(4))

#define FH_EVF_ABORTED       0x01 /* scan found nothing better, back on the old AP */
#define FH_EVF_SUBNET_CHANGE 0x02 /* new AP is on another subnet: IP changes */
#define FH_EVF_FT            0x04 /* fast transition with a cached key */
#define FH_EVF_REACTIVE      0x08 /* forced by beacon loss or emergency level */

struct fh_event {
	uint8_t type;
	uint8_t flags;
	fh_ap_t from;
	fh_ap_t to;
	fh_ms_t ts;
	uint16_t eta_ms;
	int16_t rssi;
	uint16_t retry_permille;
	uint8_t subnet;
	struct fh_ho_latency lat;
};

#define FH_EVRING_ORDER 6
#define FH_EVRING_SLOTS (1u << FH_EVRING_ORDER)

struct fh_evring {
	struct fh_event slot[FH_EVRING_SLOTS];
	_Atomic uint32_t head; /* written by the producer */
	_Atomic uint32_t tail; /* written by the consumer */
	atomic_bool overrun;
	uint32_t groups;
	uint32_t dropped;
};

void fh_evring_init(struct fh_evring *r);
void fh_evring_subscribe(struct fh_evring *r, uint32_t groups);
/* Producer side. Returns 0, or -FH_ENOBUFS when the event was dropped. */
int fh_evring_post(struct fh_evring *r, const struct fh_event *ev);
/* Consumer side. Returns 0, -FH_EAGAIN when empty, -FH_ENOBUFS once after an overrun. */
int fh_evring_recv(struct fh_evring *r, struct fh_event *ev);
const char *fh_ev_name(unsigned type);

#endif
