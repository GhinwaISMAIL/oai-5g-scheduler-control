/*
 * xApp that sets the downlink scheduler policy, the per-UE weights and caps and the
 * slices of the gNB through the MAC service model's control message.
 *
 *   xapp_sched_ctrl policy <pf|rr|wrr|wshare|slice>
 *   xapp_sched_ctrl weight <rnti|*> <1..65535>     rnti in hexadecimal, * = default weight
 *   xapp_sched_ctrl cap <rnti> <0|5..275>          resource blocks per slot, 0 = no cap
 *   xapp_sched_ctrl slice-weight <0..7> <1..65535>
 *   xapp_sched_ctrl slice-cap <0..7> <0|5..275>
 *   xapp_sched_ctrl slice-del <0..7>               slice back to its defaults, members fall back to slice 0
 *   xapp_sched_ctrl assoc <rnti> <0..7|none>       slice of a UE, none removes the association
 *   xapp_sched_ctrl nssai-map <sst> <sd|none> <0..7|none>   slice for UEs with that S-NSSAI (sd in hex)
 *   xapp_sched_ctrl reset
 *
 * The standard FlexRIC slice service model (ran function 145) is reached with
 * the verbs below; the gNB translates them to the commands above:
 *
 *   xapp_sched_ctrl sm-add none                    ADD without slice: all slices back to their defaults
 *   xapp_sched_ctrl sm-add nvs <id>:<pct> ...      NVS capacity slices, pct in (0,1], weight = pct * 1000
 *   xapp_sched_ctrl sm-add static <id>:<low>-<high> ...   static slices, the width is the slice cap
 *   xapp_sched_ctrl sm-del <id> ...                slices back to their defaults
 *   xapp_sched_ctrl sm-assoc <rnti>:<id> ...       slice of UEs, rnti in hexadecimal
 *   xapp_sched_ctrl sm-get                         print the first slice indication of each gNB
 *
 * Only the syntax is checked here (up to 32 items); the gNB checks the values and
 * writes its reason to its log when it refuses a message ("[E2 Agent]: SLICE CONTROL ...").
 *
 * The command is sent to every connected gNB that registered the MAC service
 * model.  The E2 control acknowledgement says that the gNB received the message,
 * not that the scheduler accepted it: check the result on the cell node with
 * bin/sched-ctl.py status, or in the gNB log ("[E2 Agent]: MAC control ...").
 * Run it after the gNB has registered with the RIC.
 *
 * Built inside the FlexRIC source tree by bin/patch-flexric-sched-ctrl.py, which
 * copies this file and nr_dl_ctrl_e2.h next to the stock MAC control example.
 */
#include "../../../../src/xApp/e42_xapp_api.h"
#include "../../../../src/util/alg_ds/alg/defer.h"

#include "../../../../src/sm/slice_sm/slice_sm_id.h"

#include "nr_dl_ctrl_e2.h"

#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAC_SM_ID 142
#define SM_MAX_ITEMS 32

