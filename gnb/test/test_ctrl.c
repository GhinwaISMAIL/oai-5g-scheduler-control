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
  memset(&st, 0, sizeof(st));
  nr_ctrl_init(&st);

  run(&st, "PING", r);
  CHECK(!strncmp(r, "OK", 2));
  run(&st, "GET", r);
  CHECK(strstr(r, "policy=pf") && strstr(r, "version=0") && strstr(r, "rnti=0x1001") && strstr(r, "rnti=0x1002"));

  run(&st, "policy WSHARE", r);
  CHECK(!strncmp(r, "OK version=1 policy=wshare", 26));
  CHECK(st.policy == NR_CTRL_POLICY_WSHARE);
  run(&st, "POLICY bogus", r);
  CHECK(!strncmp(r, "ERR unknown_policy", 18) && st.version == 1);

  run(&st, "WEIGHT 1001 3000", r);
  CHECK(!strncmp(r, "OK version=2", 12));
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

static int sim_slot(int n_ue, int max_ue)
{
  nr_dl_sched_params_t p = {.mac = &mac, .max_num_ue = max_ue, .num_beams = 1};
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
    CHECK(sim_slot(2, 4) == 2);
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

  /* a failed commit does not leave a hole or get accounted */
  ctl("RESET", r, sizeof(r));
  ctl("POLICY wshare", r, sizeof(r));
  stub_commit_fail_rnti = 0x1001;
  sim_slot_prepare(2, 1000000);
  sim_slot(2, 4);
  stub_commit_fail_rnti = 0;
  CHECK(!cands[0].scheduled && cands[1].scheduled);

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
  test_second_instance();
  test_many_ues();
  test_concurrency();
  printf("checks=%d failures=%d\n", checks, failures);
  return failures ? 1 : 0;
}
