/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Native test of the standard slice service model handler (ran_func_slice.c of
 * gnb/patches/0004-e2-slice-control.patch) against a running control endpoint
 * (ctrl_server).  Usage: test_slice_handler <port>.  Exit status 0 = all pass.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openair2/E2AP/RAN_FUNCTION/CUSTOMIZED/ran_func_slice.h"

static int checks, failures;
#define CHECK(cond)                                                   \
  do {                                                                \
    checks++;                                                         \
    if (!(cond)) {                                                    \
      failures++;                                                     \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                 \
  } while (0)

/* what the FlexRIC library does with an indication: free every part */
static void free_ind(slice_ind_data_t* d)
{
  ul_dl_slice_conf_t* c[2] = {&d->msg.slice_conf.dl, &d->msg.slice_conf.ul};
  for (int k = 0; k < 2; k++) {
    if (c[k]->len_sched_name > 0)
      free(c[k]->sched_name);
    for (uint32_t i = 0; i < c[k]->len_slices; i++) {
      if (c[k]->slices[i].len_label > 0)
        free(c[k]->slices[i].label);
      if (c[k]->slices[i].len_sched > 0)
        free(c[k]->slices[i].sched);
    }
    free(c[k]->slices);
  }
  if (d->msg.ue_slice_conf.len_ue_slice > 0)
    free(d->msg.ue_slice_conf.ues);
}

typedef struct {
  int ok;
  char policy[16];
  int n_slice;
  uint32_t id[8];
  float pct[8];
  int n_ue;
  uint16_t rnti[32];
  uint32_t slice[32];
  int64_t tstamp;
} snap_t;

static snap_t snapshot(void)
{
  snap_t s;
  memset(&s, 0, sizeof(s));
  slice_ind_data_t d;
  memset(&d, 0xAA, sizeof(d)); /* the library does not clear it */
  s.ok = read_slice_sm(&d);
  if (!s.ok)
    return s;
  const ul_dl_slice_conf_t* dl = &d.msg.slice_conf.dl;
  snprintf(s.policy, sizeof(s.policy), "%.*s", (int)dl->len_sched_name, dl->sched_name);
  s.n_slice = (int)dl->len_slices;
  for (int i = 0; i < s.n_slice && i < 8; i++) {
    s.id[i] = dl->slices[i].id;
    if (dl->slices[i].params.type == SLICE_ALG_SM_V0_NVS && dl->slices[i].params.u.nvs.conf == SLICE_SM_NVS_V0_CAPACITY)
      s.pct[i] = dl->slices[i].params.u.nvs.u.capacity.u.pct_reserved;
    CHECK(dl->slices[i].len_label > 0 && !strncmp(dl->slices[i].label, "slice-", 6));
    CHECK(dl->slices[i].len_sched == 3 && !memcmp(dl->slices[i].sched, "wrr", 3));
  }
  CHECK(d.msg.slice_conf.ul.len_slices == 0 && d.msg.slice_conf.ul.len_sched_name == 0);
  CHECK(d.proc_id == NULL && d.msg.tstamp > 0);
  s.n_ue = (int)d.msg.ue_slice_conf.len_ue_slice;
  for (int i = 0; i < s.n_ue && i < 32; i++) {
    s.rnti[i] = d.msg.ue_slice_conf.ues[i].rnti;
    s.slice[i] = d.msg.ue_slice_conf.ues[i].dl_id;
  }
  s.tstamp = d.msg.tstamp;
  free_ind(&d);
  return s;
}

static int slice_of(const snap_t* s, uint16_t rnti)
{
  for (int i = 0; i < s->n_ue; i++)
    if (s->rnti[i] == rnti)
      return (int)s->slice[i];
  return -1;
}

static float pct_of(const snap_t* s, uint32_t id)
{
  for (int i = 0; i < s->n_slice; i++)
    if (s->id[i] == id)
      return s->pct[i];
  return -1.0f;
}

static void send_add(const fr_slice_t* sl, uint32_t n, uint32_t n_ul)
{
  slice_ctrl_req_data_t req;
  memset(&req, 0, sizeof(req));
  req.msg.type = SLICE_CTRL_SM_V0_ADD;
  req.msg.u.add_mod_slice.dl.len_slices = n;
  req.msg.u.add_mod_slice.dl.slices = (fr_slice_t*)sl;
  fr_slice_t dummy_ul[2] = {{0}};
  req.msg.u.add_mod_slice.ul.len_slices = n_ul;
  req.msg.u.add_mod_slice.ul.slices = dummy_ul;
  sm_ag_if_ans_t ans = write_ctrl_slice_sm(&req);
  CHECK(ans.type == CTRL_OUTCOME_SM_AG_IF_ANS_V0);
}

static fr_slice_t nvs(uint32_t id, float pct)
{
  fr_slice_t s;
  memset(&s, 0, sizeof(s));
  s.id = id;
  s.params.type = SLICE_ALG_SM_V0_NVS;
  s.params.u.nvs.conf = SLICE_SM_NVS_V0_CAPACITY;
  s.params.u.nvs.u.capacity.u.pct_reserved = pct;
  return s;
}

static void send_assoc(const uint16_t* rnti, const uint32_t* id, uint32_t n)
{
  slice_ctrl_req_data_t req;
  memset(&req, 0, sizeof(req));
  ue_slice_assoc_t ues[8];
  memset(ues, 0, sizeof(ues));
  for (uint32_t i = 0; i < n; i++) {
    ues[i].rnti = rnti[i];
    ues[i].dl_id = id[i];
  }
  req.msg.type = SLICE_CTRL_SM_V0_UE_SLICE_ASSOC;
  req.msg.u.ue_slice.len_ue_slice = n;
  req.msg.u.ue_slice.ues = ues;
  write_ctrl_slice_sm(&req);
}

static void send_del(const uint32_t* ids, uint32_t n)
{
  slice_ctrl_req_data_t req;
  memset(&req, 0, sizeof(req));
  req.msg.type = SLICE_CTRL_SM_V0_DEL;
  req.msg.u.del_slice.len_dl = n;
  req.msg.u.del_slice.dl = (uint32_t*)ids;
  write_ctrl_slice_sm(&req);
}

int main(int argc, char** argv)
{
  if (argc != 2)
    return 2;
  setenv("SCHED_CTRL_PORT", argv[1], 1);
  setenv("SCHED_CTRL_ADDR", "127.0.0.1", 1);

  snap_t s = snapshot();
  CHECK(s.ok && !strcmp(s.policy, "pf") && s.n_slice == 0 && s.n_ue == 2);
  CHECK(slice_of(&s, 0x1001) == 0 && slice_of(&s, 0x1002) == 0);

  /* add two slices and put each UE in one */
  fr_slice_t two[2] = {nvs(1, 0.75f), nvs(2, 0.25f)};
  send_add(two, 2, 0);
  uint16_t r12[2] = {0x1001, 0x1002};
  uint32_t i12[2] = {1, 2};
  send_assoc(r12, i12, 2);
  s = snapshot();
  CHECK(s.n_slice == 2 && fabsf(pct_of(&s, 1) - 0.75f) < 0.0005f && fabsf(pct_of(&s, 2) - 0.25f) < 0.0005f);
  CHECK(slice_of(&s, 0x1001) == 1 && slice_of(&s, 0x1002) == 2);

  /* a message with one bad slice changes nothing */
  snap_t before = snapshot();
  fr_slice_t mixed[2] = {nvs(3, 0.5f), nvs(4, 0.5f)};
  mixed[1].params.u.nvs.conf = SLICE_SM_NVS_V0_RATE; /* not supported */
  send_add(mixed, 2, 0);
  fr_slice_t bad_id[1] = {nvs(9, 0.5f)};
  send_add(bad_id, 1, 0);
  fr_slice_t bad_pct[1] = {nvs(3, 1.5f)};
  send_add(bad_pct, 1, 0);
  fr_slice_t ok_one[1] = {nvs(3, 0.5f)};
  send_add(ok_one, 1, 1); /* with an uplink slice */
  fr_slice_t edf[1];
  memset(edf, 0, sizeof(edf));
  edf[0].id = 3;
  edf[0].params.type = SLICE_ALG_SM_V0_EDF;
  send_add(edf, 1, 0);
  s = snapshot();
  CHECK(s.n_slice == before.n_slice && pct_of(&s, 3) < 0.0f && pct_of(&s, 4) < 0.0f);
  CHECK(slice_of(&s, 0x1001) == 1 && slice_of(&s, 0x1002) == 2);

  /* more slices than the table has */
  fr_slice_t nine[9];
  for (int i = 0; i < 9; i++)
    nine[i] = nvs((uint32_t)(i % 8), 0.1f);
  send_add(nine, 9, 0);
  s = snapshot();
  CHECK(s.n_slice == 2);

  /* a static slice: a cap of the width, the default weight */
  fr_slice_t st[1];
  memset(st, 0, sizeof(st));
  st[0].id = 2;
  st[0].params.type = SLICE_ALG_SM_V0_STATIC;
  st[0].params.u.sta.pos_low = 10;
  st[0].params.u.sta.pos_high = 29;
  send_add(st, 1, 0);
  s = snapshot();
  CHECK(fabsf(pct_of(&s, 2) - 1.0f) < 0.0005f); /* weight 1000 reads back as 1.0; the cap is not part of the message */

  /* association with an unknown RNTI: refused, the UE before it is applied */
  uint16_t rb[2] = {0x1001, 0x7777};
  uint32_t ib[2] = {5, 5};
  send_assoc(rb, ib, 2);
  s = snapshot();
  CHECK(slice_of(&s, 0x1001) == 5);

  /* delete slice 1 and slice 5: their members fall back to slice 0 */
  uint32_t del[2] = {1, 5};
  send_del(del, 2);
  s = snapshot();
  CHECK(pct_of(&s, 1) < 0.0f && slice_of(&s, 0x1001) == 0 && slice_of(&s, 0x1002) == 2);
  uint32_t bad_del[2] = {2, 8};
  send_del(bad_del, 2);
  s = snapshot();
  CHECK(slice_of(&s, 0x1002) == 2); /* the invalid message deleted nothing */

  /* a message without slices removes them all */
  send_add(NULL, 0, 0);
  s = snapshot();
  CHECK(s.n_slice == 0 && slice_of(&s, 0x1002) == 0);

  /* the endpoint is gone: the reader says so and the writer does not crash */
  setenv("SCHED_CTRL_PORT", "9", 1);
  snap_t none = snapshot();
  CHECK(!none.ok);
  send_add(two, 2, 0);

  printf("checks=%d failures=%d\n", checks, failures);
  return failures ? 1 : 0;
}
