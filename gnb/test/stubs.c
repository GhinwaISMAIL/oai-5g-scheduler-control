/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stand-ins for the gNB helper functions called by the policy.
 * find_largest_free_block() and get_rb_alloc() are copies of the pinned
 * implementation; commit_alloc() and nr_find_nb_rb() are simplified models.
 */
#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "gNB_scheduler_dlsch_default_policies.h"
#include "stubs.h"

int stub_pf_calls;
uint16_t stub_commit_fail_rnti;
int stub_fail_is_cce;
int stub_bytes_per_rb = 100;

int find_largest_free_block(const uint16_t *vrb_map, uint16_t slbitmap, int bwp_start, int bwp_size, int *out_start)
{
  int best_start = 0, best_len = 0;
  int cur_start = 0, cur_len = 0;
  for (int rb = 0; rb < bwp_size; rb++) {
    if (!(vrb_map[rb + bwp_start] & slbitmap)) {
      if (cur_len == 0)
        cur_start = rb;
      cur_len++;
    } else {
      if (cur_len > best_len) {
        best_start = cur_start;
        best_len = cur_len;
      }
      cur_len = 0;
    }
  }
  if (cur_len > best_len) {
    best_start = cur_start;
    best_len = cur_len;
  }
  *out_start = best_start;
  return best_len;
}

bool get_rb_alloc(int rbSize_min,
                  int rbSize_max,
                  int bwpStart,
                  int bwpSize,
                  const uint16_t *vrb_map,
                  uint16_t sym_mask,
                  int *rbStart_ptr,
                  int *rbSize_ptr)
{
  const uint16_t *bwp_map = &vrb_map[bwpStart];
  int rbStart = 0;
  while (rbStart + rbSize_min <= bwpSize) {
    if ((bwp_map[rbStart] & sym_mask) != 0) {
      rbStart++;
      continue;
    }
    int current_limit = min(rbStart + rbSize_max, bwpSize);
    int rbEnd = rbStart + 1;
    while (rbEnd < current_limit && (bwp_map[rbEnd] & sym_mask) == 0) {
      rbEnd++;
    }
    int rbSize = rbEnd - rbStart;
    if (rbSize >= rbSize_min) {
      *rbStart_ptr = rbStart;
      *rbSize_ptr = rbSize;
      return true;
    }
    rbStart = rbEnd + 1;
  }
  return false;
}

/* Model: refuses to overlap occupied resource blocks, marks them occupied and
 * counts them in the UE's MAC statistics. */
bool commit_alloc(const nr_dl_sched_params_t *params, nr_dl_candidate_t *cand)
{
  if (stub_commit_fail_rnti && cand->rnti == stub_commit_fail_rnti) {
    if (stub_fail_is_cce)
      cand->UE->UE_sched_ctrl.dl_cce_fail++;
    return false;
  }
  uint16_t *vrb = params->vrb_map[cand->alloc_beam_idx];
  int s = cand->bwp_start + cand->sched_pdsch.rbStart;
  int e = s + cand->sched_pdsch.rbSize;
  if (cand->sched_pdsch.rbSize <= 0 || e > cand->bwp_start + cand->bwp_size)
    return false;
  for (int rb = s; rb < e; rb++)
    if (vrb[rb] & cand->alloc_slbitmap)
      return false;
  for (int rb = s; rb < e; rb++)
    vrb[rb] |= cand->alloc_slbitmap;
  if (cand->is_retx)
    cand->UE->mac_stats.dl.total_rbs_retx += cand->sched_pdsch.rbSize;
  cand->UE->mac_stats.dl.total_rbs += cand->sched_pdsch.rbSize;
  return true;
}

bool nr_find_nb_rb(uint16_t Qm, uint16_t R, long tp, uint8_t layers, uint16_t nsym, uint16_t ndmrs, uint32_t bytes,
                   uint16_t rb_min, uint16_t rb_max, uint32_t *tbs, uint16_t *nb_rb)
{
  (void)Qm; (void)R; (void)tp; (void)layers; (void)nsym; (void)ndmrs;
  uint32_t need = (bytes + stub_bytes_per_rb - 1) / stub_bytes_per_rb;
  uint32_t rb = need < rb_min ? rb_min : (need > rb_max ? rb_max : need);
  *nb_rb = (uint16_t)rb;
  *tbs = rb * stub_bytes_per_rb;
  return *tbs >= bytes && *nb_rb <= rb_max;
}

NR_pdsch_dmrs_t get_dl_dmrs_params(const void *scc, const NR_UE_DL_BWP_t *b, const NR_tda_info_t *t, int l)
{
  (void)scc; (void)b; (void)t; (void)l;
  NR_pdsch_dmrs_t d = {.N_PRB_DMRS = 12, .N_DMRS_SLOT = 1};
  return d;
}
uint8_t nr_get_Qm_dl(uint8_t i, uint8_t t) { (void)i; (void)t; return 6; }
uint16_t nr_get_code_rate_dl(uint8_t i, uint8_t t) { (void)i; (void)t; return 5000; }

/* Records that the stock policy was used; allocates nothing. */
int nr_dl_proportional_fair(const nr_dl_sched_params_t *p, nr_dl_candidate_t *c, int n)
{
  (void)p; (void)c; (void)n;
  stub_pf_calls++;
  return 0;
}