static void usage(const char *prog)
{
  fprintf(stderr,
          "usage: %s policy <pf|rr|wrr|wshare|slice>\n"
          "       %s weight <rnti|*> <1..65535>\n"
          "       %s cap <rnti> <0|5..275>\n"
          "       %s slice-weight <0..7> <1..65535>\n"
          "       %s slice-cap <0..7> <0|5..275>\n"
          "       %s slice-del <0..7>\n"
          "       %s assoc <rnti> <0..7|none>\n"
          "       %s nssai-map <sst> <sd|none> <0..7|none>\n"
          "       %s reset\n"
          "       %s sm-add none\n"
          "       %s sm-add nvs <id>:<pct> ...             (pct in (0,1])\n"
          "       %s sm-add static <id>:<low>-<high> ...\n"
          "       %s sm-del <id> ...\n"
          "       %s sm-assoc <rnti>:<id> ...              (rnti in hexadecimal)\n"
          "       %s sm-get\n",
          prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ---- standard slice service model ------------------------------------------------ */

typedef enum { SM_ADD_NONE, SM_ADD_NVS, SM_ADD_STATIC, SM_DEL, SM_ASSOC, SM_GET } sm_verb_e;

typedef struct {
  uint32_t id;
  float pct;
  uint32_t low, high;
  uint16_t rnti;
} sm_item_t;

typedef struct {
  sm_verb_e verb;
  int n;
  sm_item_t item[SM_MAX_ITEMS];
} sm_cmd_t;

/* decimal digits only, no sign, no space, at most `max` */
static bool parse_u32(const char *s, uint32_t max, uint32_t *out)
{
  if (s == NULL || s[0] < '0' || s[0] > '9')
    return false;
  char *end = NULL;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 10);
  if (errno != 0 || *end != '\0' || v > max)
    return false;
  *out = (uint32_t)v;
  return true;
}

/* one to four hexadecimal digits, not zero */
static bool parse_rnti(const char *s, uint16_t *out)
{
  size_t len = s == NULL ? 0 : strlen(s);
  if (len < 1 || len > 4)
    return false;
  unsigned v = 0;
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    unsigned d;
    if (c >= '0' && c <= '9')
      d = c - '0';
    else if (c >= 'a' && c <= 'f')
      d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      d = c - 'A' + 10;
    else
      return false;
    v = v * 16 + d;
  }
  if (v == 0)
    return false;
  *out = (uint16_t)v;
  return true;
}

/* a finite number, nothing after it */
static bool parse_pct(const char *s, float *out)
{
  if (s == NULL || s[0] == '\0' || s[0] == ' ')
    return false;
  char *end = NULL;
  errno = 0;
  float v = strtof(s, &end);
  if (errno != 0 || *end != '\0' || !isfinite(v))
    return false;
  *out = v;
  return true;
}

/* splits "left<sep>right" into two strings of at most 31 characters */
static bool split_pair(const char *arg, char sep, char left[32], char right[32])
{
  const char *p = strchr(arg, sep);
  if (p == NULL || p == arg || p[1] == '\0' || (size_t)(p - arg) > 31 || strlen(p + 1) > 31)
    return false;
  memcpy(left, arg, (size_t)(p - arg));
  left[p - arg] = '\0';
  strcpy(right, p + 1);
  return true;
}

/* returns 0 and fills `cmd`, or -1 and the reason in `*err` */
static int sm_parse(int argc, char *argv[], sm_cmd_t *cmd, const char **err)
{
  memset(cmd, 0, sizeof(*cmd));
  const char *verb = argv[1];
  int first = 2;
  if (!strcmp(verb, "sm-get")) {
    if (argc != 2) {
      *err = "sm-get takes no argument";
      return -1;
    }
    cmd->verb = SM_GET;
    return 0;
  }
  if (!strcmp(verb, "sm-add")) {
    if (argc < 3) {
      *err = "sm-add needs none, nvs or static";
      return -1;
    }
    if (!strcmp(argv[2], "none")) {
      if (argc != 3) {
        *err = "sm-add none takes no slice";
        return -1;
      }
      cmd->verb = SM_ADD_NONE;
      return 0;
    }
    if (!strcmp(argv[2], "nvs"))
      cmd->verb = SM_ADD_NVS;
    else if (!strcmp(argv[2], "static"))
      cmd->verb = SM_ADD_STATIC;
    else {
      *err = "sm-add needs none, nvs or static";
      return -1;
    }
    first = 3;
  } else if (!strcmp(verb, "sm-del")) {
    cmd->verb = SM_DEL;
  } else if (!strcmp(verb, "sm-assoc")) {
    cmd->verb = SM_ASSOC;
  } else {
    *err = "command must be policy, weight, cap, reset, slice-weight, slice-cap, slice-del, assoc, nssai-map, sm-add, sm-del, sm-assoc or sm-get";
    return -1;
  }

  if (argc - first < 1) {
    *err = "at least one item is needed";
    return -1;
  }
  if (argc - first > SM_MAX_ITEMS) {
    *err = "too many items (at most 32)";
    return -1;
  }
  for (int i = first; i < argc; i++) {
    sm_item_t *it = &cmd->item[cmd->n];
    char a[32], b[32], c[32], lo[32];
    switch (cmd->verb) {
      case SM_ADD_NVS:
        if (!split_pair(argv[i], ':', a, b) || !parse_u32(a, UINT32_MAX, &it->id) || !parse_pct(b, &it->pct)) {
          *err = "an nvs slice is <id>:<pct>, for example 1:0.75";
          return -1;
        }
        break;
      case SM_ADD_STATIC:
        if (!split_pair(argv[i], ':', a, b) || !split_pair(b, '-', lo, c) || !parse_u32(a, UINT32_MAX, &it->id)
            || !parse_u32(lo, UINT32_MAX, &it->low) || !parse_u32(c, UINT32_MAX, &it->high)) {
          *err = "a static slice is <id>:<low>-<high>, for example 3:0-9";
          return -1;
        }
        break;
      case SM_DEL:
        if (!parse_u32(argv[i], UINT32_MAX, &it->id)) {
          *err = "a slice id is a decimal number";
          return -1;
        }
        break;
      case SM_ASSOC:
        if (!split_pair(argv[i], ':', a, b) || !parse_rnti(a, &it->rnti) || !parse_u32(b, UINT32_MAX, &it->id)) {
          *err = "an association is <rnti>:<id> with the rnti in hexadecimal, for example 9e82:1";
          return -1;
        }
        break;
      default:
        break;
    }
    cmd->n++;
  }
  return 0;
}

