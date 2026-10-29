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

/* A second gNB object cannot be controlled; verify the single-instance guard. */
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
  const char *cmds[] = {"POLICY wshare", "POLICY rr", "POLICY pf", "WEIGHT 1001 5000", "WEIGHT 1002 700", "CAP 1001 30", "CAP 1001 0", "GET", "RESET"};
  for (int i = 0; i < 3000; i++)
    ctl(cmds[i % 9], r, sizeof(r));
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

int main(void)
{
  test_split_examples();
  test_split_properties();
  test_protocol();
  test_policy();
  test_budget();
  test_second_instance();
  test_many_ues();
  test_concurrency();
  printf("checks=%d failures=%d\n", checks, failures);
  return failures ? 1 : 0;
}
