/* SPDX-License-Identifier: MIT */
#include "fh/fh_sm.h"

#define T true

/* allowed[from][to]; anything not listed is a driver bug and is rejected */
static const bool allowed[FH_ST__COUNT][FH_ST__COUNT] = {
	[FH_ST_DISCONNECTED] = { [FH_ST_SCANNING] = T, [FH_ST_CONNECTED] = T },
	[FH_ST_CONNECTED] = { [FH_ST_ROAM_PREP] = T, [FH_ST_SCANNING] = T, [FH_ST_DISCONNECTED] = T },
	[FH_ST_ROAM_PREP] = { [FH_ST_CONNECTED] = T, [FH_ST_SCANNING] = T, [FH_ST_DISCONNECTED] = T },
	[FH_ST_SCANNING] = { [FH_ST_AUTH] = T, [FH_ST_CONNECTED] = T, [FH_ST_DISCONNECTED] = T },
	[FH_ST_AUTH] = { [FH_ST_REASSOC] = T, [FH_ST_DISCONNECTED] = T },
	[FH_ST_REASSOC] = { [FH_ST_KEY] = T, [FH_ST_DISCONNECTED] = T },
	[FH_ST_KEY] = { [FH_ST_CONNECTED] = T, [FH_ST_DISCONNECTED] = T },
};

void fh_sm_init(struct fh_sm *sm, fh_ms_t now)
{
	sm->state = FH_ST_DISCONNECTED;
	sm->entered = now;
	sm->transitions = 0;
	sm->rejected = 0;
}

bool fh_sm_allowed(enum fh_state from, enum fh_state to)
{
	if ((unsigned)from >= FH_ST__COUNT || (unsigned)to >= FH_ST__COUNT)
		return false;
	return allowed[from][to];
}

int fh_sm_set(struct fh_sm *sm, enum fh_state to, fh_ms_t now)
{
	if (!fh_sm_allowed(sm->state, to)) {
		sm->rejected++;
		return -FH_EINVAL;
	}
	sm->state = to;
	sm->entered = now;
	sm->transitions++;
	return 0;
}

const char *fh_state_name(enum fh_state s)
{
	static const char *const names[FH_ST__COUNT] = {
		"DISCONNECTED", "CONNECTED", "ROAM_PREP", "SCANNING", "AUTH", "REASSOC", "KEY",
	};

	return (unsigned)s < FH_ST__COUNT ? names[s] : "?";
}
