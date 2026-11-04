/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Native tests of the controllable downlink policy.  Exit status 0 = all pass.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "gNB_scheduler_dlsch_ctrl_policy.h"
#include "gNB_scheduler_dlsch_default_policies.h"
#include "nr_dl_ctrl_alloc.h"
#include "nr_dl_ctrl_e2.h"
#include "nr_dl_ctrl_slice_sm.h"
#include "nr_dl_ctrl_state.h"
#include "stubs.h"

static int checks, failures;
#define CHECK(cond)                                                       \
  do {                                                                    \
    checks++;                                                             \
    if (!(cond)) {                                                        \
      failures++;                                                         \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                     \
  } while (0)

/* ------------------------------------------------------------ split math */

static uint64_t rng_state = 88172645463325252ULL;
static uint32_t rnd(void)
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return (uint32_t)(rng_state >> 11);
}

static void test_split_examples(void)
{
  int out[8];
  uint32_t w[8];
  int d[8];

  w[0] = 3000; w[1] = 1000; d[0] = d[1] = 200;
  CHECK(nr_ctrl_split(2, w, d, 5, 100, out) == 100);
  CHECK(out[0] == 75 && out[1] == 25);

  w[0] = w[1] = 1000;
  CHECK(nr_ctrl_split(2, w, d, 5, 106, out) == 106);
  CHECK(out[0] == 53 && out[1] == 53);

  d[0] = 10; d[1] = 200;
  CHECK(nr_ctrl_split(2, w, d, 5, 100, out) == 100);
  CHECK(out[0] == 10 && out[1] == 90); /* surplus of the small user goes to the other */

  w[0] = 1000000; w[1] = 1; d[0] = d[1] = 200;
  CHECK(nr_ctrl_split(2, w, d, 5, 100, out) == 100);
  CHECK(out[0] == 95 && out[1] == 5); /* minimum grant is honoured */

  d[0] = 7; d[1] = 9;
  CHECK(nr_ctrl_split(2, w, d, 5, 100, out) == 16); /* demand below supply */
  CHECK(out[0] == 7 && out[1] == 9);

  w[0] = w[1] = w[2] = 1; d[0] = d[1] = d[2] = 200;
  CHECK(nr_ctrl_split(3, w, d, 5, 12, out) == 12); /* only two minimum grants fit */
  CHECK(out[0] == 6 && out[1] == 6 && out[2] == 0);

  CHECK(nr_ctrl_split(2, w, d, 5, 4, out) == 0 && out[0] == 0 && out[1] == 0);
  CHECK(nr_ctrl_split(0, w, d, 5, 100, out) == 0);
}

static void test_split_properties(void)
{
  int bad = 0;
  for (int iter = 0; iter < 200000; iter++) {
    int n = 1 + (int)(rnd() % 8);
    int min_rb = 1 + (int)(rnd() % 6);
    int total = (int)(rnd() % 300);
    uint32_t w[8];
    int d[8], out[8];
    for (int i = 0; i < n; i++) {
      w[i] = 1 + rnd() % 100000;
      d[i] = (int)(rnd() % 3 == 0 ? rnd() % 20 : rnd() % 400);
    }
    int sum = nr_ctrl_split(n, w, d, min_rb, total, out);
    int m = total / min_rb < n ? total / min_rb : n;
    int s = 0;
    long sum_hi = 0;
    for (int i = 0; i < m; i++)
      sum_hi += d[i] < min_rb ? min_rb : d[i];
    for (int i = 0; i < n; i++) {
      int hi = d[i] < min_rb ? min_rb : d[i];
      s += out[i];
      if (i >= m) {
        if (out[i] != 0)
          bad++;
      } else if (out[i] < min_rb || out[i] > hi) {
        bad++;
      }
    }
    if (s != sum || s > total)
      bad++;
    if (m > 0) {
      if (sum_hi >= total && s != total)
        bad++; /* work conserving */
      if (sum_hi < total && s != sum_hi)
        bad++; /* everybody fully served */
    }
    /* weighted max-min: an unsaturated user's level is not below that of a user
     * above its minimum, up to rounding */
    for (int i = 0; i < m; i++)
      for (int j = 0; j < m; j++) {
        int hi_i = d[i] < min_rb ? min_rb : d[i];
        if (out[i] < hi_i && out[j] > min_rb) {
          /* out_j / w_j <= out_i / w_i + 1/w_i + 1/w_j */
          double lhs = (double)out[j] / w[j];
          double rhs = (double)out[i] / w[i] + 1.0 / w[i] + 1.0 / w[j];
          if (lhs > rhs + 1e-9)
            bad++;
        }
      }
  }
  CHECK(bad == 0);
}

/* ------------------------------------------------------------- protocol */

static nr_ctrl_ue_view_t view2[2] = {{.rnti = 0x1001}, {.rnti = 0x1002}};

static int run(nr_ctrl_state_t *st, const char *cmd, char *reply)
{
  return nr_ctrl_handle(st, cmd, view2, 2, reply, 4096);
}

static void test_protocol(void)
{
  nr_ctrl_state_t st;
  char r[4096];
  uint32_t v0;
  memset(&st, 0, sizeof(st));
  nr_ctrl_init(&st);

  run(&st, "PING", r);
  CHECK(!strncmp(r, "OK", 2));
  run(&st, "GET", r);
  CHECK(strstr(r, "policy=pf") && strstr(r, "version=0") && strstr(r, "rnti=0x1001") && strstr(r, "rnti=0x1002"));

  run(&st, "policy WSHARE", r);
  CHECK(!strncmp(r, "OK version=1 policy=wshare", 26));
  CHECK(st.policy == NR_CTRL_POLICY_WSHARE);
  run(&st, "POLICY wrr", r);
  CHECK(!strncmp(r, "OK version=2 policy=wrr", 23) && st.policy == NR_CTRL_POLICY_WRR);
  run(&st, "policy WSHARE", r);
  CHECK(st.policy == NR_CTRL_POLICY_WSHARE);
  v0 = st.version;
  run(&st, "POLICY bogus", r);
  CHECK(!strncmp(r, "ERR unknown_policy", 18) && st.version == v0);

  run(&st, "WEIGHT 1001 3000", r);
  CHECK(!strncmp(r, "OK version=4", 12));
  CHECK(nr_ctrl_weight_of(&st, 0x1001) == 3000 && nr_ctrl_weight_of(&st, 0x1002) == NR_CTRL_DEFAULT_WEIGHT);
  run(&st, "WEIGHT 0x1002 500", r);
  CHECK(nr_ctrl_weight_of(&st, 0x1002) == 500);
  run(&st, "WEIGHT * 2000", r);
  CHECK(st.default_weight == 2000 && nr_ctrl_weight_of(&st, 0x1001) == 3000);

  uint32_t v = st.version;
  const char *bad[] = {"WEIGHT 1001 0", "WEIGHT 1001 1000001", "WEIGHT 1001 -5", "WEIGHT 1001 abc", "WEIGHT 1001 5x",
                       "WEIGHT 9999 10", "WEIGHT zz 10", "WEIGHT 0 10", "WEIGHT 10000 10", "WEIGHT 1001", "WEIGHT 1001 10 extra",
                       "CAP 1001 3", "CAP 1001 276", "CAP 9999 10", "CAP 1001 -1", "CAP 1001 +20", "NOPE", "", "   ",
                       "POLICY", "POLICY pf rr", "GET now", "WEIGHT  1001  1e3"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    run(&st, bad[i], r);
    CHECK(!strncmp(r, "ERR", 3));
  }
  CHECK(st.version == v); /* nothing rejected changed the state */

  run(&st, "CAP 1001 20", r);
  CHECK(nr_ctrl_cap_of(&st, 0x1001) == 20);
  run(&st, "CAP 1001 0", r);
  CHECK(nr_ctrl_cap_of(&st, 0x1001) == 0);

  char longcmd[400];
  memset(longcmd, 'A', sizeof(longcmd) - 1);
  longcmd[sizeof(longcmd) - 1] = 0;
  run(&st, longcmd, r);
  CHECK(!strncmp(r, "ERR command_too_long", 20));

  nr_ctrl_account(&st, 0x1001, 40);
  nr_ctrl_account(&st, 0x1001, 10);
  run(&st, "GET", r);
  CHECK(strstr(r, "granted_rbs=50 granted_slots=2"));

  uint32_t before = st.version;
  run(&st, "RESET", r);
  CHECK(st.policy == NR_CTRL_POLICY_PF && st.default_weight == NR_CTRL_DEFAULT_WEIGHT && st.n_ue == 0);
  CHECK(st.version == before + 1); /* version keeps increasing across a reset */

  /* pruning */
  run(&st, "WEIGHT 1001 10", r);
  run(&st, "WEIGHT 1002 10", r);
  CHECK(st.n_ue == 2);
  nr_ctrl_prune(&st, &view2[1], 1);
  CHECK(st.n_ue == 1 && st.ue[0].rnti == 0x1002 && st.ue[0].weight == 10);

  /* table limit */
  nr_ctrl_init(&st);
  for (int i = 0; i < NR_CTRL_MAX_UE + 4; i++)
    nr_ctrl_account(&st, (uint16_t)(0x2000 + i), 1);
  CHECK(st.n_ue == NR_CTRL_MAX_UE);
}

/* --------------------------------------------------------- policy in slots */

#define NSLOT_RB 106
static gNB_MAC_INST mac;
static NR_UE_info_t ues[40];
static uint16_t vrb[MAX_NUM_BEAM_PERIODS][275];
static nr_dl_candidate_t cands[40];

static void sim_setup(int n_ue)
{
  static bool once;
  if (!once) {
    pthread_mutex_init(&mac.sched_lock, NULL);
    once = true;
  }
  NR_SCHED_LOCK(&mac.sched_lock);
  memset(ues, 0, sizeof(ues));
  memset(mac.UE_info.connected_ue_list, 0, sizeof(mac.UE_info.connected_ue_list));
  for (int i = 0; i < n_ue; i++) {
    ues[i].rnti = (uint16_t)(0x1001 + i);
    mac.UE_info.connected_ue_list[i] = &ues[i];
  }
  mac.dl_rb_alloc = nr_dl_ctrl_policy;
  NR_SCHED_UNLOCK(&mac.sched_lock);
}

static void sim_slot_prepare(int n_ue, uint32_t pending)
{
  memset(vrb, 0, sizeof(vrb));
  for (int i = 0; i < n_ue; i++) {
    memset(&cands[i], 0, sizeof(cands[i]));
    cands[i].UE = &ues[i];
    cands[i].rnti = ues[i].rnti;
    cands[i].pending_bytes = pending;
    cands[i].bwp_start = 0;
    cands[i].bwp_size = NSLOT_RB;
    cands[i].alloc_slbitmap = 0x3fff;
    cands[i].retx_harq_pid = -1;
    cands[i].sched_pdsch.mcs = 20;
    cands[i].sched_pdsch.nrOfLayers = 1;
    cands[i].sched_pdsch.tda_info.nrOfSymbols = 12;
  }
}

static int sim_slot_no;

static int sim_slot(int n_ue, int max_ue)
{
  nr_dl_sched_params_t p = {.mac = &mac, .max_num_ue = max_ue, .num_beams = 1, .slot = sim_slot_no};
  p.vrb_map[0] = vrb[0];
  p.n_rb_avail[0] = NSLOT_RB;
  NR_SCHED_LOCK(&mac.sched_lock);
  int r = mac.dl_rb_alloc(&p, cands, n_ue);
  NR_SCHED_UNLOCK(&mac.sched_lock);
  return r;
}

static int udp_fd = -1;
static struct sockaddr_in ctrl_addr;

static int ctl(const char *cmd, char *reply, size_t size)
{
  sendto(udp_fd, cmd, strlen(cmd), 0, (struct sockaddr *)&ctrl_addr, sizeof(ctrl_addr));
  ssize_t r = recv(udp_fd, reply, size - 1, 0);
  if (r < 0) {
    reply[0] = 0;
    return -1;
  }
  reply[r] = 0;
  return (int)r;
}

static void start_endpoint(void)
{
  int port = 20000 + (int)(getpid() % 20000);
  char ps[16];
  snprintf(ps, sizeof(ps), "%d", port);
  setenv("SCHED_CTRL_PORT", ps, 1);
  setenv("SCHED_CTRL_ADDR", "127.0.0.1", 1);
  nr_dl_ctrl_policy_init(&mac);
  udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct timeval tv = {.tv_sec = 5};
  setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ctrl_addr = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
  inet_pton(AF_INET, "127.0.0.1", &ctrl_addr.sin_addr);
  char r[256];
  for (int i = 0; i < 50; i++) { /* wait for the thread to bind */
    if (ctl("PING", r, sizeof(r)) > 0)
      return;
    usleep(100000);
  }
}


