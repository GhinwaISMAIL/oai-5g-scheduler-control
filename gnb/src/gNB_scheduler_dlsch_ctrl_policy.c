/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Downlink resource-block policy with runtime control.
 *
 * Modes
 *   pf      stock proportional-fair policy (delegated unchanged)
 *   rr      one backlogged UE after the other, in RNTI order, one policy
 *           invocation each (time-domain round robin)
 *   wrr     weighted round robin in the time domain: the UE that has received
 *           the fewest resource blocks relative to its weight takes the slot,
 *           so shares follow the weights while each slot normally serves one UE
 *   wshare  the free block of every slot is split between all backlogged UEs
 *           in proportion to their weights (frequency-domain weighted share),
 *           bounded by each UE's demand and optional per-UE cap
 *
 * HARQ retransmissions and UEs that only need a MAC control element are served
 * before new data in every mode, exactly as in the stock policy.
 *
 * Control: UDP datagrams on SCHED_CTRL_ADDR:SCHED_CTRL_PORT (default
 * 127.0.0.1:9099); see nr_dl_ctrl_state.h for the command set.  All state is
 * read and written with the scheduler lock held.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common/utils/nr/nr_common.h"
#include "gNB_scheduler_dlsch_ctrl_policy.h"
#include "gNB_scheduler_dlsch_default_policies.h"
#include "nr_dl_ctrl_alloc.h"
#include "nr_dl_ctrl_state.h"
/*MAC*/
#include "NR_MAC_COMMON/nr_mac.h"
#include "NR_MAC_gNB/nr_mac_gNB.h"
#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include "common/utils/LOG/log.h"

#define CTRL_PRUNE_PERIOD 256
#define CTRL_DEFAULT_PORT 9099
#define CTRL_DEFAULT_ADDR "127.0.0.1"

static nr_ctrl_state_t g_state;
static gNB_MAC_INST *g_mac;

static int build_view(gNB_MAC_INST *mac, nr_ctrl_ue_view_t *view, int max)
{
  int n = 0;
  UE_iterator(mac->UE_info.connected_ue_list, UE)
  {
    if (n >= max)
      break;
    view[n].rnti = UE->rnti;
    view[n].mac_dl_rbs = UE->mac_stats.dl.total_rbs;
    view[n].mac_dl_rbs_retx = UE->mac_stats.dl.total_rbs_retx;
    view[n].mac_dl_sdu_bytes = UE->mac_stats.dl.total_sdu_bytes;
    n++;
  }
  return n;
}

/* Same field writes as the COMMIT_ALLOC macro, but reporting the outcome so the
 * caller can account the grant and keep going. */
static bool ctrl_commit(const nr_dl_sched_params_t *params, nr_dl_candidate_t *cand, int rb_start, int rb_size, int mcs)
{
  cand->sched_pdsch.alloc_type = PDSCH_TYPE1;
  cand->sched_pdsch.rbStart = rb_start;
  cand->sched_pdsch.rbSize = rb_size;
  cand->sched_pdsch.mcs = mcs;
  if (!commit_alloc(params, cand))
    return false;
  cand->scheduled = true;
  return true;
}

/* Resource blocks the UE could use for its pending bytes, at most max_rbSize. */
static int ctrl_demand_rbs(const nr_dl_sched_params_t *params, const nr_dl_candidate_t *cand, int max_rbSize)
{
  int mcs = cand->sched_pdsch.mcs;
  uint8_t Qm = nr_get_Qm_dl(mcs, cand->mcs_table);
  uint16_t R = nr_get_code_rate_dl(mcs, cand->mcs_table);
  NR_pdsch_dmrs_t dmrs = get_dl_dmrs_params(params->mac->common_channels->ServingCellConfigCommon,
                                            &cand->UE->current_DL_BWP,
                                            &cand->sched_pdsch.tda_info,
                                            cand->sched_pdsch.nrOfLayers);
  const int oh = 3 * 4 + (cand->UE->UE_sched_ctrl.ta_apply ? 2 : 0);
  uint32_t tbs;
  uint16_t rbSize;
  nr_find_nb_rb(Qm,
                R,
                1,
                cand->sched_pdsch.nrOfLayers,
                cand->sched_pdsch.tda_info.nrOfSymbols,
                dmrs.N_PRB_DMRS * dmrs.N_DMRS_SLOT,
                cand->pending_bytes + oh,
                NR_CTRL_MIN_RB,
                max_rbSize,
                &tbs,
                &rbSize);
  return rbSize;
}