/* names are not terminated, as in the stock example; the message carries the length */
static char *dup_name(const char *s, uint32_t *len)
{
  *len = (uint32_t)strlen(s);
  char *p = malloc(*len ? *len : 1);
  if (p == NULL)
    abort();
  memcpy(p, s, *len);
  return p;
}

static slice_ctrl_req_data_t sm_build(const sm_cmd_t *cmd)
{
  slice_ctrl_req_data_t req = {0};
  switch (cmd->verb) {
    case SM_ADD_NONE:
    case SM_ADD_NVS:
    case SM_ADD_STATIC: {
      req.msg.type = SLICE_CTRL_SM_V0_ADD;
      slice_conf_t *conf = &req.msg.u.add_mod_slice;
      conf->dl.sched_name = dup_name("PF", &conf->dl.len_sched_name);
      conf->dl.len_slices = cmd->verb == SM_ADD_NONE ? 0 : (uint32_t)cmd->n;
      if (conf->dl.len_slices > 0) {
        conf->dl.slices = calloc(conf->dl.len_slices, sizeof(fr_slice_t));
        if (conf->dl.slices == NULL)
          abort();
      }
      for (uint32_t i = 0; i < conf->dl.len_slices; i++) {
        fr_slice_t *s = &conf->dl.slices[i];
        char label[24];
        snprintf(label, sizeof(label), "slice-%u", (unsigned)cmd->item[i].id);
        s->id = cmd->item[i].id;
        s->label = dup_name(label, &s->len_label);
        s->sched = dup_name("PF", &s->len_sched);
        if (cmd->verb == SM_ADD_NVS) {
          s->params.type = SLICE_ALG_SM_V0_NVS;
          s->params.u.nvs.conf = SLICE_SM_NVS_V0_CAPACITY;
          s->params.u.nvs.u.capacity.u.pct_reserved = cmd->item[i].pct;
        } else {
          s->params.type = SLICE_ALG_SM_V0_STATIC;
          s->params.u.sta.pos_low = cmd->item[i].low;
          s->params.u.sta.pos_high = cmd->item[i].high;
        }
      }
      conf->ul.sched_name = dup_name("round_robin_ul", &conf->ul.len_sched_name);
      conf->ul.len_slices = 0;
      break;
    }
    case SM_DEL: {
      req.msg.type = SLICE_CTRL_SM_V0_DEL;
      del_slice_conf_t *del = &req.msg.u.del_slice;
      del->len_dl = (uint32_t)cmd->n;
      del->dl = calloc(del->len_dl, sizeof(uint32_t));
      if (del->dl == NULL)
        abort();
      for (uint32_t i = 0; i < del->len_dl; i++)
        del->dl[i] = cmd->item[i].id;
      break;
    }
    case SM_ASSOC: {
      req.msg.type = SLICE_CTRL_SM_V0_UE_SLICE_ASSOC;
      ue_slice_conf_t *assoc = &req.msg.u.ue_slice;
      assoc->len_ue_slice = (uint32_t)cmd->n;
      assoc->ues = calloc(assoc->len_ue_slice, sizeof(ue_slice_assoc_t));
      if (assoc->ues == NULL)
        abort();
      for (uint32_t i = 0; i < assoc->len_ue_slice; i++) {
        assoc->ues[i].rnti = cmd->item[i].rnti;
        assoc->ues[i].dl_id = cmd->item[i].id;
      }
      break;
    }
    default:
      abort();
  }
  return req;
}

