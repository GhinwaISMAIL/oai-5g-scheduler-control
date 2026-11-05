/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A fake RIC for the native test of xapp/xapp_sched_ctrl.c: the xApp calls the
 * functions of FlexRIC's xApp interface, and this file answers them and prints the
 * control messages it receives, in a text form the test compares.
 *
 * The nodes it offers: node 0 is a gNB with the MAC (142), slice (145) and another service
 * model, node 1 an eNB with the same ones, node 2 a gNB that has only another service model.
 *
 *   STAND_IN_NODES   how many of the three nodes the xApp finds (default 3)
 *   STAND_IN_REFUSE  set: the control answer says "not acknowledged"
 *   STAND_IN_SILENT  set: the subscription never delivers an indication
 *
 * The control message is freed with the xApp's own pointers, so a mismatch between
 * what the xApp allocates and what this file frees shows up under AddressSanitizer.
 */
#include "src/xApp/e42_xapp_api.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_t ind_thread;
static int ind_thread_running = 0;
static sm_cb ind_cb = NULL;

fr_args_t init_fr_args(int argc, char* argv[])
{
  (void)argv;
  fr_args_t a = {.argc = argc};
  return a;
}

void init_xapp_api(fr_args_t const* a)
{
  (void)a;
}

bool try_stop_xapp_api(void)
{
  return true;
}

e2_node_arr_xapp_t e2_nodes_xapp_api(void)
{
  static sm_ran_function_t all[] = {{142}, {145}, {2}};
  static sm_ran_function_t other[] = {{2}};
  static e2_node_connected_xapp_t nodes[3];
  nodes[0] = (e2_node_connected_xapp_t){{ngran_gNB}, 3, all};
  nodes[1] = (e2_node_connected_xapp_t){{ngran_eNB}, 3, all};
  nodes[2] = (e2_node_connected_xapp_t){{ngran_gNB}, 1, other};
  const char* count = getenv("STAND_IN_NODES");
  e2_node_arr_xapp_t arr = {.len = (uint8_t)(count != NULL ? atoi(count) : 3), .n = nodes};
  return arr;
}

void free_e2_node_arr_xapp(e2_node_arr_xapp_t* a)
{
  (void)a;
}

void free_slice_ctrl_msg(slice_ctrl_msg_t* m)
{
  if (m->type == SLICE_CTRL_SM_V0_ADD) {
    slice_conf_t* c = &m->u.add_mod_slice;
    for (uint32_t i = 0; i < c->dl.len_slices; i++) {
      free(c->dl.slices[i].label);
      free(c->dl.slices[i].sched);
    }
    free(c->dl.slices);
    free(c->dl.sched_name);
    free(c->ul.sched_name);
  } else if (m->type == SLICE_CTRL_SM_V0_DEL) {
    free(m->u.del_slice.dl);
    free(m->u.del_slice.ul);
  } else {
    free(m->u.ue_slice.ues);
  }
}

static void print_slices(const char* tag, const ul_dl_slice_conf_t* c)
{
  printf("  %s sched=%.*s slices=%u\n", tag, (int)c->len_sched_name, c->sched_name, (unsigned)c->len_slices);
  for (uint32_t i = 0; i < c->len_slices; i++) {
    const fr_slice_t* s = &c->slices[i];
    printf("  slice id=%u label=%.*s sched=%.*s", (unsigned)s->id, (int)s->len_label, s->label, (int)s->len_sched, s->sched);
    if (s->params.type == SLICE_ALG_SM_V0_NVS && s->params.u.nvs.conf == SLICE_SM_NVS_V0_CAPACITY)
      printf(" nvs-capacity %.3f\n", s->params.u.nvs.u.capacity.u.pct_reserved);
    else if (s->params.type == SLICE_ALG_SM_V0_STATIC)
      printf(" static %u %u\n", (unsigned)s->params.u.sta.pos_low, (unsigned)s->params.u.sta.pos_high);
    else
      printf(" other %d\n", (int)s->params.type);
  }
}

sm_ans_xapp_t control_sm_xapp_api(global_e2_node_id_t* id, uint32_t rf_id, void* wr)
{
  (void)id;
  if (rf_id == 142) {
    const mac_ctrl_req_data_t* r = wr;
    printf("STAND_IN control rf=142 hdr=%08x msg=%08x\n", (unsigned)r->hdr.dummy, (unsigned)r->msg.action);
  } else if (rf_id == 145) {
    const slice_ctrl_req_data_t* r = wr;
    if (r->msg.type == SLICE_CTRL_SM_V0_ADD) {
      printf("STAND_IN control rf=145 ADD\n");
      print_slices("dl", &r->msg.u.add_mod_slice.dl);
      print_slices("ul", &r->msg.u.add_mod_slice.ul);
    } else if (r->msg.type == SLICE_CTRL_SM_V0_DEL) {
      const del_slice_conf_t* d = &r->msg.u.del_slice;
      printf("STAND_IN control rf=145 DEL dl=");
      for (uint32_t i = 0; i < d->len_dl; i++)
        printf("%s%u", i ? "," : "", (unsigned)d->dl[i]);
      printf(" ul=%u\n", (unsigned)d->len_ul);
    } else {
      const ue_slice_conf_t* u = &r->msg.u.ue_slice;
      printf("STAND_IN control rf=145 ASSOC\n");
      for (uint32_t i = 0; i < u->len_ue_slice; i++)
        printf("  ue rnti=0x%04x dl=%u ul=%u\n", (unsigned)u->ues[i].rnti, (unsigned)u->ues[i].dl_id, (unsigned)u->ues[i].ul_id);
    }
  }
  sm_ans_xapp_t a = {0};
  a.success = getenv("STAND_IN_REFUSE") == NULL;
  return a;
}