/* Phases 1 and 2 of the stock policy: HARQ retransmissions with their exact
 * size, then UEs that only need a MAC control element. */
static int ctrl_serve_mandatory(const nr_dl_sched_params_t *params, nr_dl_candidate_t *candidates, int n_candidates, int n_sched)
{
  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  {
    if (cand->skipped || !cand->is_retx)
      continue;
    uint16_t *vrb_map = params->vrb_map[cand->alloc_beam_idx];
    int rbStart, rbSize;
    if (!get_rb_alloc(cand->retx_rbSize, cand->bwp_size, cand->bwp_start, cand->bwp_size, vrb_map, cand->alloc_slbitmap, &rbStart, &rbSize))
      continue;
    if (ctrl_commit(params, cand, rbStart, cand->retx_rbSize, cand->sched_pdsch.mcs)) {
      n_sched++;
      if (n_sched >= params->max_num_ue)
        return n_sched;
    }
  }

  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  {
    if (cand->skipped || cand->is_retx || cand->pending_bytes > 0)
      continue;
    uint16_t *vrb_map = params->vrb_map[cand->alloc_beam_idx];
    int rbStart, rbSize;
    if (!get_rb_alloc(NR_CTRL_MIN_RB, cand->bwp_size, cand->bwp_start, cand->bwp_size, vrb_map, cand->alloc_slbitmap, &rbStart, &rbSize))
      continue;
    if (ctrl_commit(params, cand, rbStart, NR_CTRL_MIN_RB, cand->sched_pdsch.mcs)) {
      n_sched++;
      if (n_sched >= params->max_num_ue)
        return n_sched;
    }
  }
  return n_sched;
}

static bool is_new_data(const nr_dl_candidate_t *c)
{
  return !c->skipped && !c->is_retx && !c->scheduled && c->pending_bytes > 0;
}

/* Sequential policies: new-data UEs are visited one after the other and each
 * takes what it needs from the largest free block, so with saturated queues the
 * first UE takes the whole slot.
 *   rr  : RNTI order starting after the UE that led last time
 *   wrr : least granted-resource-blocks-per-weight first (ties: lower RNTI) */