/* the first indication of the subscription, copied by the callback thread */
static slice_ind_msg_t first_ind;
static _Atomic int have_ind = 0;

static void sm_cb_slice(sm_ag_if_rd_t const *rd)
{
  if (rd == NULL || rd->type != INDICATION_MSG_AGENT_IF_ANS_V0 || rd->ind.type != SLICE_STATS_V0)
    return;
  if (atomic_load(&have_ind))
    return;
  first_ind = cp_slice_ind_msg(&rd->ind.slice.msg);
  atomic_store(&have_ind, 1);
}

static void print_indication(const slice_ind_msg_t *m)
{
  const ul_dl_slice_conf_t *dl = &m->slice_conf.dl;
  printf("scheduler: %.*s\n", (int)dl->len_sched_name, dl->sched_name ? dl->sched_name : "");
  printf("slices: %u\n", (unsigned)dl->len_slices);
  for (uint32_t i = 0; i < dl->len_slices; i++) {
    const fr_slice_t *s = &dl->slices[i];
    printf("  slice %u label %.*s sched %.*s", (unsigned)s->id, (int)s->len_label, s->label ? s->label : "", (int)s->len_sched,
           s->sched ? s->sched : "");
    if (s->params.type == SLICE_ALG_SM_V0_NVS && s->params.u.nvs.conf == SLICE_SM_NVS_V0_CAPACITY)
      printf(" nvs capacity pct_reserved %.3f\n", s->params.u.nvs.u.capacity.u.pct_reserved);
    else if (s->params.type == SLICE_ALG_SM_V0_STATIC)
      printf(" static %u-%u\n", (unsigned)s->params.u.sta.pos_low, (unsigned)s->params.u.sta.pos_high);
    else
      printf(" algorithm %d\n", (int)s->params.type);
  }
  printf("ues: %u\n", (unsigned)m->ue_slice_conf.len_ue_slice);
  for (uint32_t i = 0; i < m->ue_slice_conf.len_ue_slice; i++)
    printf("  rnti 0x%04x dl slice %u\n", (unsigned)m->ue_slice_conf.ues[i].rnti, (unsigned)m->ue_slice_conf.ues[i].dl_id);
}