/* the indication of a gNB with two slices and two UEs, built the way the gNB's reader builds it */
static void* deliver(void* unused)
{
  (void)unused;
  slice_ind_data_t ind = {0};
  ul_dl_slice_conf_t* dl = &ind.msg.slice_conf.dl;
  dl->sched_name = strdup("wrr");
  dl->len_sched_name = 3;
  dl->len_slices = 2;
  dl->slices = calloc(2, sizeof(fr_slice_t));
  const char* label[2] = {"slice-1", "slice-2"};
  float pct[2] = {0.75f, 0.25f};
  for (int i = 0; i < 2; i++) {
    dl->slices[i].id = (uint32_t)i + 1;
    dl->slices[i].label = strdup(label[i]);
    dl->slices[i].len_label = 7;
    dl->slices[i].sched = strdup("wrr");
    dl->slices[i].len_sched = 3;
    dl->slices[i].params.type = SLICE_ALG_SM_V0_NVS;
    dl->slices[i].params.u.nvs.conf = SLICE_SM_NVS_V0_CAPACITY;
    dl->slices[i].params.u.nvs.u.capacity.u.pct_reserved = pct[i];
  }
  ind.msg.ue_slice_conf.len_ue_slice = 2;
  ind.msg.ue_slice_conf.ues = calloc(2, sizeof(ue_slice_assoc_t));
  ind.msg.ue_slice_conf.ues[0] = (ue_slice_assoc_t){.rnti = 0x9e82, .dl_id = 1};
  ind.msg.ue_slice_conf.ues[1] = (ue_slice_assoc_t){.rnti = 0x53e4, .dl_id = 2};

  sm_ag_if_rd_t rd = {.type = INDICATION_MSG_AGENT_IF_ANS_V0};
  rd.ind.type = SLICE_STATS_V0;
  rd.ind.slice = ind;
  for (int i = 0; i < 3; i++)
    ind_cb(&rd);

  free_slice_ind_msg(&ind.msg);
  return NULL;
}

sm_ans_xapp_t report_sm_xapp_api(global_e2_node_id_t* id, uint32_t rf_id, void* data, sm_cb handler)
{
  (void)id;
  (void)data;
  sm_ans_xapp_t a = {0};
  if (rf_id != 145) {
    a.success = false;
    return a;
  }
  a.success = true;
  a.u.handle = 7;
  if (getenv("STAND_IN_SILENT") == NULL) {
    ind_cb = handler;
    pthread_create(&ind_thread, NULL, deliver, NULL);
    ind_thread_running = 1;
  }
  return a;
}

void rm_report_sm_xapp_api(int const handle)
{
  if (handle == 7 && ind_thread_running) {
    pthread_join(ind_thread, NULL);
    ind_thread_running = 0;
  }
}

slice_ind_msg_t cp_slice_ind_msg(slice_ind_msg_t const* src)
{
  slice_ind_msg_t out = *src;
  const ul_dl_slice_conf_t* s = &src->slice_conf.dl;
  ul_dl_slice_conf_t* d = &out.slice_conf.dl;
  d->sched_name = malloc(s->len_sched_name ? s->len_sched_name : 1);
  memcpy(d->sched_name, s->sched_name, s->len_sched_name);
  d->slices = calloc(s->len_slices ? s->len_slices : 1, sizeof(fr_slice_t));
  for (uint32_t i = 0; i < s->len_slices; i++) {
    d->slices[i] = s->slices[i];
    d->slices[i].label = malloc(s->slices[i].len_label ? s->slices[i].len_label : 1);
    memcpy(d->slices[i].label, s->slices[i].label, s->slices[i].len_label);
    d->slices[i].sched = malloc(s->slices[i].len_sched ? s->slices[i].len_sched : 1);
    memcpy(d->slices[i].sched, s->slices[i].sched, s->slices[i].len_sched);
  }
  out.ue_slice_conf.ues = calloc(src->ue_slice_conf.len_ue_slice ? src->ue_slice_conf.len_ue_slice : 1, sizeof(ue_slice_assoc_t));
  memcpy(out.ue_slice_conf.ues, src->ue_slice_conf.ues, src->ue_slice_conf.len_ue_slice * sizeof(ue_slice_assoc_t));
  return out;
}

void free_slice_ind_msg(slice_ind_msg_t* m)
{
  ul_dl_slice_conf_t* d = &m->slice_conf.dl;
  for (uint32_t i = 0; i < d->len_slices; i++) {
    free(d->slices[i].label);
    free(d->slices[i].sched);
  }
  free(d->slices);
  free(d->sched_name);
  free(m->ue_slice_conf.ues);
}
