/* SPDX-License-Identifier: MIT */
#ifndef FH_SM_H
#define FH_SM_H

#include "fh/fh_types.h"

enum fh_state {
	FH_ST_DISCONNECTED,
	FH_ST_CONNECTED,
	FH_ST_ROAM_PREP, /* handoff imminent: scanning harder, pre-authenticating */
	FH_ST_SCANNING,  /* data path down from here until CONNECTED */
	FH_ST_AUTH,
	FH_ST_REASSOC,
	FH_ST_KEY,
	FH_ST__COUNT,
};

struct fh_sm {
	enum fh_state state;
	fh_ms_t entered;
	uint32_t transitions;
	uint32_t rejected;
};

void fh_sm_init(struct fh_sm *sm, fh_ms_t now);
bool fh_sm_allowed(enum fh_state from, enum fh_state to);
/* Returns 0 or -FH_EINVAL for a transition the table does not allow. */
int fh_sm_set(struct fh_sm *sm, enum fh_state to, fh_ms_t now);
const char *fh_state_name(enum fh_state s);

static inline bool fh_state_associated(enum fh_state s)
{
	return s == FH_ST_CONNECTED || s == FH_ST_ROAM_PREP;
}

#endif