static long get_field(const char *r, const char *key)
{
  const char *p = strstr(r, key);
  return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

/* Saturated downlink for two UEs where the MAC accepts only `budget` grants per UE
 * in each group of `period` slots (the ACK/NACK allowance).  Reports, for the second
 * half of the run, the share of resource blocks used and UE 1's part of them. */
static void run_budget(int budget, int period, int nslots, long w1, long w2, double *util, double *share1, long *lost, long *share_milli)
{
  char r[4096], cmd[64];
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wshare", r, sizeof(r));
  snprintf(cmd, sizeof(cmd), "WEIGHT 1001 %ld", w1);
  ctl(cmd, r, sizeof(r));
  snprintf(cmd, sizeof(cmd), "WEIGHT 1002 %ld", w2);
  ctl(cmd, r, sizeof(r));
  stub_ack_budget = budget;
  stub_ack_period = period;
  stub_ack_reset();
  double used = 0, a = 0, b = 0;
  int counted = 0;
  for (int s = 0; s < nslots; s++) {
    sim_slot_no = s;
    sim_slot_prepare(2, 1000000);
    sim_slot(2, 4);
    if (s >= nslots / 2) {
      counted++;
      for (int i = 0; i < 2; i++)
        if (cands[i].scheduled) {
          used += cands[i].sched_pdsch.rbSize;
          if (i == 0)
            a += cands[i].sched_pdsch.rbSize;
          else
            b += cands[i].sched_pdsch.rbSize;
        }
    }
  }
  stub_ack_budget = 0;
  stub_ack_period = 0;
  sim_slot_no = 0;
  ctl("GET", r, sizeof(r));
  *util = used / ((double)counted * NSLOT_RB);
  *share1 = a / (a + b);
  *lost = get_field(r, "ws_lost_slots=");
  *share_milli = get_field(r, "ws_share_milli=");
}

static void test_budget(void)
{
  double util, share1;
  long lost, milli;

  /* allowance of 5 grants per UE in 7 slots: two UEs can have at most three shared slots */
  run_budget(5, 7, 14000, 1000, 1000, &util, &share1, &lost, &milli);
  CHECK(util > 0.9);
  CHECK(fabs(share1 - 0.5) < 0.03);
  CHECK(milli < 800); /* sharing every slot would not fit */
  /* weights 3:1 ask for 75 % but a UE can hold at most 5 of 7 slots (71 %) with that allowance */
  run_budget(5, 7, 14000, 3000, 1000, &util, &share1, &lost, &milli);
  CHECK(util > 0.9);
  CHECK(fabs(share1 - 5.0 / 7.0) < 0.02);

  /* no effective limit: the slot stays shared and nothing is lost */
  run_budget(7, 7, 6000, 1000, 1000, &util, &share1, &lost, &milli);
  CHECK(lost == 0 && util > 0.999 && milli > 900);
  CHECK(fabs(share1 - 0.5) < 0.01);
  run_budget(7, 7, 6000, 3000, 1000, &util, &share1, &lost, &milli);
  CHECK(fabs(share1 - 0.75) < 0.02);

  /* allowance too small to cover every slot: no crash, most slots still used */
  run_budget(3, 7, 14000, 1000, 1000, &util, &share1, &lost, &milli);
  CHECK(util > 0.78 && util < 0.87);
  CHECK(milli >= NR_CTRL_WS_SHARE_MIN);

  /* grants refused only for lack of a PDCCH candidate say nothing about sharing:
   * the fraction must not fall; ACK/NACK refusals of the same UE must lower it */
  char r[4096];
  for (int pass = 0; pass < 2; pass++) {
    ctl("RESET", r, sizeof(r));
    ctl("POLICY wshare", r, sizeof(r));
    stub_commit_fail_rnti = 0x1001;
    stub_fail_is_cce = pass == 0;
    for (int s = 0; s < 2000; s++) {
      sim_slot_no = s;
      sim_slot_prepare(2, 1000000);
      sim_slot(2, 4);
    }
    stub_commit_fail_rnti = 0;
    stub_fail_is_cce = 0;
    sim_slot_no = 0;
    ctl("GET", r, sizeof(r));
    if (pass == 0)
      CHECK(get_field(r, "ws_share_milli=") >= NR_CTRL_WS_SHARE_INIT);
    else
      CHECK(get_field(r, "ws_share_milli=") == NR_CTRL_WS_SHARE_MIN);
  }
}

static void test_policy(void)
{
  char r[4096];
  sim_setup(2);
  start_endpoint();

  ctl("PING", r, sizeof(r));
  CHECK(!strncmp(r, "OK ctrl", 7));

  /* default: stock policy is called, nothing of ours runs */
  sim_slot_prepare(2, 1000000);
  stub_pf_calls = 0;
  sim_slot(2, 4);
  CHECK(stub_pf_calls == 1);

  /* weighted 3:1 over many slots */
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wshare", r, sizeof(r));
  CHECK(!strncmp(r, "OK", 2));
  ctl("WEIGHT 1001 3000", r, sizeof(r));
  CHECK(!strncmp(r, "OK", 2));
  ctl("WEIGHT 1002 1000", r, sizeof(r));
  uint32_t b1 = ues[0].mac_stats.dl.total_rbs, b2 = ues[1].mac_stats.dl.total_rbs;
  int slots = 2000;
  for (int s = 0; s < slots; s++) {
    sim_slot_prepare(2, 1000000);
    CHECK(sim_slot(2, 4) >= 1);
  }
  double g1 = ues[0].mac_stats.dl.total_rbs - b1, g2 = ues[1].mac_stats.dl.total_rbs - b2;
  CHECK(fabs(g1 / g2 - 3.0) < 0.1);
  CHECK((g1 + g2) == (double)slots * NSLOT_RB); /* every block used */
  ctl("GET", r, sizeof(r));
  CHECK(strstr(r, "policy=wshare") && strstr(r, "weight=3000 explicit=1"));
  char want[64];
  snprintf(want, sizeof(want), "mac_dl_rbs=%u", ues[0].mac_stats.dl.total_rbs);
  CHECK(strstr(r, want)); /* read-back matches the MAC counters */

  /* equal weights */
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wshare", r, sizeof(r));
  b1 = ues[0].mac_stats.dl.total_rbs; b2 = ues[1].mac_stats.dl.total_rbs;
  for (int s = 0; s < slots; s++) {
    sim_slot_prepare(2, 1000000);
    sim_slot(2, 4);
  }
  g1 = ues[0].mac_stats.dl.total_rbs - b1; g2 = ues[1].mac_stats.dl.total_rbs - b2;
  CHECK(fabs(g1 / g2 - 1.0) < 0.01);

  /* cap: UE1 limited to 20 RBs per slot, UE2 takes the rest */
  ctl("CAP 1001 20", r, sizeof(r));
  CHECK(!strncmp(r, "OK", 2));
  sim_slot_prepare(2, 1000000);
  sim_slot(2, 4);
  CHECK(cands[0].sched_pdsch.rbSize == 20 && cands[1].sched_pdsch.rbSize == NSLOT_RB - 20);
  CHECK(cands[0].sched_pdsch.rbStart + cands[0].sched_pdsch.rbSize == cands[1].sched_pdsch.rbStart
        || cands[1].sched_pdsch.rbStart + cands[1].sched_pdsch.rbSize == cands[0].sched_pdsch.rbStart);
  ctl("CAP 1001 0", r, sizeof(r));

  /* small backlog: UE2 needs 5 RBs, UE1 takes everything else */
  sim_slot_prepare(2, 1000000);
  cands[1].pending_bytes = 400;
  sim_slot(2, 4);
  CHECK(cands[1].sched_pdsch.rbSize == 5 && cands[0].sched_pdsch.rbSize == NSLOT_RB - 5);

  /* allocations never overlap and stay inside the band */
  sim_slot_prepare(2, 1000000);
  sim_slot(2, 4);
  uint16_t seen[275] = {0};
  for (int i = 0; i < 2; i++)
    for (int rb = cands[i].sched_pdsch.rbStart; rb < cands[i].sched_pdsch.rbStart + cands[i].sched_pdsch.rbSize; rb++) {
      CHECK(rb < NSLOT_RB && !seen[rb]);
      seen[rb] = 1;
    }

  /* only one UE schedulable per slot */
  sim_slot_prepare(2, 1000000);
  CHECK(sim_slot(2, 1) == 1);
  CHECK(cands[0].scheduled != cands[1].scheduled);
  CHECK((cands[0].scheduled ? cands[0] : cands[1]).sched_pdsch.rbSize == NSLOT_RB);

  /* HARQ retransmission first and with its exact size; no-data UE gets the minimum */
  sim_slot_prepare(2, 1000000);
  cands[0].is_retx = true;
  cands[0].retx_rbSize = 10;
  cands[0].pending_bytes = 0;
  CHECK(sim_slot(2, 4) == 2);
  CHECK(cands[0].sched_pdsch.rbSize == 10 && cands[0].sched_pdsch.rbStart == 0);
  CHECK(cands[1].sched_pdsch.rbSize == NSLOT_RB - 10);
  sim_slot_prepare(2, 0);
  CHECK(sim_slot(2, 4) == 2);
  CHECK(cands[0].sched_pdsch.rbSize == NR_CTRL_MIN_RB && cands[1].sched_pdsch.rbSize == NR_CTRL_MIN_RB);

  /* a refused grant is counted and its share goes to the other UE */
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wshare", r, sizeof(r));
  stub_commit_fail_rnti = 0x1001;
  sim_slot_prepare(2, 1000000);
  sim_slot(2, 4);
  stub_commit_fail_rnti = 0;
  CHECK(!cands[0].scheduled && cands[1].scheduled);
  CHECK(cands[1].sched_pdsch.rbSize == NSLOT_RB && cands[1].sched_pdsch.rbStart == 0);
  ctl("GET", r, sizeof(r));
  CHECK(strstr(r, "commit_fail=1 fail_cce=0 fail_pucch=1 multi_ue_slots=0"));
  CHECK(strstr(r, "ws_groups=1 ws_free_rb=106"));
  CHECK(strstr(r, "granted_rbs=0 granted_slots=0 commit_fail=1")); /* UE 0x1001 */

  /* a refusal of the later UE: the first one is extended to use the freed share */
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wshare", r, sizeof(r));
  stub_commit_fail_rnti = 0x1002;
  stub_fail_is_cce = 1;
  sim_slot_prepare(2, 1000000);
  sim_slot(2, 4);
  stub_commit_fail_rnti = 0;
  stub_fail_is_cce = 0;
  CHECK(cands[0].scheduled && !cands[1].scheduled);
  CHECK(cands[0].sched_pdsch.rbSize == NSLOT_RB && cands[0].sched_pdsch.rbStart == 0);
  {
    int used = 0;
    for (int rb = 0; rb < NSLOT_RB; rb++)
      used += (vrb[0][rb] & 0x3fff) != 0;
    CHECK(used == NSLOT_RB); /* the extension is marked in the resource map */
  }
  ctl("GET", r, sizeof(r));
  CHECK(strstr(r, "fail_cce=1 fail_pucch=0"));

  /* extension never exceeds the UE's demand */
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wshare", r, sizeof(r));
  stub_commit_fail_rnti = 0x1002;
  sim_slot_prepare(2, 1000000);
  cands[0].pending_bytes = 2988; /* 30 RBs with the 12 byte MAC overhead */
  sim_slot(2, 4);
  stub_commit_fail_rnti = 0;
  CHECK(cands[0].sched_pdsch.rbSize == 30);

  /* weighted round robin: one UE per slot, resource blocks follow the weights */
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wrr", r, sizeof(r));
  CHECK(!strncmp(r, "OK", 2));
  ctl("WEIGHT 1001 3000", r, sizeof(r));
  ctl("WEIGHT 1002 1000", r, sizeof(r));
  {
    uint32_t w1 = ues[0].mac_stats.dl.total_rbs, w2 = ues[1].mac_stats.dl.total_rbs;
    int one_per_slot = 1, n1 = 0;
    for (int s = 0; s < 2000; s++) {
      sim_slot_prepare(2, 1000000);
      int n = sim_slot(2, 4);
      if (n != 1)
        one_per_slot = 0;
      n1 += cands[0].scheduled;
    }
    double r1 = ues[0].mac_stats.dl.total_rbs - w1, r2 = ues[1].mac_stats.dl.total_rbs - w2;
    CHECK(one_per_slot);
    CHECK(fabs(r1 / r2 - 3.0) < 0.05);
    CHECK(r1 + r2 == 2000.0 * NSLOT_RB);
    CHECK(n1 == 1500);
  }
  /* wrr with a cap on the leading UE: the next UE uses the rest of the slot */
  ctl("CAP 1001 30", r, sizeof(r));
  sim_slot_prepare(2, 1000000);
  CHECK(sim_slot(2, 4) == 2);
  CHECK(cands[0].sched_pdsch.rbSize == 30 && cands[1].sched_pdsch.rbSize == NSLOT_RB - 30);
  ctl("CAP 1001 0", r, sizeof(r));

  /* round robin: one UE per slot, strictly alternating */
  ctl("RESET", r, sizeof(r));
  ctl("POLICY rr", r, sizeof(r));
  int served[2] = {0, 0}, last = -1, alt = 1;
  for (int s = 0; s < 1000; s++) {
    sim_slot_prepare(2, 1000000);
    int n = sim_slot(2, 4);
    CHECK(n == 1);
    int who = cands[0].scheduled ? 0 : 1;
    served[who]++;
    if (last >= 0 && who == last)
      alt = 0;
    last = who;
  }
  CHECK(alt && served[0] == 500 && served[1] == 500);

}

/* ------------------------------------------------------------ E2 transport */

/* One command as the gNB's E2 handler delivers it: two words in, text out, sent to the
 * live control endpoint.  Returns 0 when the controller answered OK. */
static int e2_deliver(uint32_t hdr, uint32_t act, char *reply, size_t size)
{
  char text[128];
  if (nr_ctrl_e2_to_text(hdr, act, text, sizeof(text)) != 0) {
    snprintf(reply, size, "not a scheduler command");
    return -2;
  }
  return nr_ctrl_e2_send(text, reply, size);
}

/* The "weight=<n> explicit=.. cap=<n>" part of one UE's line of the GET reply. */
static long ue_field(const char *get, const char *rnti, const char *key)
{
  char pat[32];
  snprintf(pat, sizeof(pat), "rnti=%s ", rnti);
  const char *p = strstr(get, pat);
  return p ? get_field(p, key) : -1;
}

static void test_e2(void)
{
  char r[4096], g[4096];
  uint32_t hdr, act;
  const char *err = NULL;

  sim_setup(2);
  ctl("RESET", r, sizeof(r));

  /* the words the xApp builds, against values written out by hand */
  CHECK(nr_ctrl_e2_pack("policy", "wshare", NULL, NULL, &hdr, &act, &err) == 0 && hdr == 0x53430001u && act == 2);
  CHECK(nr_ctrl_e2_pack("policy", "wrr", NULL, NULL, &hdr, &act, &err) == 0 && act == 3);
  CHECK(nr_ctrl_e2_pack("policy", "pf", NULL, NULL, &hdr, &act, &err) == 0 && act == 0);
  CHECK(nr_ctrl_e2_pack("policy", "rr", NULL, NULL, &hdr, &act, &err) == 0 && act == 1);
  CHECK(nr_ctrl_e2_pack("weight", "1001", "3000", NULL, &hdr, &act, &err) == 0 && hdr == 0x53430002u && act == 0x10010bb8u);
  CHECK(nr_ctrl_e2_pack("weight", "*", "7", NULL, &hdr, &act, &err) == 0 && act == 7);
  CHECK(nr_ctrl_e2_pack("cap", "1002", "20", NULL, &hdr, &act, &err) == 0 && hdr == 0x53430003u && act == 0x10020014u);
  CHECK(nr_ctrl_e2_pack("reset", NULL, NULL, NULL, &hdr, &act, &err) == 0 && hdr == 0x53430004u && act == 0);

  /* words the xApp refuses to build */
  CHECK(nr_ctrl_e2_pack("policy", "bogus", NULL, NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("policy", NULL, NULL, NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("weight", "1001", "0", NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("weight", "1001", "65536", NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("weight", "0", "5", NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("weight", "10000", "5", NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("weight", "zz", "5", NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("weight", "1001", "5x", NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("cap", "1001", "3", NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("cap", "1001", "276", NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("cap", "*", "20", NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("nope", NULL, NULL, NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack(NULL, NULL, NULL, NULL, &hdr, &act, &err) != 0);

  /* words the gNB refuses to translate (the stock example sends header 1, message 42) */
  char text[128];
  CHECK(nr_ctrl_e2_to_text(1, 42, text, sizeof(text)) != 0);
  CHECK(nr_ctrl_e2_to_text(0x53430000u, 0, text, sizeof(text)) != 0);
  CHECK(nr_ctrl_e2_to_text(0x53430005u, 0, text, sizeof(text)) != 0);
  CHECK(nr_ctrl_e2_to_text(0x53430001u, 5, text, sizeof(text)) != 0);
  CHECK(nr_ctrl_e2_to_text(0x53430002u, 0x10010000u, text, sizeof(text)) != 0); /* weight 0 */
  CHECK(nr_ctrl_e2_to_text(0x53430003u, 0x00000014u, text, sizeof(text)) != 0); /* cap without rnti */
  CHECK(nr_ctrl_e2_to_text(0x53430003u, 0x10010003u, text, sizeof(text)) != 0); /* cap 3 */
  CHECK(nr_ctrl_e2_to_text(0x53430004u, 1, text, sizeof(text)) != 0);
  CHECK(nr_ctrl_e2_to_text(0x53430001u, 2, text, 4) != 0); /* buffer too small */

  /* the text for each command */
  CHECK(nr_ctrl_e2_to_text(0x53430001u, 3, text, sizeof(text)) == 0 && !strcmp(text, "POLICY wrr"));
  CHECK(nr_ctrl_e2_to_text(0x53430002u, 0x0000000au, text, sizeof(text)) == 0 && !strcmp(text, "WEIGHT * 10"));
  CHECK(nr_ctrl_e2_to_text(0x53430002u, 0xa6a4ffffu, text, sizeof(text)) == 0 && !strcmp(text, "WEIGHT a6a4 65535"));
  CHECK(nr_ctrl_e2_to_text(0x53430003u, 0x00e30005u, text, sizeof(text)) == 0 && !strcmp(text, "CAP 00e3 5"));

  /* end to end against the live controller: xApp words -> gNB translation -> state */
  CHECK(nr_ctrl_e2_pack("policy", "wshare", NULL, NULL, &hdr, &act, &err) == 0 && e2_deliver(hdr, act, r, sizeof(r)) == 0);
  ctl("GET", g, sizeof(g));
  CHECK(strstr(g, "policy=wshare") != NULL);

  CHECK(nr_ctrl_e2_pack("weight", "1001", "3000", NULL, &hdr, &act, &err) == 0 && e2_deliver(hdr, act, r, sizeof(r)) == 0);
  CHECK(nr_ctrl_e2_pack("weight", "1002", "1000", NULL, &hdr, &act, &err) == 0 && e2_deliver(hdr, act, r, sizeof(r)) == 0);
  CHECK(nr_ctrl_e2_pack("cap", "1002", "20", NULL, &hdr, &act, &err) == 0 && e2_deliver(hdr, act, r, sizeof(r)) == 0);
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1001", "weight=") == 3000 && ue_field(g, "0x1001", "cap=") == 0);
  CHECK(ue_field(g, "0x1002", "weight=") == 1000 && ue_field(g, "0x1002", "cap=") == 20);

  CHECK(nr_ctrl_e2_pack("weight", "*", "250", NULL, &hdr, &act, &err) == 0 && e2_deliver(hdr, act, r, sizeof(r)) == 0);
  ctl("GET", g, sizeof(g));
  CHECK(get_field(g, "default_weight=") == 250);

  CHECK(nr_ctrl_e2_pack("cap", "1002", "0", NULL, &hdr, &act, &err) == 0 && e2_deliver(hdr, act, r, sizeof(r)) == 0);
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1002", "cap=") == 0);

  /* the controller's own refusals reach the caller */
  CHECK(nr_ctrl_e2_pack("weight", "7777", "5", NULL, &hdr, &act, &err) == 0); /* valid words, unknown UE */
  CHECK(e2_deliver(hdr, act, r, sizeof(r)) != 0 && !strncmp(r, "ERR", 3));
  CHECK(e2_deliver(1, 42, r, sizeof(r)) == -2);

  CHECK(nr_ctrl_e2_pack("policy", "wrr", NULL, NULL, &hdr, &act, &err) == 0 && e2_deliver(hdr, act, r, sizeof(r)) == 0);
  ctl("GET", g, sizeof(g));
  CHECK(strstr(g, "policy=wrr") != NULL);

  CHECK(nr_ctrl_e2_pack("reset", NULL, NULL, NULL, &hdr, &act, &err) == 0 && e2_deliver(hdr, act, r, sizeof(r)) == 0);
  ctl("GET", g, sizeof(g));
  CHECK(strstr(g, "policy=pf") != NULL && get_field(g, "default_weight=") == NR_CTRL_DEFAULT_WEIGHT);

  /* nothing listening: the sender reports failure instead of hanging */
  char *old_port = strdup(getenv("SCHED_CTRL_PORT"));
  setenv("SCHED_CTRL_PORT", "9", 1);
  struct timeval t0, t1;
  gettimeofday(&t0, NULL);
  CHECK(nr_ctrl_e2_send("PING", r, sizeof(r)) != 0);
  gettimeofday(&t1, NULL);
  CHECK((t1.tv_sec - t0.tv_sec) < 4);
  setenv("SCHED_CTRL_PORT", old_port, 1);
  free(old_port);
}

/* A second gNB object cannot be controlled; verify the single-instance guard. */
/* ------------------------------------------------------------ slices */

static nssai_t slice_nssai[40]; /* S-NSSAI the simulated MAC reports for each UE */

/* `slots` saturated slots; UEs flagged in skip[] have no data (not offered to the policy).
 * Adds the new-data resource blocks of each UE to got[]. */
static void slice_run(int n_ue, const int *skip, int slots, double *got)
{
  for (int s = 0; s < slots; s++) {
    sim_slot_prepare(n_ue, 1000000);
    for (int i = 0; i < n_ue; i++) {
      cands[i].nssai = slice_nssai[i];
      if (skip && skip[i])
        cands[i].skipped = true;
    }
    sim_slot(n_ue, 4);
    for (int i = 0; i < n_ue; i++)
      if (cands[i].scheduled && !cands[i].is_retx)
        got[i] += cands[i].sched_pdsch.rbSize;
  }
}

static long slice_field(const char *get, int id, const char *key)
{
  char pat[24];
  snprintf(pat, sizeof(pat), "SLICE id=%d ", id);
  const char *p = strstr(get, pat);
  return p ? get_field(p, key) : -1;
}

/* 0 default, 1 rnti, 2 nssai of one UE's line; -1 when the UE is not listed */
static int ue_source(const char *get, const char *rnti)
{
  char pat[32];
  snprintf(pat, sizeof(pat), "rnti=%s ", rnti);
  const char *p = strstr(get, pat);
  if (!p)
    return -1;
  const char *eol = strchr(p, '\n');
  const char *q = strstr(p, "source=");
  if (!q || (eol && q > eol))
    return -1;
  return !strncmp(q + 7, "rnti", 4) ? 1 : (!strncmp(q + 7, "nssai", 5) ? 2 : 0);
}

static int count_of(const char *hay, const char *needle)
{
  int n = 0;
  for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle))
    n++;
  return n;
}

static void slice_policy_on(void)
{
  char r[4096];
  ctl("RESET", r, sizeof(r));
  ctl("POLICY slice", r, sizeof(r));
}

/* three UEs: 1001 and 1002 in slice 1 (slice cap `scap`), 1003 in slice 2; UE 1001 gets `ucap` */
static void cap_case(int scap, int ucap)
{
  char r[4096], c[48];
  slice_policy_on();
  ctl("ASSOC 1001 1", r, sizeof(r));
  ctl("ASSOC 1002 1", r, sizeof(r));
  ctl("ASSOC 1003 2", r, sizeof(r));
  snprintf(c, sizeof(c), "SLICE 1 CAP %d", scap);
  ctl(c, r, sizeof(r));
  snprintf(c, sizeof(c), "CAP 1001 %d", ucap);
  ctl(c, r, sizeof(r));
}

static void test_slices(void)
{
  char r[4096], g[8192];
  double got[40];

  /* --- commands, limits and read-back */
  sim_setup(4);
  memset(slice_nssai, 0, sizeof(slice_nssai));
  ctl("RESET", r, sizeof(r));
  ctl("POLICY slice", r, sizeof(r));
  CHECK(strstr(r, "OK") && strstr(r, "policy=slice"));
  ctl("GET", g, sizeof(g));
  long v0 = get_field(g, "version=");
  const char *bad[] = {"SLICE 8 WEIGHT 1000", "SLICE 1 WEIGHT 0", "SLICE 1 WEIGHT 1000001", "SLICE 1 CAP 4", "SLICE 1 CAP 276",
                       "SLICE 1 SHARE 5", "SLICE x WEIGHT 5", "SLICE 1 WEIGHT", "ASSOC 1001 8", "ASSOC 1001 x", "ASSOC 9999 1",
                       "ASSOC zz 1", "ASSOC 1001", "NSSAIMAP 256 none 1", "NSSAIMAP 1 zz 1", "NSSAIMAP 1 1000000 1", "NSSAIMAP 1 none 8",
                       "NSSAIMAP 1 none -", "NSSAIMAP 1 none", "POLICY slices"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    ctl(bad[i], r, sizeof(r));
    CHECK(!strncmp(r, "ERR", 3));
  }
  ctl("GET", g, sizeof(g));
  CHECK(get_field(g, "version=") == v0); /* a refused command changes nothing */
  CHECK(!strstr(g, "SLICE id=") && !strstr(g, "RULE "));

  const char *good[] = {"SLICE 1 WEIGHT 3000", "slice 2 weight 1000", "SLICE 2 CAP 30", "ASSOC 1001 1", "ASSOC 1002 1", "ASSOC 1003 2", "NSSAIMAP 2 none 3"};
  for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
    ctl(good[i], r, sizeof(r));
    CHECK(!strncmp(r, "OK", 2));
  }
  ctl("GET", g, sizeof(g));
  CHECK(get_field(g, "version=") == v0 + 7);
  CHECK(slice_field(g, 1, "weight=") == 3000 && slice_field(g, 1, "ues=") == 2);
  CHECK(slice_field(g, 2, "weight=") == 1000 && slice_field(g, 2, "cap=") == 30 && slice_field(g, 2, "ues=") == 1);
  CHECK(slice_field(g, 0, "ues=") == 1); /* UE 0x1004 */
  CHECK(strstr(g, "RULE sst=2 sd=ffffff slice=3"));
  CHECK(ue_field(g, "0x1001", "slice=") == 1 && ue_source(g, "0x1001") == 1);
  CHECK(ue_field(g, "0x1004", "slice=") == 0 && ue_source(g, "0x1004") == 0);
  /* the same rule is replaced, not duplicated; a specific sd is another rule */
  ctl("NSSAIMAP 2 none 4", r, sizeof(r));
  ctl("NSSAIMAP 2 123456 5", r, sizeof(r));
  ctl("GET", g, sizeof(g));
  CHECK(count_of(g, "RULE ") == 2 && strstr(g, "RULE sst=2 sd=ffffff slice=4") && strstr(g, "RULE sst=2 sd=123456 slice=5"));
  ctl("NSSAIMAP 2 123456 -", r, sizeof(r));
  CHECK(!strncmp(r, "OK", 2));
  ctl("GET", g, sizeof(g));
  CHECK(count_of(g, "RULE ") == 1);
  for (int i = 10; i < 17; i++) {
    char c[48];
    snprintf(c, sizeof(c), "NSSAIMAP %d none 1", i);
    ctl(c, r, sizeof(r));
    CHECK(!strncmp(r, "OK", 2));
  }
  ctl("NSSAIMAP 99 none 1", r, sizeof(r));
  CHECK(!strncmp(r, "ERR", 3)); /* table of 8 rules is full */
  ctl("RESET", r, sizeof(r));
  ctl("GET", g, sizeof(g));
  CHECK(!strstr(g, "SLICE id=") && !strstr(g, "RULE ") && strstr(g, "policy=pf"));

  /* --- slice shares follow the slice weights, 2 + 2 UEs */
  slice_policy_on();
  ctl("SLICE 1 WEIGHT 3000", r, sizeof(r));
  ctl("SLICE 2 WEIGHT 1000", r, sizeof(r));
  ctl("ASSOC 1001 1", r, sizeof(r));
  ctl("ASSOC 1002 1", r, sizeof(r));
  ctl("ASSOC 1003 2", r, sizeof(r));
  ctl("ASSOC 1004 2", r, sizeof(r));
  memset(got, 0, sizeof(got));
  slice_run(4, NULL, 4000, got);
  double tot = got[0] + got[1] + got[2] + got[3];
  CHECK(tot == 4000.0 * NSLOT_RB);
  CHECK(fabs((got[0] + got[1]) / tot - 0.75) < 0.01);
  CHECK(fabs(got[0] - got[1]) / (got[0] + got[1]) < 0.02 && fabs(got[2] - got[3]) / (got[2] + got[3]) < 0.02);
  ctl("GET", g, sizeof(g));
  CHECK(slice_field(g, 1, "granted_rbs=") == (long)(got[0] + got[1]) && slice_field(g, 2, "granted_rbs=") == (long)(got[2] + got[3]));

  /* --- the slice share does not depend on the number of UEs in the slice (3 + 1) ... */
  slice_policy_on();
  ctl("SLICE 1 WEIGHT 3000", r, sizeof(r));
  ctl("SLICE 2 WEIGHT 1000", r, sizeof(r));
  ctl("ASSOC 1001 1", r, sizeof(r));
  ctl("ASSOC 1002 1", r, sizeof(r));
  ctl("ASSOC 1003 1", r, sizeof(r));
  ctl("ASSOC 1004 2", r, sizeof(r));
  memset(got, 0, sizeof(got));
  slice_run(4, NULL, 4000, got);
  tot = got[0] + got[1] + got[2] + got[3];
  CHECK(fabs((got[0] + got[1] + got[2]) / tot - 0.75) < 0.01);
  CHECK(fabs(got[3] / tot - 0.25) < 0.01);
  /* ... nor on how many of them have data: two of the three stop */
  int skip[4] = {0, 1, 1, 0};
  memset(got, 0, sizeof(got));
  slice_run(4, skip, 3000, got);
  tot = got[0] + got[3];
  CHECK(got[1] == 0.0 && got[2] == 0.0);
  CHECK(fabs(got[0] / tot - 0.75) < 0.01);

  /* --- inside a slice the UE weights decide */
  slice_policy_on();
  ctl("ASSOC 1001 1", r, sizeof(r));
  ctl("ASSOC 1002 1", r, sizeof(r));
  ctl("ASSOC 1003 2", r, sizeof(r));
  ctl("ASSOC 1004 2", r, sizeof(r));
  ctl("WEIGHT 1001 3000", r, sizeof(r));
  ctl("WEIGHT 1002 1000", r, sizeof(r));
  memset(got, 0, sizeof(got));
  slice_run(4, NULL, 4000, got);
  tot = got[0] + got[1] + got[2] + got[3];
  CHECK(fabs((got[0] + got[1]) / tot - 0.5) < 0.01);
  CHECK(fabs(got[0] / got[1] - 3.0) < 0.1);
  CHECK(fabs(got[2] / got[3] - 1.0) < 0.05);

  /* --- one UE per slot, as in wrr, and retransmissions are served first and not charged */
  sim_setup(2);
  slice_policy_on();
  sim_slot_prepare(2, 1000000);
  cands[0].is_retx = true;
  cands[0].retx_rbSize = 10;
  cands[0].pending_bytes = 0;
  CHECK(sim_slot(2, 4) == 2);
  CHECK(cands[0].sched_pdsch.rbStart == 0 && cands[0].sched_pdsch.rbSize == 10);
  CHECK(cands[1].sched_pdsch.rbSize == NSLOT_RB - 10);
  ctl("GET", g, sizeof(g));
  CHECK(slice_field(g, 0, "granted_rbs=") == NSLOT_RB - 10); /* only the new data is charged */

  /* --- a slice that was idle does not take every slot when it comes back */
  slice_policy_on();
  ctl("ASSOC 1001 1", r, sizeof(r));
  ctl("ASSOC 1002 2", r, sizeof(r));
  int idle2[2] = {0, 1};
  memset(got, 0, sizeof(got));
  slice_run(2, idle2, 3000, got);
  CHECK(got[0] == 3000.0 * NSLOT_RB && got[1] == 0.0);
  memset(got, 0, sizeof(got));
  slice_run(2, NULL, 300, got);
  CHECK(fabs(got[1] / (got[0] + got[1]) - 0.5) < 0.05);
  memset(got, 0, sizeof(got));
  slice_run(2, NULL, 2000, got);
  CHECK(fabs(got[1] / (got[0] + got[1]) - 0.5) < 0.01);
  /* --- credit is bounded: a slice with data in only one call out of 20 is never idle for long, but the
   * credit it collects stays below NR_CTRL_SLICE_CREDIT_RBS, so it cannot take every slot afterwards */
  slice_policy_on();
  ctl("ASSOC 1001 1", r, sizeof(r));
  ctl("ASSOC 1002 2", r, sizeof(r));
  int quiet1[2] = {1, 0}; /* the UE of slice 1 has no data */
  memset(got, 0, sizeof(got));
  for (int k = 0; k < 400; k++) {
    slice_run(2, quiet1, 19, got);
    slice_run(2, NULL, 1, got);
  }
  memset(got, 0, sizeof(got));
  slice_run(2, NULL, 2000, got);
  CHECK(got[0] - got[1] > 0.0); /* it does get its burst ... */
  CHECK(got[0] - got[1] <= NR_CTRL_SLICE_CREDIT_RBS + 2.0 * NSLOT_RB); /* ... of the size of the credit and not more */
  memset(got, 0, sizeof(got));
  slice_run(2, NULL, 2000, got);
  CHECK(fabs(got[0] / (got[0] + got[1]) - 0.5) < 0.01);
  /* the same with unequal weights: the burst is measured in the slice's own grants */
  slice_policy_on();
  ctl("SLICE 1 WEIGHT 3000", r, sizeof(r));
  ctl("ASSOC 1001 1", r, sizeof(r));
  ctl("ASSOC 1002 2", r, sizeof(r));
  memset(got, 0, sizeof(got));
  for (int k = 0; k < 400; k++) {
    slice_run(2, quiet1, 19, got);
    slice_run(2, NULL, 1, got);
  }
  memset(got, 0, sizeof(got));
  slice_run(2, NULL, 4000, got);
  CHECK(fabs(got[0] / (got[0] + got[1]) - 0.75) < 0.02);
  /* a gap shorter than the credit costs nothing: the slice is served first until it has caught up */
  slice_policy_on();
  ctl("ASSOC 1001 1", r, sizeof(r));
  ctl("ASSOC 1002 2", r, sizeof(r));
  memset(got, 0, sizeof(got));
  slice_run(2, NULL, 200, got);
  memset(got, 0, sizeof(got));
  slice_run(2, idle2, 10, got); /* slice 2 sends nothing for 10 calls (about half the credit) */
  CHECK(got[0] == 10.0 * NSLOT_RB);
  memset(got, 0, sizeof(got));
  slice_run(2, NULL, 40, got);
  CHECK(got[1] - got[0] >= 10.0 * NSLOT_RB - 2.0 * NSLOT_RB); /* it catches up what it missed */

  /* --- the slice cap limits what all UEs of the slice take together in one call */
  sim_setup(3);
  memset(slice_nssai, 0, sizeof(slice_nssai));
  slice_policy_on();
  ctl("ASSOC 1001 1", r, sizeof(r));
  ctl("ASSOC 1002 1", r, sizeof(r));
  ctl("ASSOC 1003 2", r, sizeof(r));
  ctl("SLICE 1 CAP 30", r, sizeof(r));
  sim_slot_prepare(3, 1000000);
  CHECK(sim_slot(3, 4) == 2);
  CHECK(cands[0].scheduled && cands[0].sched_pdsch.rbSize == 30);
  CHECK(!cands[1].scheduled); /* nothing of the cap is left for the second UE of the slice */
  CHECK(cands[2].scheduled && cands[2].sched_pdsch.rbSize == NSLOT_RB - 30);
  memset(got, 0, sizeof(got));
  slice_run(3, NULL, 2000, got);
  CHECK(got[0] + got[1] == 30.0 * 2000 && got[2] == (NSLOT_RB - 30.0) * 2000);
  CHECK(fabs(got[0] - got[1]) / (got[0] + got[1]) < 0.02); /* the two UEs take turns */
  ctl("GET", g, sizeof(g));
  CHECK(slice_field(g, 1, "cap=") == 30);
  /* the UE cap works inside the slice cap (each case starts from clean counters, so UE 1001 leads) */
  cap_case(30, 20);
  sim_slot_prepare(3, 1000000);
  CHECK(sim_slot(3, 4) == 3);
  CHECK(cands[0].sched_pdsch.rbSize == 20 && cands[1].sched_pdsch.rbSize == 10 && cands[2].sched_pdsch.rbSize == NSLOT_RB - 30);
  /* a remainder below the minimum grant is not used */
  cap_case(30, 28);
  sim_slot_prepare(3, 1000000);
  CHECK(sim_slot(3, 4) == 2);
  CHECK(cands[0].sched_pdsch.rbSize == 28 && !cands[1].scheduled && cands[2].sched_pdsch.rbSize == NSLOT_RB - 28);
  /* a UE cap above the slice cap changes nothing */
  cap_case(30, 40);
  sim_slot_prepare(3, 1000000);
  CHECK(sim_slot(3, 4) == 2);
  CHECK(cands[0].sched_pdsch.rbSize == 30 && !cands[1].scheduled);
  /* without a slice cap the first UE of the slice takes the slot, as before */
  cap_case(0, 0);
  sim_slot_prepare(3, 1000000);
  CHECK(sim_slot(3, 4) == 1);
  CHECK(cands[0].sched_pdsch.rbSize == NSLOT_RB);

  /* --- membership: RNTI association, then S-NSSAI rule, then slice 0 */
  sim_setup(3);
  memset(slice_nssai, 0, sizeof(slice_nssai));
  slice_nssai[2] = (nssai_t){.sst = 2, .sd = 0xFFFFFF};
  slice_policy_on();
  memset(got, 0, sizeof(got));
  slice_run(3, NULL, 1, got); /* lets the policy see the S-NSSAI of the candidates */
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1003", "slice=") == 0 && ue_source(g, "0x1003") == 0);
  CHECK(strstr(g, "nssai=2:ffffff"));
  ctl("NSSAIMAP 2 none 3", r, sizeof(r));
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1003", "slice=") == 3 && ue_source(g, "0x1003") == 2);
  CHECK(ue_field(g, "0x1001", "slice=") == 0 && ue_source(g, "0x1001") == 0);
  ctl("ASSOC 1003 5", r, sizeof(r));
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1003", "slice=") == 5 && ue_source(g, "0x1003") == 1);
  ctl("ASSOC 1003 -", r, sizeof(r));
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1003", "slice=") == 3 && ue_source(g, "0x1003") == 2);
  ctl("NSSAIMAP 2 123456 4", r, sizeof(r)); /* another sd: no match */
  ctl("NSSAIMAP 2 none -", r, sizeof(r));
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1003", "slice=") == 0 && ue_source(g, "0x1003") == 0);
  /* the rule decides the scheduling too: the NSSAI slice is served by its weight */
  ctl("NSSAIMAP 2 none 3", r, sizeof(r));
  ctl("SLICE 3 WEIGHT 3000", r, sizeof(r));
  memset(got, 0, sizeof(got));
  slice_run(3, NULL, 4000, got);
  tot = got[0] + got[1] + got[2];
  CHECK(fabs(got[2] / tot - 0.75) < 0.01); /* slice 3 (3000) against slice 0 (1000, two UEs): 3000 / 4000 */

  /* --- a UE that leaves loses its association, the rules stay */
  sim_setup(3);
  slice_policy_on();
  ctl("ASSOC 1003 4", r, sizeof(r));
  ctl("NSSAIMAP 7 none 1", r, sizeof(r));
  sim_setup(2);
  memset(got, 0, sizeof(got));
  slice_run(2, NULL, 600, got); /* the table is pruned every 256 calls */
  ctl("GET", g, sizeof(g));
  CHECK(!strstr(g, "rnti=0x1003 ") && strstr(g, "RULE sst=7 sd=ffffff slice=1"));
  sim_setup(3);
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1003", "slice=") == 0 && ue_source(g, "0x1003") == 0);
  sim_setup(2);
}

/* The slice commands in the two words of the E2 control message. */
static void test_slices_e2(void)
{
  char r[4096], g[8192], text[128];
  uint32_t hdr, act;
  const char *err = NULL;

  /* words built by the xApp, against values written out by hand */
  CHECK(nr_ctrl_e2_pack("policy", "slice", NULL, NULL, &hdr, &act, &err) == 0 && hdr == 0x53430001u && act == 4);
  CHECK(nr_ctrl_e2_pack("slice-weight", "3", "3000", NULL, &hdr, &act, &err) == 0 && hdr == 0x53430305u && act == 3000);
  CHECK(nr_ctrl_e2_pack("slice-cap", "2", "30", NULL, &hdr, &act, &err) == 0 && hdr == 0x53430206u && act == 30);
  CHECK(nr_ctrl_e2_pack("slice-cap", "2", "0", NULL, &hdr, &act, &err) == 0 && hdr == 0x53430206u && act == 0);
  CHECK(nr_ctrl_e2_pack("assoc", "1001", "1", NULL, &hdr, &act, &err) == 0 && hdr == 0x53430107u && act == 0x10010000u);
  CHECK(nr_ctrl_e2_pack("assoc", "1001", "none", NULL, &hdr, &act, &err) == 0 && hdr == 0x5343ff07u && act == 0x10010000u);
  CHECK(nr_ctrl_e2_pack("nssai-map", "2", "none", "3", &hdr, &act, &err) == 0 && hdr == 0x53430308u && act == 0x02ffffffu);
  CHECK(nr_ctrl_e2_pack("nssai-map", "1", "abcdef", "none", &hdr, &act, &err) == 0 && hdr == 0x5343ff08u && act == 0x01abcdefu);
  CHECK(nr_ctrl_e2_pack("nssai-map", "255", "0", "7", &hdr, &act, &err) == 0 && hdr == 0x53430708u && act == 0xff000000u);

  /* words the xApp refuses to build */
  const char *bad[][4] = {{"slice-weight", "8", "5", NULL}, {"slice-weight", "1", "0", NULL}, {"slice-weight", "1", "65536", NULL},
                          {"slice-weight", "x", "5", NULL}, {"slice-weight", NULL, NULL, NULL}, {"slice-cap", "1", "4", NULL},
                          {"slice-cap", "1", "276", NULL}, {"slice-cap", "9", "30", NULL}, {"assoc", "1001", "8", NULL},
                          {"assoc", "0", "1", NULL}, {"assoc", "1001", "x", NULL}, {"assoc", "1001", NULL, NULL},
                          {"nssai-map", "256", "none", "1"}, {"nssai-map", "1", "1000000", "1"}, {"nssai-map", "1", "zz", "1"},
                          {"nssai-map", "1", "none", "9"}, {"nssai-map", "1", "none", NULL}, {"nssai-map", NULL, NULL, NULL}};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    CHECK(nr_ctrl_e2_pack(bad[i][0], bad[i][1], bad[i][2], bad[i][3], &hdr, &act, &err) != 0);

  /* the text for each word pair, and the pairs the gNB refuses */
  CHECK(nr_ctrl_e2_to_text(0x53430001u, 4, text, sizeof(text)) == 0 && !strcmp(text, "POLICY slice"));
  CHECK(nr_ctrl_e2_to_text(0x53430305u, 3000, text, sizeof(text)) == 0 && !strcmp(text, "SLICE 3 WEIGHT 3000"));
  CHECK(nr_ctrl_e2_to_text(0x53430206u, 30, text, sizeof(text)) == 0 && !strcmp(text, "SLICE 2 CAP 30"));
  CHECK(nr_ctrl_e2_to_text(0x53430107u, 0x10010000u, text, sizeof(text)) == 0 && !strcmp(text, "ASSOC 1001 1"));
  CHECK(nr_ctrl_e2_to_text(0x5343ff07u, 0x10010000u, text, sizeof(text)) == 0 && !strcmp(text, "ASSOC 1001 -"));
  CHECK(nr_ctrl_e2_to_text(0x53430308u, 0x02ffffffu, text, sizeof(text)) == 0 && !strcmp(text, "NSSAIMAP 2 ffffff 3"));
  CHECK(nr_ctrl_e2_to_text(0x5343ff08u, 0x01abcdefu, text, sizeof(text)) == 0 && !strcmp(text, "NSSAIMAP 1 abcdef -"));
  CHECK(nr_ctrl_e2_to_text(0x53430101u, 1, text, sizeof(text)) != 0);        /* old opcode with an argument */
  CHECK(nr_ctrl_e2_to_text(0x53430102u, 0x10010bb8u, text, sizeof(text)) != 0);
  CHECK(nr_ctrl_e2_to_text(0x53430103u, 0x10010014u, text, sizeof(text)) != 0);
  CHECK(nr_ctrl_e2_to_text(0x53430104u, 0, text, sizeof(text)) != 0);
  CHECK(nr_ctrl_e2_to_text(0x5343000au, 0, text, sizeof(text)) != 0);        /* unknown opcode */
  CHECK(nr_ctrl_e2_to_text(0x53430805u, 5, text, sizeof(text)) != 0);        /* slice 8 */
  CHECK(nr_ctrl_e2_to_text(0x53430005u, 0, text, sizeof(text)) != 0);        /* weight 0 */
  CHECK(nr_ctrl_e2_to_text(0x53430105u, 0x10000u, text, sizeof(text)) != 0); /* weight above 65535 */
  CHECK(nr_ctrl_e2_to_text(0x53430106u, 4, text, sizeof(text)) != 0);        /* cap 4 */
  CHECK(nr_ctrl_e2_to_text(0x53430107u, 0, text, sizeof(text)) != 0);        /* no rnti */
  CHECK(nr_ctrl_e2_to_text(0x53430107u, 0x10010001u, text, sizeof(text)) != 0);
  CHECK(nr_ctrl_e2_to_text(0x53430a08u, 0, text, sizeof(text)) != 0);        /* slice 10 */
  CHECK(nr_ctrl_e2_to_text(0x53430107u, 0x10010000u, text, 6) != 0);         /* buffer too small */

  /* end to end against the live controller */
  sim_setup(2);
  memset(slice_nssai, 0, sizeof(slice_nssai));
  ctl("RESET", r, sizeof(r));
  nr_ctrl_e2_pack("policy", "slice", NULL, NULL, &hdr, &act, &err);
  CHECK(e2_deliver(hdr, act, r, sizeof(r)) == 0);
  nr_ctrl_e2_pack("slice-weight", "1", "3000", NULL, &hdr, &act, &err);
  CHECK(e2_deliver(hdr, act, r, sizeof(r)) == 0);
  nr_ctrl_e2_pack("slice-cap", "1", "30", NULL, &hdr, &act, &err);
  CHECK(e2_deliver(hdr, act, r, sizeof(r)) == 0);
  nr_ctrl_e2_pack("assoc", "1001", "1", NULL, &hdr, &act, &err);
  CHECK(e2_deliver(hdr, act, r, sizeof(r)) == 0);
  nr_ctrl_e2_pack("nssai-map", "2", "none", "3", &hdr, &act, &err);
  CHECK(e2_deliver(hdr, act, r, sizeof(r)) == 0);
  ctl("GET", g, sizeof(g));
  CHECK(strstr(g, "policy=slice"));
  CHECK(slice_field(g, 1, "weight=") == 3000 && slice_field(g, 1, "cap=") == 30);
  CHECK(ue_field(g, "0x1001", "slice=") == 1 && ue_source(g, "0x1001") == 1);
  CHECK(strstr(g, "RULE sst=2 sd=ffffff slice=3"));
  nr_ctrl_e2_pack("assoc", "1001", "none", NULL, &hdr, &act, &err);
  CHECK(e2_deliver(hdr, act, r, sizeof(r)) == 0);
  nr_ctrl_e2_pack("nssai-map", "2", "none", "none", &hdr, &act, &err);
  CHECK(e2_deliver(hdr, act, r, sizeof(r)) == 0);
  ctl("GET", g, sizeof(g));
  CHECK(ue_source(g, "0x1001") == 0 && !strstr(g, "RULE "));
  /* the controller's refusals reach the caller */
  nr_ctrl_e2_pack("assoc", "7777", "1", NULL, &hdr, &act, &err);
  CHECK(e2_deliver(hdr, act, r, sizeof(r)) != 0 && !strncmp(r, "ERR", 3));
  nr_ctrl_e2_pack("nssai-map", "9", "none", "none", &hdr, &act, &err);
  CHECK(e2_deliver(hdr, act, r, sizeof(r)) != 0 && !strncmp(r, "ERR", 3)); /* no such rule */
  ctl("RESET", r, sizeof(r));
}

static void test_second_instance(void)
{
  gNB_MAC_INST other;
  memset(&other, 0, sizeof(other));
  nr_dl_ctrl_policy_init(&other);
  CHECK(other.dl_rb_alloc == nr_dl_proportional_fair);
}

/* Many UEs: deficit ordering must rotate service. Uses the state API directly
 * through the policy with the first instance's state. */
static void test_many_ues(void)
{
  char r[4096];
  sim_setup(25);
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wshare", r, sizeof(r));
  /* the control thread enumerates the UEs of the first instance; point it at the new list */
  int slots = 2100;
  int served[25] = {0};
  for (int s = 0; s < slots; s++) {
    sim_slot_prepare(25, 1000000);
    sim_slot(25, 32);
    for (int i = 0; i < 25; i++)
      if (cands[i].scheduled)
        served[i]++;
  }
  int mn = slots, mx = 0;
  for (int i = 0; i < 25; i++) {
    if (served[i] < mn) mn = served[i];
    if (served[i] > mx) mx = served[i];
  }
  CHECK(mn > 0);
  CHECK(mx - mn <= 2 * slots / 25); /* roughly even: 21 of 25 admitted per slot */
}

/* Control commands racing the scheduler. */
static void *hammer(void *arg)
{
  (void)arg;
  char r[4096];
  const char *cmds[] = {"POLICY wshare", "POLICY rr", "POLICY pf", "WEIGHT 1001 5000", "WEIGHT 1002 700", "CAP 1001 30", "CAP 1001 0", "GET", "RESET",
                        "POLICY slice", "SLICE 1 WEIGHT 3000", "SLICE 2 CAP 30", "ASSOC 1001 1", "ASSOC 1002 -", "NSSAIMAP 1 none 2", "NSSAIMAP 1 none -"};
  for (int i = 0; i < 3000; i++)
    ctl(cmds[i % (int)(sizeof(cmds) / sizeof(cmds[0]))], r, sizeof(r));
  return NULL;
}

static void test_concurrency(void)
{
  sim_setup(2);
  /* reuse the same gNB object as the endpoint: restore pointers it holds */
  pthread_t th;
  pthread_create(&th, NULL, hammer, NULL);
  for (int s = 0; s < 200000; s++) {
    sim_slot_prepare(2, 1000000);
    int n = sim_slot(2, 4);
    CHECK(n >= 0 && n <= 2);
  }
  pthread_join(th, NULL);
}

/* A UE that joins late (or reconnects) enters at the level of its peers, less the credit bound, in
 * wrr and inside a slice: it does not take every slot until its counter has caught up. */
static void test_ue_credit(void)
{
  char r[4096];
  double got[40];
  const char *policy[] = {"POLICY wrr", "POLICY slice"};
  for (int m = 0; m < 2; m++) {
    sim_setup(3);
    memset(slice_nssai, 0, sizeof(slice_nssai));
    ctl("RESET", r, sizeof(r));
    ctl(policy[m], r, sizeof(r));
    int late[3] = {0, 0, 1}; /* the third UE has not attached yet */
    memset(got, 0, sizeof(got));
    slice_run(3, late, 3000, got);
    CHECK(got[2] == 0.0 && fabs(got[0] - got[1]) / (got[0] + got[1]) < 0.01);
    memset(got, 0, sizeof(got));
    slice_run(3, NULL, 600, got);
    double tot = got[0] + got[1] + got[2];
    CHECK(tot == 600.0 * NSLOT_RB);
    CHECK(got[2] > tot / 3.0); /* it does get a burst ... */
    CHECK(got[2] - tot / 3.0 <= NR_CTRL_UE_CREDIT_RBS + 2.0 * NSLOT_RB); /* ... of the size of the credit, not of its whole lag */
    memset(got, 0, sizeof(got));
    slice_run(3, NULL, 3000, got);
    tot = got[0] + got[1] + got[2];
    CHECK(fabs(got[2] / tot - 1.0 / 3.0) < 0.01 && fabs(got[0] / tot - 1.0 / 3.0) < 0.01);
  }

  /* weights: a late UE of weight 3000 against two of 1000 settles at 3/5 after its burst */
  sim_setup(3);
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wrr", r, sizeof(r));
  ctl("WEIGHT 1003 3000", r, sizeof(r));
  int late[3] = {0, 0, 1};
  memset(got, 0, sizeof(got));
  slice_run(3, late, 3000, got);
  memset(got, 0, sizeof(got));
  slice_run(3, NULL, 4000, got);
  double tot = got[0] + got[1] + got[2];
  CHECK(fabs(got[2] / tot - 0.6) < 0.02);

  /* within a slice only: UEs of other slices are not compared with it */
  sim_setup(3);
  memset(slice_nssai, 0, sizeof(slice_nssai));
  slice_policy_on();
  ctl("ASSOC 1001 1", r, sizeof(r));
  ctl("ASSOC 1002 2", r, sizeof(r));
  ctl("ASSOC 1003 2", r, sizeof(r));
  int late2[3] = {0, 0, 1}; /* UE 1003 joins slice 2 late */
  memset(got, 0, sizeof(got));
  slice_run(3, late2, 3000, got);
  memset(got, 0, sizeof(got));
  slice_run(3, NULL, 3000, got);
  tot = got[0] + got[1] + got[2];
  CHECK(fabs((got[1] + got[2]) / tot - 0.5) < 0.01); /* the slices stay at their weights */
  CHECK(fabs(got[1] / got[2] - 1.0) < 0.1);          /* and the two UEs of slice 2 split it */
  /* the counters in status are real grants: the credit is kept apart, so a slice equals the sum of its UEs */
  char g[8192];
  ctl("GET", g, sizeof(g));
  long s2 = slice_field(g, 2, "granted_rbs=");
  CHECK(s2 > 0 && s2 == ue_field(g, "0x1002", "granted_rbs=") + ue_field(g, "0x1003", "granted_rbs="));
}

/* ------------------------------------------- slices: UEs and sessions that change during a run */

/* The UEs offered to the policy, in this order, with the RNTIs given (a UE that is not in the list is not connected). */
static void sim_set_ues(const uint16_t *rnti, int n)
{
  NR_SCHED_LOCK(&mac.sched_lock);
  memset(ues, 0, sizeof(ues));
  memset(mac.UE_info.connected_ue_list, 0, sizeof(mac.UE_info.connected_ue_list));
  for (int i = 0; i < n; i++) {
    ues[i].rnti = rnti[i];
    mac.UE_info.connected_ue_list[i] = &ues[i];
  }
  mac.dl_rb_alloc = nr_dl_ctrl_policy;
  NR_SCHED_UNLOCK(&mac.sched_lock);
}

/* Resource blocks of new data granted to the UEs of `got[]` over `slots` saturated calls, with a limit on
 * the first `window` calls reported in early[] (to see a burst). */
static void dyn_run(int n_ue, const int *skip, int slots, double *got, int window, double *early)
{
  for (int s = 0; s < slots; s++) {
    sim_slot_prepare(n_ue, 1000000);
    for (int i = 0; i < n_ue; i++) {
      cands[i].nssai = slice_nssai[i];
      if (skip && skip[i])
        cands[i].skipped = true;
    }
    sim_slot(n_ue, 4);
    for (int i = 0; i < n_ue; i++)
      if (cands[i].scheduled && !cands[i].is_retx) {
        got[i] += cands[i].sched_pdsch.rbSize;
        if (early && s < window)
          early[i] += cands[i].sched_pdsch.rbSize;
      }
  }
}

static void test_slices_dynamic(void)
{
  char r[4096], g[8192];
  double got[40], early[40], tot;
  const nssai_t none = {.sst = 0, .sd = 0xFFFFFF};
  const nssai_t s1 = {.sst = 1, .sd = 0xFFFFFF};
  const nssai_t s2 = {.sst = 2, .sd = 0xFFFFFF};

  /* --- a UE that gets a PDU session during the run moves to the slice of its S-NSSAI, without a burst */
  sim_setup(4);
  slice_policy_on();
  ctl("NSSAIMAP 1 none 1", r, sizeof(r));
  ctl("NSSAIMAP 2 none 2", r, sizeof(r));
  slice_nssai[0] = s1;
  slice_nssai[1] = s2;
  slice_nssai[2] = none; /* no session yet: slice 0 */
  slice_nssai[3] = none;
  memset(got, 0, sizeof(got));
  dyn_run(4, NULL, 3000, got, 0, NULL);
  tot = got[0] + got[1] + got[2] + got[3];
  CHECK(fabs(got[0] / tot - 1.0 / 3.0) < 0.01 && fabs(got[1] / tot - 1.0 / 3.0) < 0.01); /* three slices of equal weight */
  CHECK(fabs((got[2] + got[3]) / tot - 1.0 / 3.0) < 0.01);
  slice_nssai[2] = s2; /* UE 1003 gets its session */
  memset(got, 0, sizeof(got));
  memset(early, 0, sizeof(early));
  dyn_run(4, NULL, 3000, got, 40, early);
  tot = got[0] + got[1] + got[2] + got[3];
  /* slices: 0 (UE 1004), 1 (UE 1001), 2 (UEs 1002 and 1003) */
  CHECK(fabs(got[0] / tot - 1.0 / 3.0) < 0.01 && fabs(got[3] / tot - 1.0 / 3.0) < 0.01);
  CHECK(fabs(got[1] / tot - 1.0 / 6.0) < 0.01 && fabs(got[2] / tot - 1.0 / 6.0) < 0.01);
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1003", "slice=") == 2 && ue_source(g, "0x1003") == 2);
  CHECK(early[2] <= 40.0 * NSLOT_RB / 3.0 + NR_CTRL_UE_CREDIT_RBS + NSLOT_RB); /* no burst at the move */

  /* --- the same UE changes to another S-NSSAI later: it follows, and a rule edit moves the UEs too */
  slice_nssai[2] = s1;
  ctl("GET", g, sizeof(g));
  memset(got, 0, sizeof(got));
  dyn_run(4, NULL, 1, got, 0, NULL);
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1003", "slice=") == 1 && ue_source(g, "0x1003") == 2);
  ctl("NSSAIMAP 1 none 6", r, sizeof(r)); /* the rule for S-NSSAI 1 now points to slice 6 */
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1001", "slice=") == 6 && ue_field(g, "0x1003", "slice=") == 6 && ue_field(g, "0x1002", "slice=") == 2);
  ctl("NSSAIMAP 1 none -", r, sizeof(r)); /* rule deleted: both UEs fall back to slice 0 */
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1001", "slice=") == 0 && ue_source(g, "0x1001") == 0 && ue_field(g, "0x1003", "slice=") == 0);

  /* --- S-NSSAIs that differ only in the slice differentiator are different rules */
  sim_setup(3);
  slice_policy_on();
  ctl("NSSAIMAP 1 none 1", r, sizeof(r));
  ctl("NSSAIMAP 1 000005 2", r, sizeof(r));
  ctl("NSSAIMAP 1 0000FF 3", r, sizeof(r));
  slice_nssai[0] = s1;
  slice_nssai[1] = (nssai_t){.sst = 1, .sd = 0x000005};
  slice_nssai[2] = (nssai_t){.sst = 1, .sd = 0x0000FF};
  memset(got, 0, sizeof(got));
  dyn_run(3, NULL, 3000, got, 0, NULL);
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1001", "slice=") == 1 && ue_field(g, "0x1002", "slice=") == 2 && ue_field(g, "0x1003", "slice=") == 3);
  ctl("NSSAIMAP 1 000005 7", r, sizeof(r)); /* a rule is replaced, not added */
  ctl("GET", g, sizeof(g));
  CHECK(count_of(g, "RULE ") == 3 && ue_field(g, "0x1002", "slice=") == 7);
  ctl("NSSAIMAP 1 none -", r, sizeof(r)); /* deleting the first rule keeps the others */
  ctl("GET", g, sizeof(g));
  CHECK(count_of(g, "RULE ") == 2 && ue_field(g, "0x1001", "slice=") == 0 && ue_field(g, "0x1002", "slice=") == 7 &&
        ue_field(g, "0x1003", "slice=") == 3);

  /* --- the table holds eight rules; a ninth is refused and nothing changes */
  slice_policy_on();
  for (int i = 0; i < NR_CTRL_MAX_RULE; i++) {
    char c[48];
    snprintf(c, sizeof(c), "NSSAIMAP %d none %d", 10 + i, i % 8);
    ctl(c, r, sizeof(r));
    CHECK(!strncmp(r, "OK", 2));
  }
  ctl("NSSAIMAP 99 none 1", r, sizeof(r));
  CHECK(!strncmp(r, "ERR", 3) && strstr(r, "table_full"));
  ctl("NSSAIMAP 10 none 5", r, sizeof(r)); /* an existing key can still be changed */
  CHECK(!strncmp(r, "OK", 2));
  ctl("GET", g, sizeof(g));
  CHECK(count_of(g, "RULE ") == NR_CTRL_MAX_RULE);

  /* --- a UE that connects during the run: it is placed by its rule at once and gets no burst */
  sim_setup(3);
  slice_policy_on();
  ctl("NSSAIMAP 1 none 1", r, sizeof(r));
  slice_nssai[0] = s1;
  slice_nssai[1] = s1;
  slice_nssai[2] = none; /* slice 0 */
  memset(got, 0, sizeof(got));
  dyn_run(3, NULL, 3000, got, 0, NULL);
  tot = got[0] + got[1] + got[2];
  CHECK(fabs(got[2] / tot - 0.5) < 0.01 && fabs(got[0] / tot - 0.25) < 0.01);
  sim_setup(4); /* UE 1004 connects with S-NSSAI 1 */
  slice_nssai[3] = s1;
  memset(got, 0, sizeof(got));
  memset(early, 0, sizeof(early));
  dyn_run(4, NULL, 3000, got, 40, early);
  tot = got[0] + got[1] + got[2] + got[3];
  CHECK(fabs(got[2] / tot - 0.5) < 0.01); /* slice 0 keeps its half */
  CHECK(fabs(got[0] / tot - 1.0 / 6.0) < 0.01 && fabs(got[3] / tot - 1.0 / 6.0) < 0.01);
  CHECK(early[3] <= 40.0 * NSLOT_RB / 6.0 + NR_CTRL_UE_CREDIT_RBS + NSLOT_RB);
  ctl("GET", g, sizeof(g));
  CHECK(ue_field(g, "0x1004", "slice=") == 1 && ue_source(g, "0x1004") == 2);

  /* --- a UE that leaves loses its association, its weight and its cap; it comes back as a new UE */
  ctl("ASSOC 1004 5", r, sizeof(r));
  ctl("WEIGHT 1004 3000", r, sizeof(r));
  ctl("CAP 1004 20", r, sizeof(r));
  {
    uint16_t three[3] = {0x1001, 0x1002, 0x1003};
    sim_set_ues(three, 3);
    memset(got, 0, sizeof(got));
    dyn_run(3, NULL, 600, got, 0, NULL); /* more than the 256 calls between two prunes */
    uint16_t four[4] = {0x1001, 0x1002, 0x1003, 0x1004};
    sim_set_ues(four, 4);
    ctl("GET", g, sizeof(g));
    CHECK(ue_field(g, "0x1004", "slice=") == 0 && ue_source(g, "0x1004") == 0);
    CHECK(ue_field(g, "0x1004", "weight=") == NR_CTRL_DEFAULT_WEIGHT && ue_field(g, "0x1004", "cap=") == 0);
    CHECK(ue_field(g, "0x1004", "granted_rbs=") == 0);
  }

  /* --- random churn against a model of the membership: UEs come and go, sessions and rules change.
   * Checked in every call: the grants fit the slot, the number of UEs and the slice cap; at the end the
   * resource blocks of every slice equal what the model attributes to it. */
  {
    enum { N = 6, KEYS = 4 };
    const nssai_t keys[KEYS] = {{.sst = 0, .sd = 0xFFFFFF}, {.sst = 1, .sd = 0xFFFFFF}, {.sst = 2, .sd = 0xFFFFFF}, {.sst = 1, .sd = 5}};
    uint16_t id[N];
    int present[N], absent_for[N], data[N], key_of[N], assoc[N];
    int rule[KEYS], scap[NR_CTRL_MAX_SLICE];
    double tally[NR_CTRL_MAX_SLICE];
    uint32_t rng = 12345;
#define RND(m) ((int)((rng = rng * 1664525u + 1013904223u) >> 8) % (m))
    for (int i = 0; i < N; i++) {
      id[i] = (uint16_t)(0x2001 + i);
      present[i] = i < 4;
      absent_for[i] = 0;
      data[i] = 1;
      key_of[i] = 1 + i % 3;
      assoc[i] = -1;
    }
    for (int k = 0; k < KEYS; k++)
      rule[k] = -1;
    for (int k = 0; k < NR_CTRL_MAX_SLICE; k++) {
      scap[k] = 0;
      tally[k] = 0;
    }
    slice_policy_on();
    int bad_fit = 0, bad_cap = 0, bad_slice = 0, bad_cmd = 0;
    for (int slot = 0; slot < 30000; slot++) {
      char c[64];
      int x = RND(1000);
      int u = RND(N), k = RND(KEYS), sl = RND(NR_CTRL_MAX_SLICE);
      if (x < 20) {
        data[u] = !data[u];
      } else if (x < 30 && present[u]) {
        key_of[u] = k; /* the UE's first bearer now has another S-NSSAI */
      } else if (x < 36) {
        if (rule[k] >= 0 && RND(3) == 0) {
          snprintf(c, sizeof(c), "NSSAIMAP %d %s -", keys[k].sst, keys[k].sd == 0xFFFFFF ? "none" : "000005");
          rule[k] = -1;
        } else {
          snprintf(c, sizeof(c), "NSSAIMAP %d %s %d", keys[k].sst, keys[k].sd == 0xFFFFFF ? "none" : "000005", sl);
          rule[k] = sl;
        }
        bad_cmd += (ctl(c, r, sizeof(r)) < 0 || strncmp(r, "OK", 2) != 0);
      } else if (x < 42 && present[u]) {
        if (RND(3) == 0) {
          snprintf(c, sizeof(c), "ASSOC %x -", id[u]);
          assoc[u] = -1;
        } else {
          snprintf(c, sizeof(c), "ASSOC %x %d", id[u], sl);
          assoc[u] = sl;
        }
        bad_cmd += (ctl(c, r, sizeof(r)) < 0 || strncmp(r, "OK", 2) != 0);
      } else if (x < 46) {
        static const int caps[4] = {0, 20, 30, 40};
        int cp = caps[RND(4)];
        snprintf(c, sizeof(c), "SLICE %d CAP %d", sl, cp);
        scap[sl] = cp;
        bad_cmd += (ctl(c, r, sizeof(r)) < 0 || strncmp(r, "OK", 2) != 0);
      } else if (x < 50) {
        snprintf(c, sizeof(c), "SLICE %d WEIGHT %d", sl, 1000 * (1 + RND(3)));
        bad_cmd += (ctl(c, r, sizeof(r)) < 0 || strncmp(r, "OK", 2) != 0);
      } else if (x < 52) {
        if (present[u]) { /* the UE leaves: its state is dropped at the next prune */
          present[u] = 0;
          absent_for[u] = 300 + RND(100);
          assoc[u] = -1;
        }
      }
      for (int i = 0; i < N; i++)
        if (!present[i] && absent_for[i] > 0 && --absent_for[i] == 0)
          present[i] = 1; /* it comes back as a new UE after more than one prune period */

      uint16_t rn[N];
      int idx[N], n = 0;
      for (int i = 0; i < N; i++)
        if (present[i]) {
          rn[n] = id[i];
          idx[n++] = i;
        }
      sim_set_ues(rn, n);
      sim_slot_prepare(n, 1000000);
      int model_slice[N];
      for (int j = 0; j < n; j++) {
        int i = idx[j];
        cands[j].nssai = keys[key_of[i]];
        if (!data[i])
          cands[j].skipped = true;
        model_slice[j] = assoc[i] >= 0 ? assoc[i] : (rule[key_of[i]] >= 0 ? rule[key_of[i]] : 0);
      }
      sim_slot(n, 4);
      int used = 0, nsched = 0;
      double per_slice[NR_CTRL_MAX_SLICE] = {0};
      for (int j = 0; j < n; j++)
        if (cands[j].scheduled && !cands[j].is_retx) {
          int rb = cands[j].sched_pdsch.rbSize;
          used += rb;
          nsched++;
          per_slice[model_slice[j]] += rb;
          tally[model_slice[j]] += rb;
          if (!data[idx[j]])
            bad_slice++; /* a UE without data was granted */
        }
      bad_fit += (used > NSLOT_RB || nsched > 4);
      for (int q = 0; q < NR_CTRL_MAX_SLICE; q++)
        bad_cap += (scap[q] > 0 && per_slice[q] > scap[q]);
    }
    CHECK(bad_cmd == 0);
    CHECK(bad_fit == 0);
    CHECK(bad_cap == 0);
    CHECK(bad_slice == 0);
    ctl("GET", g, sizeof(g));
    for (int q = 0; q < NR_CTRL_MAX_SLICE; q++) {
      long v = slice_field(g, q, "granted_rbs=");
      CHECK((v < 0 ? 0.0 : (double)v) == tally[q]); /* the policy and the model agree on every slice */
    }
#undef RND
  }
  sim_setup(2);
}

/* ------------------------------------------------- standard slice messages */

static int has(const char *r, const char *needle)
{
  return strstr(r, needle) != NULL;
}

static int send_plan(const nr_ctrl_sm_plan_t *plan, int *refused, char *reply, size_t size)
{
  return nr_ctrl_sm_apply(plan, (nr_ctrl_sm_send_fn)(void *)0, NULL, reply, size, refused);
}

static int e2_send_cb(const char *text, char *reply, size_t size, void *ctx)
{
  (void)ctx;
  return nr_ctrl_e2_send(text, reply, size);
}

static int apply_plan(const nr_ctrl_sm_plan_t *plan, int *refused, char *reply, size_t size)
{
  return nr_ctrl_sm_apply(plan, e2_send_cb, NULL, reply, size, refused);
}

static void test_slice_del_command(void)
{
  char r[4096], g[8192], text[64];
  uint32_t hdr, act;
  const char *err = NULL;
  (void)send_plan;

  sim_setup(4);
  slice_policy_on();
  ctl("SLICE 3 WEIGHT 4000", r, sizeof(r));
  ctl("SLICE 3 CAP 20", r, sizeof(r));
  ctl("SLICE 2 WEIGHT 2000", r, sizeof(r));
  ctl("ASSOC 1001 3", r, sizeof(r));
  ctl("ASSOC 1002 2", r, sizeof(r));
  ctl("NSSAIMAP 1 none 3", r, sizeof(r));
  ctl("NSSAIMAP 2 none 2", r, sizeof(r));
  ctl("GET", g, sizeof(g));
  CHECK(has(g, "SLICE id=3 weight=4000 cap=20") && has(g, "RULE sst=1 sd=ffffff slice=3"));
  long v0 = get_field(g, "version=");

  /* slice 3 back to its defaults; the other slice, its UE and its rule stay */
  ctl("SLICEDEL 3", r, sizeof(r));
  CHECK(!strncmp(r, "OK", 2));
  ctl("GET", g, sizeof(g));
  CHECK(get_field(g, "version=") == v0 + 1);
  CHECK(!has(g, "SLICE id=3 "));
  CHECK(!has(g, "slice=3"));                                   /* no UE and no rule points to slice 3 */
  CHECK(has(g, "SLICE id=2 weight=2000") && has(g, "RULE sst=2 sd=ffffff slice=2"));
  CHECK(has(g, "rnti=0x1002") && has(g, "slice=2 source=rnti"));
  CHECK(!has(g, "RULE sst=1"));
  CHECK(has(g, "rnti=0x1001") && has(g, "slice=0 source=default"));

  /* counters are cleared with the slice, and deleting an empty slice is allowed */
  ctl("SLICE 4 WEIGHT 500", r, sizeof(r));
  for (int i = 0; i < 50; i++)
    (sim_slot_prepare(4, 1000000), sim_slot(4, 4));
  ctl("SLICEDEL 4", r, sizeof(r));
  CHECK(!strncmp(r, "OK", 2));
  ctl("SLICEDEL 4", r, sizeof(r));
  CHECK(!strncmp(r, "OK", 2));
  ctl("SLICEDEL 0", r, sizeof(r)); /* slice 0: the default weight and no cap, the UEs stay in it */
  CHECK(!strncmp(r, "OK", 2));

  /* refusals change nothing */
  ctl("GET", g, sizeof(g));
  long v1 = get_field(g, "version=");
  const char *bad[] = {"SLICEDEL 8", "SLICEDEL x", "SLICEDEL -1", "SLICEDEL", "SLICEDEL 1 2", "SLICEDEL 99999999999"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    ctl(bad[i], r, sizeof(r));
    CHECK(!strncmp(r, "ERR", 3));
  }
  ctl("GET", g, sizeof(g));
  CHECK(get_field(g, "version=") == v1);

  /* the E2 word and the text of the command */
  CHECK(nr_ctrl_e2_pack("slice-del", "3", NULL, NULL, &hdr, &act, &err) == 0 && hdr == 0x53430309u && act == 0);
  CHECK(nr_ctrl_e2_pack("slice-del", "0", NULL, NULL, &hdr, &act, &err) == 0 && hdr == 0x53430009u);
  CHECK(nr_ctrl_e2_pack("slice-del", "8", NULL, NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_pack("slice-del", NULL, NULL, NULL, &hdr, &act, &err) != 0);
  CHECK(nr_ctrl_e2_to_text(0x53430309u, 0, text, sizeof(text)) == 0 && !strcmp(text, "SLICEDEL 3"));
  CHECK(nr_ctrl_e2_to_text(0x53430809u, 0, text, sizeof(text)) != 0); /* slice 8 */
  CHECK(nr_ctrl_e2_to_text(0x5343ff09u, 0, text, sizeof(text)) != 0);
  CHECK(nr_ctrl_e2_to_text(0x53430309u, 1, text, sizeof(text)) != 0); /* the message word must be 0 */
}

static void test_slice_sm_plan(void)
{
  nr_ctrl_sm_plan_t plan;
  char err[128];
  nr_ctrl_sm_slice_t s[9];
  memset(s, 0, sizeof(s));

  /* NVS capacity: the share becomes the weight, the cap is cleared */
  s[0] = (nr_ctrl_sm_slice_t){.id = 1, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = 0.7f};
  s[1] = (nr_ctrl_sm_slice_t){.id = 5, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = 1.0f};
  CHECK(nr_ctrl_sm_plan_add(s, 2, 0, &plan, err, sizeof(err)) == 0 && plan.n == 4);
  CHECK(!strcmp(plan.text[0], "SLICE 1 WEIGHT 700") && !strcmp(plan.text[1], "SLICE 1 CAP 0"));
  CHECK(!strcmp(plan.text[2], "SLICE 5 WEIGHT 1000") && !strcmp(plan.text[3], "SLICE 5 CAP 0"));
  s[0].pct_reserved = 0.0004f; /* below one thousandth: weight 1, never 0 */
  CHECK(nr_ctrl_sm_plan_add(s, 1, 0, &plan, err, sizeof(err)) == 0 && !strcmp(plan.text[0], "SLICE 1 WEIGHT 1"));

  /* STATIC: the width is the cap, the weight is the default */
  s[0] = (nr_ctrl_sm_slice_t){.id = 2, .alg = NR_CTRL_SM_ALG_STATIC, .pos_low = 8, .pos_high = 12};
  CHECK(nr_ctrl_sm_plan_add(s, 1, 0, &plan, err, sizeof(err)) == 0 && plan.n == 2);
  CHECK(!strcmp(plan.text[0], "SLICE 2 WEIGHT 1000") && !strcmp(plan.text[1], "SLICE 2 CAP 5"));
  s[0].pos_low = 0;
  s[0].pos_high = 274;
  CHECK(nr_ctrl_sm_plan_add(s, 1, 0, &plan, err, sizeof(err)) == 0 && !strcmp(plan.text[1], "SLICE 2 CAP 275"));

  /* refusals leave an empty plan, with a reason */
  struct { nr_ctrl_sm_slice_t x; const char *why; } bad[] = {
    {{.id = 1, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = 0.0f}, "pct"},
    {{.id = 1, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = 1.01f}, "pct"},
    {{.id = 1, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = -0.5f}, "pct"},
    {{.id = 1, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = 70.0f}, "pct"},
    {{.id = 1, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = __builtin_nanf("")}, "pct"},
    {{.id = 8, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = 0.5f}, "outside"},
    {{.id = 0xffffffffu, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = 0.5f}, "outside"},
    {{.id = 1, .alg = NR_CTRL_SM_ALG_STATIC, .pos_low = 0, .pos_high = 3}, "width"},   /* 4 resource blocks */
    {{.id = 1, .alg = NR_CTRL_SM_ALG_STATIC, .pos_low = 0, .pos_high = 275}, "width"}, /* 276 */
    {{.id = 1, .alg = NR_CTRL_SM_ALG_STATIC, .pos_low = 9, .pos_high = 3}, "below"},
    {{.id = 1, .alg = NR_CTRL_SM_ALG_STATIC, .pos_low = 0xffffffffu, .pos_high = 0}, "below"},
    {{.id = 1, .alg = NR_CTRL_SM_ALG_STATIC, .pos_low = 0, .pos_high = 0xffffffffu}, "width"},
    {{.id = 1, .alg = NR_CTRL_SM_ALG_UNSUPPORTED}, "supported"},
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    s[0] = bad[i].x;
    err[0] = 0;
    CHECK(nr_ctrl_sm_plan_add(s, 1, 0, &plan, err, sizeof(err)) != 0 && plan.n == 0 && has(err, bad[i].why));
  }
  /* one bad slice among good ones: nothing is planned */
  s[0] = (nr_ctrl_sm_slice_t){.id = 1, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = 0.5f};
  s[1] = (nr_ctrl_sm_slice_t){.id = 2, .alg = NR_CTRL_SM_ALG_UNSUPPORTED};
  CHECK(nr_ctrl_sm_plan_add(s, 2, 0, &plan, err, sizeof(err)) != 0 && plan.n == 0);
  /* the same id twice, uplink slices, too many slices */
  s[1] = s[0];
  CHECK(nr_ctrl_sm_plan_add(s, 2, 0, &plan, err, sizeof(err)) != 0 && has(err, "twice"));
  CHECK(nr_ctrl_sm_plan_add(s, 1, 1, &plan, err, sizeof(err)) != 0 && has(err, "uplink"));
  for (int i = 0; i < 9; i++)
    s[i] = (nr_ctrl_sm_slice_t){.id = (uint32_t)(i % 8), .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = 0.1f};
  CHECK(nr_ctrl_sm_plan_add(s, 9, 0, &plan, err, sizeof(err)) != 0 && has(err, "more than 8"));
  for (int i = 0; i < 8; i++)
    s[i].id = (uint32_t)i;
  CHECK(nr_ctrl_sm_plan_add(s, 8, 0, &plan, err, sizeof(err)) == 0 && plan.n == 16);

  /* no slice in the message: all slices deleted */
  CHECK(nr_ctrl_sm_plan_add(s, 0, 0, &plan, err, sizeof(err)) == 0 && plan.n == 8);
  CHECK(!strcmp(plan.text[0], "SLICEDEL 0") && !strcmp(plan.text[7], "SLICEDEL 7"));

  /* the example of the stock slice xApp (static slices of 4, 4 and 5 resource blocks) is refused: below the minimum grant */
  s[0] = (nr_ctrl_sm_slice_t){.id = 0, .alg = NR_CTRL_SM_ALG_STATIC, .pos_low = 0, .pos_high = 3};
  s[1] = (nr_ctrl_sm_slice_t){.id = 2, .alg = NR_CTRL_SM_ALG_STATIC, .pos_low = 4, .pos_high = 7};
  s[2] = (nr_ctrl_sm_slice_t){.id = 5, .alg = NR_CTRL_SM_ALG_STATIC, .pos_low = 8, .pos_high = 12};
  CHECK(nr_ctrl_sm_plan_add(s, 3, 0, &plan, err, sizeof(err)) != 0 && plan.n == 0);

  /* delete and association */
  uint32_t ids[9] = {2, 5, 0, 1, 1, 1, 1, 1, 1};
  CHECK(nr_ctrl_sm_plan_del(ids, 2, 0, &plan, err, sizeof(err)) == 0 && plan.n == 2 && !strcmp(plan.text[0], "SLICEDEL 2") && !strcmp(plan.text[1], "SLICEDEL 5"));
  CHECK(nr_ctrl_sm_plan_del(ids, 0, 0, &plan, err, sizeof(err)) != 0);
  CHECK(nr_ctrl_sm_plan_del(ids, 2, 1, &plan, err, sizeof(err)) != 0 && has(err, "uplink"));
  CHECK(nr_ctrl_sm_plan_del(ids, 9, 0, &plan, err, sizeof(err)) != 0 && has(err, "more than 8"));
  ids[1] = 8;
  CHECK(nr_ctrl_sm_plan_del(ids, 2, 0, &plan, err, sizeof(err)) != 0 && plan.n == 0 && has(err, "outside"));

  nr_ctrl_sm_assoc_t a[33];
  memset(a, 0, sizeof(a));
  a[0] = (nr_ctrl_sm_assoc_t){.rnti = 0x1001, .dl_id = 1};
  a[1] = (nr_ctrl_sm_assoc_t){.rnti = 0xabcd, .dl_id = 7};
  CHECK(nr_ctrl_sm_plan_assoc(a, 2, &plan, err, sizeof(err)) == 0 && plan.n == 2 && !strcmp(plan.text[0], "ASSOC 1001 1") && !strcmp(plan.text[1], "ASSOC abcd 7"));
  CHECK(nr_ctrl_sm_plan_assoc(a, 0, &plan, err, sizeof(err)) != 0);
  a[1].rnti = 0;
  CHECK(nr_ctrl_sm_plan_assoc(a, 2, &plan, err, sizeof(err)) != 0 && plan.n == 0 && has(err, "RNTI"));
  a[1].rnti = 0x10000;
  CHECK(nr_ctrl_sm_plan_assoc(a, 2, &plan, err, sizeof(err)) != 0 && has(err, "RNTI"));
  a[1].rnti = 0x1002;
  a[1].dl_id = 8;
  CHECK(nr_ctrl_sm_plan_assoc(a, 2, &plan, err, sizeof(err)) != 0 && has(err, "outside"));
  for (int i = 0; i < 33; i++)
    a[i] = (nr_ctrl_sm_assoc_t){.rnti = (uint32_t)(0x2000 + i), .dl_id = 1};
  CHECK(nr_ctrl_sm_plan_assoc(a, 32, &plan, err, sizeof(err)) == 0 && plan.n == 32);
  CHECK(nr_ctrl_sm_plan_assoc(a, 33, &plan, err, sizeof(err)) != 0 && has(err, "more than 32"));
  /* a short error buffer is filled and terminated */
  char tiny[8];
  CHECK(nr_ctrl_sm_plan_assoc(a, 0, &plan, tiny, sizeof(tiny)) != 0 && strlen(tiny) < sizeof(tiny));
}

static void test_slice_sm_parse(void)
{
  char g[8192];
  nr_ctrl_sm_view_t v;

  sim_setup(3);
  slice_policy_on();
  ctl("SLICE 1 WEIGHT 700", g, sizeof(g));
  ctl("SLICE 6 CAP 30", g, sizeof(g));
  ctl("ASSOC 1002 6", g, sizeof(g));
  ctl("GET", g, sizeof(g));
  CHECK(nr_ctrl_sm_parse_get(g, &v) == 0);
  CHECK(!strcmp(v.policy, "slice"));
  CHECK(v.n_slice >= 2);
  int ok1 = 0, ok6 = 0;
  for (int i = 0; i < v.n_slice; i++) {
    if (v.slice[i].id == 1 && v.slice[i].weight == 700 && v.slice[i].cap == 0)
      ok1 = 1;
    if (v.slice[i].id == 6 && v.slice[i].weight == 1000 && v.slice[i].cap == 30)
      ok6 = 1;
  }
  CHECK(ok1 && ok6);
  CHECK(v.n_ue == 3);
  int in6 = 0, in0 = 0;
  for (int i = 0; i < v.n_ue; i++) {
    if (v.ue[i].rnti == 0x1002 && v.ue[i].slice == 6)
      in6 = 1;
    if (v.ue[i].rnti != 0x1002 && v.ue[i].slice == 0)
      in0++;
  }
  CHECK(in6 && in0 == 2);

  /* a reply cut off at any length, an error reply and text that is no reply never crash and never give invented entries */
  size_t full = strlen(g);
  for (size_t cut = 0; cut <= full; cut += 7) {
    char part[8192];
    memcpy(part, g, cut);
    part[cut] = '\0';
    int rc = nr_ctrl_sm_parse_get(part, &v);
    CHECK(rc == 0 || rc == -1);
    CHECK(v.n_slice <= NR_CTRL_SM_MAX_SLICE && v.n_ue <= NR_CTRL_SM_MAX_UE);
    for (int i = 0; i < v.n_ue; i++)
      CHECK(v.ue[i].rnti >= 0x1001 && v.ue[i].rnti <= 0x1003);
  }
  CHECK(nr_ctrl_sm_parse_get("ERR unknown_command\n", &v) != 0);
  CHECK(nr_ctrl_sm_parse_get("", &v) != 0);
  CHECK(nr_ctrl_sm_parse_get("OK ctrl v1\n", &v) != 0); /* a PING reply has no policy */
  CHECK(nr_ctrl_sm_parse_get("OK version=1 policy=pf\nSLICE id=9 weight=1 cap=0\nSLICE id=x\nUE rnti=0xzz\nUE rnti=0x1 slice=9\n", &v) == 0 && v.n_slice == 0 && v.n_ue == 0);
}

/* A slice message from the xApp, through the translator and the real endpoint, against the scheduler. */
static void test_slice_sm_roundtrip(void)
{
  char r[4096], g[8192], err[128];
  double got[40], tot;
  nr_ctrl_sm_plan_t plan;
  nr_ctrl_sm_slice_t s[2];
  nr_ctrl_sm_assoc_t a[4];
  int refused;

  sim_setup(4);
  slice_policy_on();
  memset(got, 0, sizeof(got));

  /* two NVS slices of 75 % and 25 %, UEs 1001 and 1002 in the first, 1003 and 1004 in the second */
  s[0] = (nr_ctrl_sm_slice_t){.id = 1, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = 0.75f};
  s[1] = (nr_ctrl_sm_slice_t){.id = 2, .alg = NR_CTRL_SM_ALG_NVS_CAPACITY, .pct_reserved = 0.25f};
  CHECK(nr_ctrl_sm_plan_add(s, 2, 0, &plan, err, sizeof(err)) == 0);
  CHECK(apply_plan(&plan, &refused, r, sizeof(r)) == 4 && refused == -1);
  for (int i = 0; i < 4; i++)
    a[i] = (nr_ctrl_sm_assoc_t){.rnti = (uint32_t)(0x1001 + i), .dl_id = i < 2 ? 1u : 2u};
  CHECK(nr_ctrl_sm_plan_assoc(a, 4, &plan, err, sizeof(err)) == 0);
  CHECK(apply_plan(&plan, &refused, r, sizeof(r)) == 4 && refused == -1);
  dyn_run(4, NULL, 4000, got, 0, NULL);
  tot = got[0] + got[1] + got[2] + got[3];
  CHECK(fabs((got[0] + got[1]) / tot - 0.75) < 0.02);
  CHECK(fabs(got[0] / tot - 0.375) < 0.02 && fabs(got[2] / tot - 0.125) < 0.02);

  /* the status read back through the indication path agrees */
  nr_ctrl_sm_view_t v;
  CHECK(nr_ctrl_e2_send("GET", g, sizeof(g)) == 0 && nr_ctrl_sm_parse_get(g, &v) == 0);
  int w1 = 0, w2 = 0;
  for (int i = 0; i < v.n_slice; i++) {
    w1 += v.slice[i].id == 1 && v.slice[i].weight == 750;
    w2 += v.slice[i].id == 2 && v.slice[i].weight == 250;
  }
  CHECK(w1 == 1 && w2 == 1 && v.n_ue == 4);

  /* delete slice 1: its UEs fall back to slice 0 (weight 1000) next to slice 2 (weight 250), 80 : 20 */
  uint32_t del = 1;
  CHECK(nr_ctrl_sm_plan_del(&del, 1, 0, &plan, err, sizeof(err)) == 0 && apply_plan(&plan, &refused, r, sizeof(r)) == 1);
  memset(got, 0, sizeof(got));
  dyn_run(4, NULL, 4000, got, 0, NULL);
  tot = got[0] + got[1] + got[2] + got[3];
  CHECK(fabs((got[0] + got[1]) / tot - 0.80) < 0.02);
  CHECK(fabs(got[0] / tot - got[1] / tot) < 0.02); /* the two UEs of slice 0 share it equally */

  /* a static slice limits the resource blocks the slice takes per call */
  s[0] = (nr_ctrl_sm_slice_t){.id = 2, .alg = NR_CTRL_SM_ALG_STATIC, .pos_low = 0, .pos_high = 9};
  CHECK(nr_ctrl_sm_plan_add(s, 1, 0, &plan, err, sizeof(err)) == 0 && apply_plan(&plan, &refused, r, sizeof(r)) == 2);
  nr_ctrl_e2_send("GET", g, sizeof(g));
  CHECK(has(g, "SLICE id=2 weight=1000 cap=10"));
  memset(got, 0, sizeof(got));
  dyn_run(4, NULL, 2000, got, 0, NULL);
  tot = got[0] + got[1] + got[2] + got[3];
  CHECK(got[2] + got[3] > 0 && (got[2] + got[3]) / tot < 0.10); /* at most 10 of the resource blocks of a slot */

  /* a message without slices removes all of them */
  CHECK(nr_ctrl_sm_plan_add(s, 0, 0, &plan, err, sizeof(err)) == 0 && apply_plan(&plan, &refused, r, sizeof(r)) == 8);
  nr_ctrl_e2_send("GET", g, sizeof(g));
  CHECK(!has(g, "cap=10") && !has(g, "weight=750") && !has(g, "slice=1") && !has(g, "slice=2"));
  memset(got, 0, sizeof(got));
  dyn_run(4, NULL, 4000, got, 0, NULL);
  tot = got[0] + got[1] + got[2] + got[3];
  for (int i = 0; i < 4; i++)
    CHECK(fabs(got[i] / tot - 0.25) < 0.02);

  /* the endpoint refuses an unknown RNTI: the commands before it are applied, the rest is not */
  a[0] = (nr_ctrl_sm_assoc_t){.rnti = 0x1001, .dl_id = 3};
  a[1] = (nr_ctrl_sm_assoc_t){.rnti = 0x7777, .dl_id = 3};
  a[2] = (nr_ctrl_sm_assoc_t){.rnti = 0x1002, .dl_id = 3};
  CHECK(nr_ctrl_sm_plan_assoc(a, 3, &plan, err, sizeof(err)) == 0);
  CHECK(apply_plan(&plan, &refused, r, sizeof(r)) == 1 && refused == 1 && has(r, "unknown_rnti"));
  nr_ctrl_e2_send("GET", g, sizeof(g));
  CHECK(has(g, "rnti=0x1001") && has(g, "slice=3 source=rnti") && !has(g, "rnti=0x1002 weight=1000 explicit=0 cap=0 granted_rbs=0 granted_slots=0 commit_fail=0 mac_dl_rbs=0 mac_dl_rbs_retx=0 mac_dl_sdu_bytes=0 slice=3"));
  slice_policy_on();
}

/* Several UEs (or slices) that were behind together must not hold the others off: the credit of each is
 * bounded against the leader, not against the other laggards. */
static void test_credit_groups(void)
{
  char r[512];
  double got[40], tot;

  /* wrr: UEs 1003 and 1004 have nothing to send for 3000 calls, then all four have data */
  sim_setup(4);
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wrr", r, sizeof(r));
  for (int s = 0; s < 3000; s++) {
    sim_slot_prepare(4, 1000000);
    cands[2].skipped = cands[3].skipped = true;
    sim_slot(4, 4);
  }
  memset(got, 0, sizeof(got));
  dyn_run(4, NULL, 4000, got, 0, NULL);
  tot = got[0] + got[1] + got[2] + got[3];
  for (int i = 0; i < 4; i++)
    CHECK(fabs(got[i] / tot - 0.25) < 0.02);

  /* the same with three late UEs out of four */
  sim_setup(4);
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wrr", r, sizeof(r));
  for (int s = 0; s < 3000; s++) {
    sim_slot_prepare(4, 1000000);
    cands[1].skipped = cands[2].skipped = cands[3].skipped = true;
    sim_slot(4, 4);
  }
  memset(got, 0, sizeof(got));
  dyn_run(4, NULL, 4000, got, 0, NULL);
  tot = got[0] + got[1] + got[2] + got[3];
  for (int i = 0; i < 4; i++)
    CHECK(fabs(got[i] / tot - 0.25) < 0.02);

  /* slices: slice 1 alone for 3000 calls, then slices 2 and 3 appear (equal weights, one UE each) */
  sim_setup(3);
  slice_policy_on();
  ctl("ASSOC 1001 1", r, sizeof(r));
  ctl("ASSOC 1002 2", r, sizeof(r));
  ctl("ASSOC 1003 3", r, sizeof(r));
  for (int s = 0; s < 3000; s++) {
    sim_slot_prepare(3, 1000000);
    cands[1].skipped = cands[2].skipped = true;
    sim_slot(3, 4);
  }
  memset(got, 0, sizeof(got));
  dyn_run(3, NULL, 4000, got, 0, NULL);
  tot = got[0] + got[1] + got[2];
  for (int i = 0; i < 3; i++)
    CHECK(fabs(got[i] / tot - 1.0 / 3.0) < 0.02);

  /* the burst of the late ones is still allowed, up to the bound: within the first 100 calls the late UEs get
   * more than their share, but the leader keeps some of the calls */
  sim_setup(4);
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wrr", r, sizeof(r));
  for (int s = 0; s < 3000; s++) {
    sim_slot_prepare(4, 1000000);
    cands[2].skipped = cands[3].skipped = true;
    sim_slot(4, 4);
  }
  double early[40];
  memset(got, 0, sizeof(got));
  memset(early, 0, sizeof(early));
  dyn_run(4, NULL, 2000, got, 200, early);
  CHECK(early[0] + early[1] > 0);                       /* the leaders are served during the catch-up */
  CHECK((early[2] + early[3]) / (early[0] + early[1] + early[2] + early[3]) < 0.75);
}

int main(void)
{
  test_split_examples();
  test_split_properties();
  test_protocol();
  test_policy();
  test_budget();
  test_e2();
  test_slices();
  test_slices_e2();
  test_ue_credit();
  test_slices_dynamic();
  test_credit_groups();
  test_slice_del_command();
  test_slice_sm_plan();
  test_slice_sm_parse();
  test_slice_sm_roundtrip();
  test_second_instance();
  test_many_ues();
  test_concurrency();
  printf("checks=%d failures=%d\n", checks, failures);
  return failures ? 1 : 0;
}
