/* SPDX-License-Identifier: MIT */
#ifndef FH_DRV_H
#define FH_DRV_H

#include "fh/fh_evring.h"
#include "fh/fh_keycache.h"
#include "fh/fh_link.h"
#include "fh/fh_predict.h"
#include "fh/fh_scan.h"
#include "fh/fh_sm.h"

struct fh_drv;

/* BSS descriptor as the driver learns it from beacons and neighbour reports. */
struct fh_ap_info {
	uint64_t bssid;
	uint8_t chan;
	uint8_t subnet;
};

struct fh_scan_report {
	fh_ap_t ap;
	int16_t rssi;
	uint8_t load_pct; /* from the BSS load element */
};

enum fh_key_mode { FH_KEY_FT, FH_KEY_4WAY, FH_KEY_FULL_EAP };

/*
 * Radio and firmware operations. Every call returns the air time it used in
 * milliseconds; the simulator implements them, a real driver would issue
 * frames and wait for completions.
 */
struct fh_hw_ops {
	uint16_t (*probe)(void *hw, fh_ms_t now, uint8_t chan, struct fh_scan_report *rep, int max, int *n_rep);
	uint16_t (*auth)(void *hw, fh_ms_t now, fh_ap_t ap, bool ft);
	uint16_t (*reassoc)(void *hw, fh_ms_t now, fh_ap_t ap);
	uint16_t (*key)(void *hw, fh_ms_t now, fh_ap_t ap, enum fh_key_mode mode);
	uint16_t (*preauth)(void *hw, fh_ms_t now, fh_ap_t ap);
	int (*neighbor_report)(void *hw, fh_ap_t ap, fh_ap_t *out, int max);
};

enum fh_verdict { FH_STAY, FH_PREPARE, FH_CANCEL, FH_GO };

struct fh_decision {
	enum fh_verdict verdict;
	fh_ap_t target;
	uint16_t eta_ms;
	bool reactive;
};

/* Roaming policy. The driver core runs the state machine; the policy decides. */
struct fh_roam_ops {
	const char *name;
	bool bgscan;          /* neighbour reports and background scanning */
	bool fast_transition; /* pre-authentication and cached-key transitions */
	void (*evaluate)(struct fh_drv *d, fh_ms_t now, struct fh_decision *dec);
	/* Pick the target once the handoff scan is over; FH_AP_NONE aborts. */
	fh_ap_t (*select)(struct fh_drv *d, fh_ms_t now);
};

extern const struct fh_roam_ops fh_roam_baseline;
extern const struct fh_roam_ops fh_roam_adapted;

struct fh_drv_cfg {
	const struct fh_roam_ops *roam;
	struct fh_predictor *pred; /* optional */
	int8_t base_dbm;      /* baseline: hard roam threshold */
	int8_t bgscan_dbm;    /* adapted: background scanning starts below this */
	int8_t prep_dbm;      /* adapted: forecast below this arms HANDOFF_IMMINENT */
	int8_t roam_dbm;      /* adapted: roam only when current or forecast is below this */
	int8_t emergency_dbm; /* adapted: go now, no time-to-trigger */
	uint8_t hyst_db;
	uint8_t load_weight_db; /* dB of penalty for a fully loaded AP */
	uint8_t beacon_loss_n;
	uint16_t ttt_ms;
	uint16_t horizon_ms;
	uint16_t holdoff_ms;    /* baseline: minimum time between scans */
	uint16_t fresh_ms;      /* cache age that still counts as a current measurement */
	uint16_t pingpong_ms;
};

struct fh_drv_stats {
	uint32_t handoffs;
	uint32_t pingpongs;
	uint32_t aborted;
	uint32_t reactive;
	uint32_t ft_used;
	uint32_t full_eap;
	uint32_t preauths;
	uint32_t imminent;
	uint32_t cancelled;
	uint32_t degraded;
};

#define FH_HO_HIST 8

struct fh_drv {
	struct fh_drv_cfg cfg;
	const struct fh_hw_ops *hw;
	void *hw_priv;
	struct fh_ap_info ap[FH_MAX_APS];
	uint8_t n_ap;
	uint64_t sta;

	struct fh_sm sm;
	struct fh_link link;
	fh_ap_t cur;
	fh_ap_t prev;
	struct fh_scan scan;
	struct fh_keycache kc;
	struct fh_evring ev;

	/* handoff in progress */
	fh_ap_t target;
	fh_ms_t ho_start;
	fh_ms_t phase_end;
	struct fh_ho_latency lat;
	enum fh_key_mode key_mode;
	uint8_t ho_flags;

	/* context captured when the next AP was first predicted */
	struct fh_pred_ctx ctx;
	bool ctx_valid;
	fh_ap_t predicted[3];
	uint8_t n_predicted;

	/* policy state */
	fh_ms_t ttt_start;
	fh_ms_t recover_start;
	fh_ms_t last_scan;
	fh_ms_t last_degraded;
	fh_ms_t offchan_until;

	struct {
		fh_ap_t from, to;
		fh_ms_t ts;
	} hist[FH_HO_HIST];
	uint8_t hist_n;
	struct fh_drv_stats st;
};

void fh_drv_cfg_default(struct fh_drv_cfg *cfg, const struct fh_roam_ops *roam);
int fh_drv_init(struct fh_drv *d, const struct fh_drv_cfg *cfg, const struct fh_hw_ops *hw, void *hw_priv,
		const struct fh_ap_info *aps, int n_ap, uint64_t sta);
int fh_drv_associate(struct fh_drv *d, fh_ap_t ap, fh_ms_t now, int rssi_dbm);

/* Data path and beacon notifications from the receive path. */
void fh_drv_rx(struct fh_drv *d, fh_ms_t now, int rssi_dbm, bool first_try_ok);
void fh_drv_beacon(struct fh_drv *d, fh_ms_t now, bool received, int rssi_dbm);
/* Timer: policy, background scan scheduler and handoff phases. Call every ms. */
void fh_drv_tick(struct fh_drv *d, fh_ms_t now);

bool fh_drv_associated(const struct fh_drv *d);
/* Start pre-authentication with ap over the current AP, if not cached yet. */
void fh_drv_preauth(struct fh_drv *d, fh_ms_t now, fh_ap_t ap);
void fh_drv_build_ctx(const struct fh_drv *d, fh_ms_t now, struct fh_pred_ctx *c);

#endif
