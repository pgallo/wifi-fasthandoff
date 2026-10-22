/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "fh/fh_evring.h"

void fh_evring_init(struct fh_evring *r)
{
	memset(r->slot, 0, sizeof(r->slot));
	atomic_init(&r->head, 0);
	atomic_init(&r->tail, 0);
	atomic_init(&r->overrun, false);
	r->groups = 0;
	r->dropped = 0;
}

void fh_evring_subscribe(struct fh_evring *r, uint32_t groups)
{
	r->groups = groups;
}

int fh_evring_post(struct fh_evring *r, const struct fh_event *ev)
{
	if (!(r->groups & FH_EVGRP(ev->type)))
		return 0;

	uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
	uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

	if (head - tail >= FH_EVRING_SLOTS) {
		r->dropped++;
		atomic_store_explicit(&r->overrun, true, memory_order_release);
		return -FH_ENOBUFS;
	}
	r->slot[head & (FH_EVRING_SLOTS - 1)] = *ev;
	atomic_store_explicit(&r->head, head + 1, memory_order_release);
	return 0;
}

int fh_evring_recv(struct fh_evring *r, struct fh_event *ev)
{
	if (atomic_exchange_explicit(&r->overrun, false, memory_order_acq_rel))
		return -FH_ENOBUFS;

	uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
	uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);

	if (tail == head)
		return -FH_EAGAIN;
	*ev = r->slot[tail & (FH_EVRING_SLOTS - 1)];
	atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
	return 0;
}

const char *fh_ev_name(unsigned type)
{
	switch (type) {
	case FH_EV_LINK_DEGRADED:
		return "LINK_DEGRADED";
	case FH_EV_HANDOFF_IMMINENT:
		return "HANDOFF_IMMINENT";
	case FH_EV_HANDOFF_START:
		return "HANDOFF_START";
	case FH_EV_HANDOFF_DONE:
		return "HANDOFF_DONE";
	default:
		return "UNKNOWN";
	}
}
