/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Policy state and text command protocol of the controllable downlink
 * scheduler.  Independent of the gNB sources so it can be tested natively.
 *
 * Commands (one per datagram, case-insensitive keywords, RNTI in hex):
 *   PING
 *   GET
 *   POLICY pf|rr|wrr|wshare
 *   WEIGHT <rnti|*> <1..1000000>      relative downlink share (wshare)
 *   CAP <rnti> <0|5..275>             most resource blocks per slot, 0 = none
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
#define NR_CTRL_SLOTS 40 /* slot indices tracked (80 slots per frame at most 40 are kept) */

enum { NR_CTRL_POLICY_PF = 0, NR_CTRL_POLICY_RR = 1, NR_CTRL_POLICY_WSHARE = 2, NR_CTRL_POLICY_WRR = 3 };

typedef struct {
  uint16_t rnti;
  uint32_t weight;
  int cap;
  uint64_t granted_rbs;
  uint64_t granted_slots;
  uint64_t commit_fail; /* grants planned for this UE that the MAC refused */
} nr_ctrl_ue_t;

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
  int n_ue;
  nr_ctrl_ue_t ue[NR_CTRL_MAX_UE];
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
/* granted_rbs / weight, the quantity the policy equalises */
double nr_ctrl_deficit(const nr_ctrl_state_t *st, uint16_t rnti);
void nr_ctrl_account(nr_ctrl_state_t *st, uint16_t rnti, int rbs);
void nr_ctrl_note_commit_fail(nr_ctrl_state_t *st, uint16_t rnti, int slot, int cce);
/* Drop entries of UEs that are no longer connected. */
void nr_ctrl_prune(nr_ctrl_state_t *st, const nr_ctrl_ue_view_t *ues, int n_ues);
/* Executes one command and writes the reply; returns the reply length. */
int nr_ctrl_handle(nr_ctrl_state_t *st, const char *cmd, const nr_ctrl_ue_view_t *ues, int n_ues, char *reply, size_t reply_size);

#endif /* NR_DL_CTRL_STATE_H */