static int ctrl_sequential(const nr_dl_sched_params_t *params, nr_dl_candidate_t *candidates, int n_candidates, int n_sched, bool by_deficit)
{
  nr_dl_candidate_t *order[MAX_MOBILES_PER_GNB];
  int n = 0;
  FOR_EACH_CANDIDATE(c, candidates, n_candidates)
  if (is_new_data(c) && n < MAX_MOBILES_PER_GNB)
    order[n++] = c;
  if (n == 0)
    return n_sched;

  /* insertion sort: by RNTI (rr) or by deficit then RNTI (wrr) */
  for (int i = 1; i < n; i++) {
    nr_dl_candidate_t *k = order[i];
    double dk = by_deficit ? nr_ctrl_deficit(&g_state, k->rnti) : 0.0;
    int j = i - 1;
    while (j >= 0) {
      double dj = by_deficit ? nr_ctrl_deficit(&g_state, order[j]->rnti) : 0.0;
      if (dj < dk || (dj == dk && order[j]->rnti <= k->rnti))
        break;
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = k;
  }
  int first = 0;
  if (!by_deficit) {
    /* rotate so the first UE with RNTI above the last leader leads */
    while (first < n && order[first]->rnti <= g_state.rr_last_rnti)
      first++;
    if (first == n)
      first = 0;
  }

  bool led = false;
  for (int k = 0; k < n; k++) {
    nr_dl_candidate_t *cand = order[(first + k) % n];
    int cap = nr_ctrl_cap_of(&g_state, cand->rnti);
    uint16_t *vrb_map = params->vrb_map[cand->alloc_beam_idx];
    int rbStart;
    int max_rb = find_largest_free_block(vrb_map, cand->alloc_slbitmap, cand->bwp_start, cand->bwp_size, &rbStart);
    if (cap > 0 && max_rb > cap)
      max_rb = cap;
    if (max_rb < NR_CTRL_MIN_RB)
      continue;
    int rbSize = ctrl_demand_rbs(params, cand, max_rb);
    if (ctrl_commit(params, cand, rbStart, rbSize, cand->sched_pdsch.mcs)) {
      nr_ctrl_account(&g_state, cand->rnti, rbSize);
      if (!led) {
        g_state.rr_last_rnti = cand->rnti;
        led = true;
      }
      n_sched++;
      if (n_sched >= params->max_num_ue)
        return n_sched;
    }
  }
  return n_sched;
}

static bool same_group(const nr_dl_candidate_t *a, const nr_dl_candidate_t *b)
{
  return a->alloc_beam_idx == b->alloc_beam_idx && a->bwp_start == b->bwp_start && a->bwp_size == b->bwp_size
         && a->alloc_slbitmap == b->alloc_slbitmap;
}

/* Frequency-domain weighted share of the largest free block for one group of
 * UEs that compete for the same resource blocks. */
static int ctrl_wshare_group(const nr_dl_sched_params_t *params, nr_dl_candidate_t **grp, int ng, int n_sched)
{
  uint16_t *vrb_map = params->vrb_map[grp[0]->alloc_beam_idx];
  int start;
  int free_rb = find_largest_free_block(vrb_map, grp[0]->alloc_slbitmap, grp[0]->bwp_start, grp[0]->bwp_size, &start);
  if (free_rb < NR_CTRL_MIN_RB)
    return n_sched;

  /* least served relative to weight first: decides who is admitted when there
   * are more UEs than minimum-size grants fit */
  for (int i = 1; i < ng; i++) {
    nr_dl_candidate_t *k = grp[i];
    double dk = nr_ctrl_deficit(&g_state, k->rnti);
    int j = i - 1;
    while (j >= 0 && nr_ctrl_deficit(&g_state, grp[j]->rnti) > dk) {
      grp[j + 1] = grp[j];
      j--;
    }
    grp[j + 1] = k;
  }

  int room = params->max_num_ue - n_sched;
  int n = ng < room ? ng : room;
  if (n > NR_CTRL_SPLIT_MAX)
    n = NR_CTRL_SPLIT_MAX;
  if (n <= 0)
    return n_sched;

  uint32_t weight[MAX_MOBILES_PER_GNB];
  int demand[MAX_MOBILES_PER_GNB];
  int out[MAX_MOBILES_PER_GNB] = {0};
  for (int i = 0; i < n; i++) {
    weight[i] = nr_ctrl_weight_of(&g_state, grp[i]->rnti);
    int d = ctrl_demand_rbs(params, grp[i], free_rb);
    int cap = nr_ctrl_cap_of(&g_state, grp[i]->rnti);
    if (cap > 0 && d > cap)
      d = cap;
    demand[i] = d;
  }
  nr_ctrl_split(n, weight, demand, NR_CTRL_MIN_RB, free_rb, out);

  int cursor = start;
  for (int i = 0; i < n; i++) {
    if (out[i] <= 0)
      continue;
    if (ctrl_commit(params, grp[i], cursor, out[i], grp[i]->sched_pdsch.mcs)) {
      nr_ctrl_account(&g_state, grp[i]->rnti, out[i]);
      cursor += out[i];
      n_sched++;
      if (n_sched >= params->max_num_ue)
        return n_sched;
    }
  }
  return n_sched;
}

static int ctrl_wshare(const nr_dl_sched_params_t *params, nr_dl_candidate_t *candidates, int n_candidates, int n_sched)
{
  nr_dl_candidate_t *pool[MAX_MOBILES_PER_GNB];
  bool used[MAX_MOBILES_PER_GNB] = {false};
  int np = 0;
  FOR_EACH_CANDIDATE(c, candidates, n_candidates)
  if (is_new_data(c) && np < MAX_MOBILES_PER_GNB)
    pool[np++] = c;

  for (int g = 0; g < np; g++) {
    if (used[g])
      continue;
    nr_dl_candidate_t *grp[MAX_MOBILES_PER_GNB] = {NULL};
    int ng = 0;
    for (int j = g; j < np; j++) {
      if (!used[j] && same_group(pool[g], pool[j])) {
        used[j] = true;
        grp[ng++] = pool[j];
      }
    }
    n_sched = ctrl_wshare_group(params, grp, ng, n_sched);
    if (n_sched >= params->max_num_ue)
      break;
  }
  return n_sched;
}

int nr_dl_ctrl_policy(const nr_dl_sched_params_t *params, nr_dl_candidate_t *candidates, int n_candidates)
{
  /* called with the scheduler lock held */
  g_state.calls++;
  if (g_state.calls % CTRL_PRUNE_PERIOD == 0) {
    nr_ctrl_ue_view_t view[MAX_MOBILES_PER_GNB];
    int nv = build_view(g_mac, view, MAX_MOBILES_PER_GNB);
    nr_ctrl_prune(&g_state, view, nv);
  }

  if (g_state.policy == NR_CTRL_POLICY_PF)
    return nr_dl_proportional_fair(params, candidates, n_candidates);

  int n_sched = ctrl_serve_mandatory(params, candidates, n_candidates, 0);
  if (n_sched >= params->max_num_ue)
    return n_sched;
  if (g_state.policy == NR_CTRL_POLICY_RR)
    return ctrl_sequential(params, candidates, n_candidates, n_sched, false);
  if (g_state.policy == NR_CTRL_POLICY_WRR)
    return ctrl_sequential(params, candidates, n_candidates, n_sched, true);
  return ctrl_wshare(params, candidates, n_candidates, n_sched);
}

static void *ctrl_thread(void *arg)
{
  (void)arg;
  const char *addr_s = getenv("SCHED_CTRL_ADDR");
  const char *port_s = getenv("SCHED_CTRL_PORT");
  if (!addr_s || !*addr_s)
    addr_s = CTRL_DEFAULT_ADDR;
  int port = (port_s && *port_s) ? atoi(port_s) : CTRL_DEFAULT_PORT;

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
  if (fd < 0 || port <= 0 || port > 65535 || inet_pton(AF_INET, addr_s, &sa.sin_addr) != 1
      || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    LOG_E(NR_MAC, "[SCHED_CTRL] cannot open control endpoint %s:%d: %s\n", addr_s, port, strerror(errno));
    if (fd >= 0)
      close(fd);
    return NULL;
  }
  struct timeval tv = {.tv_sec = 1};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  LOG_I(NR_MAC, "[SCHED_CTRL] control endpoint on %s:%d\n", addr_s, port);

  for (;;) {
    char cmd[256];
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    ssize_t r = recvfrom(fd, cmd, sizeof(cmd) - 1, 0, (struct sockaddr *)&peer, &plen);
    if (r < 0)
      continue; /* timeout or interruption */
    cmd[r] = '\0';

    char reply[4096];
    int len;
    NR_SCHED_LOCK(&g_mac->sched_lock);
    nr_ctrl_ue_view_t view[MAX_MOBILES_PER_GNB];
    int nv = build_view(g_mac, view, MAX_MOBILES_PER_GNB);
    uint32_t before = g_state.version;
    len = nr_ctrl_handle(&g_state, cmd, view, nv, reply, sizeof(reply));
    uint32_t after = g_state.version;
    int policy = g_state.policy;
    NR_SCHED_UNLOCK(&g_mac->sched_lock);

    if (after != before)
      LOG_I(NR_MAC, "[SCHED_CTRL] \"%.60s\" applied, version %u policy %s\n", cmd, (unsigned)after, nr_ctrl_policy_name(policy));
    if (len > 0)
      sendto(fd, reply, (size_t)len, 0, (struct sockaddr *)&peer, plen);
  }
  return NULL;
}

void nr_dl_ctrl_policy_init(gNB_MAC_INST *mac)
{
  if (g_mac) {
    LOG_W(NR_MAC, "[SCHED_CTRL] only the first MAC instance is controllable; instance keeps proportional fair\n");
    mac->dl_rb_alloc = nr_dl_proportional_fair;
    return;
  }
  g_mac = mac;
  nr_ctrl_init(&g_state);
  mac->dl_rb_alloc = nr_dl_ctrl_policy;

  pthread_t th;
  if (pthread_create(&th, NULL, ctrl_thread, NULL) == 0) {
    pthread_detach(th);
  } else {
    LOG_E(NR_MAC, "[SCHED_CTRL] control thread not started; policy stays proportional fair\n");
  }
}
