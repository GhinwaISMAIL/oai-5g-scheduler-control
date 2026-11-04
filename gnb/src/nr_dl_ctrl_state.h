/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Policy state and text command protocol of the controllable downlink
 * scheduler.  Independent of the gNB sources so it can be tested natively.
 *
 * Commands (one per datagram, case-insensitive keywords, RNTI in hex):
 *   PING
 *   GET
 *   POLICY pf|rr|wrr|wshare|slice
 *   WEIGHT <rnti|*> <1..1000000>      relative downlink share (wshare)
 *   CAP <rnti> <0|5..275>             most resource blocks per slot, 0 = none
 *   SLICE <id> WEIGHT <1..1000000>    relative share of a slice (policy slice)
 *   SLICE <id> CAP <0|5..275>         most resource blocks all UEs of the slice take together in one call
 *   SLICEDEL <id>                     slice back to its defaults (weight, no cap, counters), its associations
 *                                     and S-NSSAI rules removed, so its UEs fall back to slice 0
 *   ASSOC <rnti> <id|->               puts a UE in a slice (id 0..7), '-' removes the association
 *   NSSAIMAP <sst> <sd|none> <id|->   slice for UEs with that S-NSSAI (sd in hex), '-' deletes the rule
 *   RESET                             stock proportional-fair, state cleared
 * Replies start with "OK" or "ERR <reason>".
 */
#ifndef NR_DL_CTRL_STATE_H
#define NR_DL_CTRL_STATE_H

#include <stddef.h>
#include <stdint.h>

#define NR_CTRL_MAX_UE 32
#define NR_CTRL_MIN_RB 5
#define NR_CTRL_MAX_CAP 275
#define NR_CTRL_MIN_WEIGHT 1
#define NR_CTRL_MAX_WEIGHT 1000000
#define NR_CTRL_DEFAULT_WEIGHT 1000
#define NR_CTRL_WS_SHARE_INIT 500 /* start: share the slot in half of the slots */
#define NR_CTRL_WS_SHARE_MIN 20   /* never stop probing: 2 % of the slots */
#define NR_CTRL_MAX_SLICE 8
#define NR_CTRL_MAX_RULE 8
#define NR_CTRL_NO_SD 0xFFFFFFu /* S-NSSAI without a slice differentiator */
/* A backlogged slice may be at most this many resource blocks of grants behind the other backlogged
 * slices (in units of its own weight): the credit a slice collects while it has little or nothing to
 * send, which it can spend as a burst once it has data again.  The same bound applies to a UE
 * against the other backlogged UEs of its slice (of all UEs for wrr), so a UE that attaches or
 * reconnects does not take every slot until its counter has caught up with its peers. */
#define NR_CTRL_SLICE_CREDIT_RBS 2000
#define NR_CTRL_UE_CREDIT_RBS 2000
#define NR_CTRL_SLOTS 40 /* slot indices tracked (80 slots per frame at most 40 are kept) */

enum { NR_CTRL_POLICY_PF = 0, NR_CTRL_POLICY_RR = 1, NR_CTRL_POLICY_WSHARE = 2, NR_CTRL_POLICY_WRR = 3, NR_CTRL_POLICY_SLICE = 4 };

/* where the slice of a UE comes from */
enum { NR_CTRL_SRC_DEFAULT = 0, NR_CTRL_SRC_RNTI = 1, NR_CTRL_SRC_NSSAI = 2 };

typedef struct {
  uint16_t rnti;
  uint32_t weight;
  int cap;
  uint64_t granted_rbs;
  uint64_t credit_rbs;  /* virtual resource blocks added by the credit bound; only the ordering uses them */
  uint64_t granted_slots;
  uint64_t commit_fail; /* grants planned for this UE that the MAC refused */
  uint8_t assoc;        /* 1: explicitly associated with `slice` */
  uint8_t slice;
  uint8_t nssai_known;  /* the S-NSSAI below was seen on a scheduler candidate */
  uint8_t sst;
  uint32_t sd;
} nr_ctrl_ue_t;

typedef struct {
  uint32_t weight;
  int cap;
  uint64_t granted_rbs;
  uint64_t credit_rbs; /* virtual resource blocks added by the credit bound; only the ordering uses them */
  uint64_t granted_slots;
  uint8_t configured;  /* a SLICE command set its weight or cap (even to the default value) */
} nr_ctrl_slice_t;

typedef struct {
  uint8_t sst;
  uint32_t sd;
  uint8_t slice;
} nr_ctrl_rule_t;