static int sm_run(const sm_cmd_t *cmd)
{
  char name[] = "xapp_sched_ctrl";
  char *fr_argv[] = {name, NULL};
  fr_args_t args = init_fr_args(1, fr_argv);
  init_xapp_api(&args);
  sleep(1);

  e2_node_arr_xapp_t nodes = e2_nodes_xapp_api();
  defer({ free_e2_node_arr_xapp(&nodes); });

  printf("Connected E2 nodes = %d\n", nodes.len);
  int done = 0, failed = 0;
  for (int i = 0; i < nodes.len; i++) {
    e2_node_connected_xapp_t *n = &nodes.n[i];
    bool has_slice = false;
    for (size_t j = 0; j < n->len_rf; j++)
      if (n->rf[j].id == SM_SLICE_ID)
        has_slice = true;
    if (!has_slice || (n->id.type != ngran_gNB && n->id.type != ngran_gNB_DU)) {
      printf("Node %d: no slice service model on a gNB, skipped\n", i);
      continue;
    }

    if (cmd->verb == SM_GET) {
      const char *interval = "5_ms";
      sm_ans_xapp_t h = report_sm_xapp_api(&nodes.n[i].id, SM_SLICE_ID, (void *)interval, sm_cb_slice);
      if (!h.success) {
        printf("Node %d: subscription to the slice service model failed\n", i);
        failed++;
        continue;
      }
      for (int w = 0; w < 500 && !atomic_load(&have_ind); w++)
        usleep(10000);
      if (atomic_load(&have_ind)) {
        printf("Node %d: slice indication\n", i);
        print_indication(&first_ind);
        free_slice_ind_msg(&first_ind);
        done++;
      } else {
        printf("Node %d: no slice indication within 5 s\n", i);
        failed++;
      }
      rm_report_sm_xapp_api(h.u.handle);
      atomic_store(&have_ind, 0);
      continue;
    }

    slice_ctrl_req_data_t req = sm_build(cmd);
    sm_ans_xapp_t const a = control_sm_xapp_api(&nodes.n[i].id, SM_SLICE_ID, &req);
    free_slice_ctrl_msg(&req.msg);
    printf("Node %d: slice message (%d items) %s\n", i, cmd->n, a.success ? "acknowledged" : "not acknowledged");
    if (a.success)
      done++;
    else
      failed++;
  }

  while (try_stop_xapp_api() == false)
    usleep(1000);

  if (done == 0 || failed > 0) {
    printf("Slice message not delivered\n");
    return 1;
  }
  if (cmd->verb == SM_GET)
    printf("Slice indication read\n");
  else
    printf("Slice message delivered; check the gNB log (SLICE CONTROL) and sched-ctl.py status\n");
  return 0;
}

int main(int argc, char *argv[])
{
  uint32_t hdr = 0, act = 0;
  const char *err = NULL;
  if (argc >= 2 && !strncmp(argv[1], "sm-", 3)) {
    sm_cmd_t cmd;
    if (sm_parse(argc, argv, &cmd, &err) != 0) {
      fprintf(stderr, "error: %s\n", err);
      usage(argv[0]);
      return 2;
    }
    return sm_run(&cmd);
  }
  if (argc < 2 || argc > 5
      || nr_ctrl_e2_pack(argv[1], argc > 2 ? argv[2] : NULL, argc > 3 ? argv[3] : NULL, argc > 4 ? argv[4] : NULL, &hdr, &act, &err) != 0) {
    if (argc >= 2 && argc <= 5)
      fprintf(stderr, "error: %s\n", err);
    usage(argv[0]);
    return 2;
  }

  /* the FlexRIC options (-c, -p) are not used here: the default configuration file is read */
  char *fr_argv[] = {argv[0], NULL};
  fr_args_t args = init_fr_args(1, fr_argv);
  init_xapp_api(&args);
  sleep(1);

  e2_node_arr_xapp_t nodes = e2_nodes_xapp_api();
  defer({ free_e2_node_arr_xapp(&nodes); });

  printf("Connected E2 nodes = %d\n", nodes.len);
  int sent = 0, failed = 0;
  for (int i = 0; i < nodes.len; i++) {
    e2_node_connected_xapp_t *n = &nodes.n[i];
    bool has_mac = false;
    for (size_t j = 0; j < n->len_rf; j++)
      if (n->rf[j].id == MAC_SM_ID)
        has_mac = true;
    if (!has_mac || (n->id.type != ngran_gNB && n->id.type != ngran_gNB_DU)) {
      printf("Node %d: no MAC service model on a gNB, skipped\n", i);
      continue;
    }
    mac_ctrl_req_data_t wr = {.hdr.dummy = hdr, .msg.action = act};
    sm_ans_xapp_t const a = control_sm_xapp_api(&nodes.n[i].id, MAC_SM_ID, &wr);
    printf("Node %d: control message (header 0x%08x, message 0x%08x) %s\n", i, (unsigned)hdr, (unsigned)act, a.success ? "acknowledged" : "not acknowledged");
    if (a.success)
      sent++;
    else
      failed++;
  }

  while (try_stop_xapp_api() == false)
    usleep(1000);

  if (sent == 0 || failed > 0) {
    printf("Scheduler command not delivered\n");
    return 1;
  }
  printf("Scheduler command delivered; check the cell with sched-ctl.py status\n");
  return 0;
}