typedef struct {
  int policy;
  uint32_t version; /* incremented by every accepted state change */
  uint32_t default_weight;
  uint64_t calls; /* scheduler invocations since start */
  uint16_t rr_last_rnti;
  /* diagnostics (non-pf policies) */
  uint64_t commit_fail;    /* grants refused at commit, all UEs */
  uint64_t fail_cce;       /* ... of which no free PDCCH candidate (CCE) */
  uint64_t fail_pucch;     /* ... of which no ACK/NACK occasion (PUCCH) */
  uint32_t slot_calls[NR_CTRL_SLOTS]; /* invocations by slot index in the frame */
  uint32_t slot_fail[NR_CTRL_SLOTS];  /* refused grants by slot index */
  uint64_t multi_ue_slots; /* invocations that scheduled two or more new-data UEs */
  uint64_t ws_groups;      /* wshare: groups planned */
  uint64_t ws_free_rb;     /* wshare: free block size summed over groups */
  uint64_t ws_demand_rb;   /* wshare: summed demand of the admitted UEs */
  uint64_t ws_planned_rb;  /* wshare: resource blocks assigned by the split */
  uint64_t ws_committed_rb; /* wshare: resource blocks accepted at commit */
  uint32_t ws_share_milli;  /* wshare: fraction of slots (in 1/1000) in which the slot is shared in frequency */
  uint32_t ws_acc;          /* wshare: accumulator that spreads the shared slots evenly */
  uint64_t ws_shared_slots; /* wshare: slots shared in frequency (two or more UEs competing) */
  uint64_t ws_single_slots; /* wshare: slots given to one UE at a time (two or more UEs competing) */
  uint64_t ws_lost_slots;   /* wshare: slots where every grant was refused */
  int n_ue;
  nr_ctrl_ue_t ue[NR_CTRL_MAX_UE];
  nr_ctrl_slice_t slice[NR_CTRL_MAX_SLICE];
  int n_rule;
  nr_ctrl_rule_t rule[NR_CTRL_MAX_RULE];
} nr_ctrl_state_t;

/* Snapshot of a connected UE taken by the caller under the scheduler lock. */
typedef struct {
  uint16_t rnti;
  uint64_t mac_dl_rbs;
  uint64_t mac_dl_rbs_retx;
  uint64_t mac_dl_sdu_bytes;
} nr_ctrl_ue_view_t;

void nr_ctrl_init(nr_ctrl_state_t *st);
const char *nr_ctrl_policy_name(int policy);
uint32_t nr_ctrl_weight_of(const nr_ctrl_state_t *st, uint16_t rnti);
int nr_ctrl_cap_of(const nr_ctrl_state_t *st, uint16_t rnti);
/* (granted_rbs + credit_rbs) / weight, the quantity the policy equalises */
double nr_ctrl_deficit(const nr_ctrl_state_t *st, uint16_t rnti);
void nr_ctrl_account(nr_ctrl_state_t *st, uint16_t rnti, int rbs);

/* Slices.  The caller records the S-NSSAI of every scheduler candidate first. */
void nr_ctrl_note_nssai(nr_ctrl_state_t *st, uint16_t rnti, uint8_t sst, uint32_t sd);
/* Slice id of the UE (0..7) and, in *source, why: NR_CTRL_SRC_*. */
int nr_ctrl_slice_of(const nr_ctrl_state_t *st, uint16_t rnti, int *source);
/* (granted_rbs + credit_rbs) / weight of the slice, the quantity the slice policy equalises */
double nr_ctrl_slice_deficit(const nr_ctrl_state_t *st, int slice);
int nr_ctrl_slice_cap(const nr_ctrl_state_t *st, int slice);
void nr_ctrl_account_slice(nr_ctrl_state_t *st, int slice, int rbs);
/* Called once per scheduler call with the slices that have a UE with new data.  The credit of a
 * backlogged slice is raised, never lowered, so that its deficit is at most NR_CTRL_SLICE_CREDIT_RBS
 * (of its own weight) below the highest deficit of the other backlogged slices.  A slice that had
 * little or nothing to send for a while can therefore burst by that much, but cannot take every slot
 * until it has caught up with what it did not use. */
void nr_ctrl_slice_activity(nr_ctrl_state_t *st, const uint8_t backlogged[NR_CTRL_MAX_SLICE]);
/* The same for the UEs that have new data in this call: group[i] is the slice of UE i (0 for all in
 * wrr).  A UE is compared with the other UEs of its group only, with NR_CTRL_UE_CREDIT_RBS. */
void nr_ctrl_ue_activity(nr_ctrl_state_t *st, const uint16_t *rnti, const uint8_t *group, int n);
void nr_ctrl_note_commit_fail(nr_ctrl_state_t *st, uint16_t rnti, int slot, int cce);
/* Drop entries of UEs that are no longer connected. */
void nr_ctrl_prune(nr_ctrl_state_t *st, const nr_ctrl_ue_view_t *ues, int n_ues);
/* Executes one command and writes the reply; returns the reply length. */
int nr_ctrl_handle(nr_ctrl_state_t *st, const char *cmd, const nr_ctrl_ue_view_t *ues, int n_ues, char *reply, size_t reply_size);

#endif /* NR_DL_CTRL_STATE_H */
