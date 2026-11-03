/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nr_dl_ctrl_state.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nr_ctrl_init(nr_ctrl_state_t *st)
{
  uint32_t version = st->version;
  memset(st, 0, sizeof(*st));
  st->version = version;
  st->policy = NR_CTRL_POLICY_PF;
  st->default_weight = NR_CTRL_DEFAULT_WEIGHT;
  st->ws_share_milli = NR_CTRL_WS_SHARE_INIT;
  st->ws_acc = 1000 - NR_CTRL_WS_SHARE_INIT; /* the first contested slot is shared */
  for (int i = 0; i < NR_CTRL_MAX_SLICE; i++) {
    st->slice[i].weight = NR_CTRL_DEFAULT_WEIGHT;
    st->slice[i].idle_calls = NR_CTRL_SLICE_IDLE_CALLS;
  }
}

const char *nr_ctrl_policy_name(int policy)
{
  switch (policy) {
    case NR_CTRL_POLICY_PF:
      return "pf";
    case NR_CTRL_POLICY_RR:
      return "rr";
    case NR_CTRL_POLICY_WSHARE:
      return "wshare";
    case NR_CTRL_POLICY_WRR:
      return "wrr";
    case NR_CTRL_POLICY_SLICE:
      return "slice";
    default:
      return "unknown";
  }
}

static nr_ctrl_ue_t *find_ue(const nr_ctrl_state_t *st, uint16_t rnti)
{
  for (int i = 0; i < st->n_ue; i++)
    if (st->ue[i].rnti == rnti)
      return (nr_ctrl_ue_t *)&st->ue[i];
  return NULL;
}

static nr_ctrl_ue_t *get_or_add(nr_ctrl_state_t *st, uint16_t rnti)
{
  nr_ctrl_ue_t *u = find_ue(st, rnti);
  if (u)
    return u;
  if (st->n_ue >= NR_CTRL_MAX_UE)
    return NULL;
  u = &st->ue[st->n_ue++];
  memset(u, 0, sizeof(*u));
  u->rnti = rnti;
  u->weight = 0; /* 0: follow the default weight */
  return u;
}

uint32_t nr_ctrl_weight_of(const nr_ctrl_state_t *st, uint16_t rnti)
{
  const nr_ctrl_ue_t *u = find_ue(st, rnti);
  return (u && u->weight) ? u->weight : st->default_weight;
}

int nr_ctrl_cap_of(const nr_ctrl_state_t *st, uint16_t rnti)
{
  const nr_ctrl_ue_t *u = find_ue(st, rnti);
  return u ? u->cap : 0;
}

double nr_ctrl_deficit(const nr_ctrl_state_t *st, uint16_t rnti)
{
  const nr_ctrl_ue_t *u = find_ue(st, rnti);
  uint64_t granted = u ? u->granted_rbs : 0;
  return (double)granted / (double)nr_ctrl_weight_of(st, rnti);
}

void nr_ctrl_account(nr_ctrl_state_t *st, uint16_t rnti, int rbs)
{
  nr_ctrl_ue_t *u = get_or_add(st, rnti);
  if (!u || rbs <= 0)
    return;
  u->granted_rbs += (uint64_t)rbs;
  u->granted_slots++;
}

void nr_ctrl_note_nssai(nr_ctrl_state_t *st, uint16_t rnti, uint8_t sst, uint32_t sd)
{
  nr_ctrl_ue_t *u = get_or_add(st, rnti);
  if (!u)
    return;
  u->nssai_known = 1;
  u->sst = sst;
  u->sd = sd & 0xFFFFFFu;
}

int nr_ctrl_slice_of(const nr_ctrl_state_t *st, uint16_t rnti, int *source)
{
  int dummy;
  if (!source)
    source = &dummy;
  const nr_ctrl_ue_t *u = find_ue(st, rnti);
  if (u && u->assoc && u->slice < NR_CTRL_MAX_SLICE) {
    *source = NR_CTRL_SRC_RNTI;
    return u->slice;
  }
  if (u && u->nssai_known)
    for (int i = 0; i < st->n_rule; i++)
      if (st->rule[i].sst == u->sst && st->rule[i].sd == u->sd) {
        *source = NR_CTRL_SRC_NSSAI;
        return st->rule[i].slice;
      }
  *source = NR_CTRL_SRC_DEFAULT;
  return 0;
}

double nr_ctrl_slice_deficit(const nr_ctrl_state_t *st, int slice)
{
  if (slice < 0 || slice >= NR_CTRL_MAX_SLICE)
    return 0.0;
  return (double)st->slice[slice].granted_rbs / (double)st->slice[slice].weight;
}

int nr_ctrl_slice_cap(const nr_ctrl_state_t *st, int slice)
{
  return (slice >= 0 && slice < NR_CTRL_MAX_SLICE) ? st->slice[slice].cap : 0;
}

void nr_ctrl_account_slice(nr_ctrl_state_t *st, int slice, int rbs)
{
  if (slice < 0 || slice >= NR_CTRL_MAX_SLICE || rbs <= 0)
    return;
  st->slice[slice].granted_rbs += (uint64_t)rbs;
  st->slice[slice].granted_slots++;
}

int nr_ctrl_cap_min(int a, int b)
{
  if (a <= 0)
    return b > 0 ? b : 0;
  if (b <= 0)
    return a;
  return a < b ? a : b;
}

void nr_ctrl_slice_activity(nr_ctrl_state_t *st, const uint8_t backlogged[NR_CTRL_MAX_SLICE])
{
  /* lowest deficit among the slices that were already backlogged */
  double lowest = -1.0;
  for (int i = 0; i < NR_CTRL_MAX_SLICE; i++) {
    if (!backlogged[i] || st->slice[i].idle_calls >= NR_CTRL_SLICE_IDLE_CALLS)
      continue;
    double d = nr_ctrl_slice_deficit(st, i);
    if (lowest < 0.0 || d < lowest)
      lowest = d;
  }
  for (int i = 0; i < NR_CTRL_MAX_SLICE; i++) {
    nr_ctrl_slice_t *sl = &st->slice[i];
    if (backlogged[i]) {
      if (sl->idle_calls >= NR_CTRL_SLICE_IDLE_CALLS && lowest >= 0.0) {
        uint64_t floor_rbs = (uint64_t)(lowest * (double)sl->weight);
        if (floor_rbs > sl->granted_rbs)
          sl->granted_rbs = floor_rbs;
      }
      sl->idle_calls = 0;
    } else if (sl->idle_calls < NR_CTRL_SLICE_IDLE_CALLS) {
      sl->idle_calls++;
    }
  }
}

void nr_ctrl_note_commit_fail(nr_ctrl_state_t *st, uint16_t rnti, int slot, int cce)
{
  st->commit_fail++;
  if (cce)
    st->fail_cce++;
  else
    st->fail_pucch++;
  if (slot >= 0 && slot < NR_CTRL_SLOTS)
    st->slot_fail[slot]++;
  nr_ctrl_ue_t *u = get_or_add(st, rnti);
  if (u)
    u->commit_fail++;
}

void nr_ctrl_prune(nr_ctrl_state_t *st, const nr_ctrl_ue_view_t *ues, int n_ues)
{
  int w = 0;
  for (int r = 0; r < st->n_ue; r++) {
    int alive = 0;
    for (int k = 0; k < n_ues; k++)
      if (ues[k].rnti == st->ue[r].rnti) {
        alive = 1;
        break;
      }
    if (alive) {
      if (w != r)
        st->ue[w] = st->ue[r];
      w++;
    }
  }
  st->n_ue = w;
}

static int is_connected(const nr_ctrl_ue_view_t *ues, int n_ues, uint16_t rnti)
{
  for (int k = 0; k < n_ues; k++)
    if (ues[k].rnti == rnti)
      return 1;
  return 0;
}

static int parse_ulong(const char *s, int base, unsigned long max, unsigned long *out)
{
  if (!s || !*s || isspace((unsigned char)*s) || *s == '-' || *s == '+')
    return 0;
  char *end = NULL;
  errno = 0;
  unsigned long v = strtoul(s, &end, base);
  if (errno || !end || *end != '\0' || v > max)
    return 0;
  *out = v;
  return 1;
}

static int parse_rnti(const char *s, uint16_t *rnti)
{
  unsigned long v;
  if (!parse_ulong(s, 16, 0xffff, &v) || v == 0)
    return 0;
  *rnti = (uint16_t)v;
  return 1;
}

static int reply_err(char *reply, size_t size, const char *reason)
{
  return snprintf(reply, size, "ERR %s\n", reason);
}

static int reply_ok(const nr_ctrl_state_t *st, char *reply, size_t size)
{
  return snprintf(reply, size, "OK version=%u policy=%s\n", (unsigned)st->version, nr_ctrl_policy_name(st->policy));
}

static int reply_status(const nr_ctrl_state_t *st, const nr_ctrl_ue_view_t *ues, int n_ues, char *reply, size_t size)
{
  int len = snprintf(reply,
                     size,
                     "OK version=%u policy=%s default_weight=%u min_rb=%d calls=%llu commit_fail=%llu fail_cce=%llu fail_pucch=%llu multi_ue_slots=%llu "
                     "ws_groups=%llu ws_free_rb=%llu ws_demand_rb=%llu ws_planned_rb=%llu ws_committed_rb=%llu ws_share_milli=%u ws_shared_slots=%llu ws_single_slots=%llu ws_lost_slots=%llu\n",
                     (unsigned)st->version,
                     nr_ctrl_policy_name(st->policy),
                     (unsigned)st->default_weight,
                     NR_CTRL_MIN_RB,
                     (unsigned long long)st->calls,
                     (unsigned long long)st->commit_fail,
                     (unsigned long long)st->fail_cce,
                     (unsigned long long)st->fail_pucch,
                     (unsigned long long)st->multi_ue_slots,
                     (unsigned long long)st->ws_groups,
                     (unsigned long long)st->ws_free_rb,
                     (unsigned long long)st->ws_demand_rb,
                     (unsigned long long)st->ws_planned_rb,
                     (unsigned long long)st->ws_committed_rb,
                     (unsigned)st->ws_share_milli,
                     (unsigned long long)st->ws_shared_slots,
                     (unsigned long long)st->ws_single_slots,
                     (unsigned long long)st->ws_lost_slots);
  int in_slice[NR_CTRL_MAX_SLICE] = {0};
  int any_member = 0; /* some UE is in a slice by association or rule */
  for (int k = 0; k < n_ues; k++) {
    int src;
    in_slice[nr_ctrl_slice_of(st, ues[k].rnti, &src)]++;
    if (src != NR_CTRL_SRC_DEFAULT)
      any_member = 1;
  }
  for (int i = 0; i < NR_CTRL_MAX_SLICE && len > 0 && (size_t)len < size; i++) {
    const nr_ctrl_slice_t *sl = &st->slice[i];
    if (!sl->configured && !sl->granted_rbs && !(in_slice[i] && any_member))
      continue;
    len += snprintf(reply + len,
                    size - (size_t)len,
                    "SLICE id=%d weight=%u cap=%d granted_rbs=%llu granted_slots=%llu ues=%d\n",
                    i,
                    (unsigned)sl->weight,
                    sl->cap,
                    (unsigned long long)sl->granted_rbs,
                    (unsigned long long)sl->granted_slots,
                    in_slice[i]);
  }
  for (int i = 0; i < st->n_rule && len > 0 && (size_t)len < size; i++)
    len += snprintf(reply + len, size - (size_t)len, "RULE sst=%u sd=%06x slice=%u\n", (unsigned)st->rule[i].sst, (unsigned)st->rule[i].sd, (unsigned)st->rule[i].slice);
  for (int k = 0; k < n_ues && len > 0 && (size_t)len < size; k++) {
    const nr_ctrl_ue_t *u = find_ue(st, ues[k].rnti);
    int src;
    int sl = nr_ctrl_slice_of(st, ues[k].rnti, &src);
    char nssai[24];
    if (u && u->nssai_known)
      snprintf(nssai, sizeof(nssai), "%u:%06x", (unsigned)u->sst, (unsigned)u->sd);
    else
      snprintf(nssai, sizeof(nssai), "-");
    len += snprintf(reply + len,
                    size - (size_t)len,
                    "UE rnti=0x%04x weight=%u explicit=%d cap=%d granted_rbs=%llu granted_slots=%llu commit_fail=%llu "
                    "mac_dl_rbs=%llu mac_dl_rbs_retx=%llu mac_dl_sdu_bytes=%llu slice=%d source=%s nssai=%s\n",
                    ues[k].rnti,
                    (unsigned)nr_ctrl_weight_of(st, ues[k].rnti),
                    (u && u->weight) ? 1 : 0,
                    u ? u->cap : 0,
                    (unsigned long long)(u ? u->granted_rbs : 0),
                    (unsigned long long)(u ? u->granted_slots : 0),
                    (unsigned long long)(u ? u->commit_fail : 0),
                    (unsigned long long)ues[k].mac_dl_rbs,
                    (unsigned long long)ues[k].mac_dl_rbs_retx,
                    (unsigned long long)ues[k].mac_dl_sdu_bytes,
                    sl,
                    src == NR_CTRL_SRC_RNTI ? "rnti" : (src == NR_CTRL_SRC_NSSAI ? "nssai" : "default"),
                    nssai);
  }
  for (int k = 0; k < NR_CTRL_SLOTS && len > 0 && (size_t)len < size; k++) {
    if (st->slot_calls[k] || st->slot_fail[k])
      len += snprintf(reply + len, size - (size_t)len, "SLOT %d calls=%u fail=%u\n", k, (unsigned)st->slot_calls[k], (unsigned)st->slot_fail[k]);
  }
  if (len < 0)
    return 0;
  if ((size_t)len >= size)
    len = (int)size - 1;
  return len;
}

int nr_ctrl_handle(nr_ctrl_state_t *st, const char *cmd, const nr_ctrl_ue_view_t *ues, int n_ues, char *reply, size_t reply_size)
{
  char buf[128];
  size_t l = strnlen(cmd, sizeof(buf));
  if (l >= sizeof(buf))
    return reply_err(reply, reply_size, "command_too_long");
  memcpy(buf, cmd, l + 1);

  char *tok[4];
  int nt = 0;
  char *save = NULL;
  for (char *t = strtok_r(buf, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save)) {
    if (nt == 4)
      return reply_err(reply, reply_size, "too_many_arguments");
    tok[nt++] = t;
  }
  if (nt == 0)
    return reply_err(reply, reply_size, "empty_command");
  for (char *p = tok[0]; *p; p++)
    *p = (char)toupper((unsigned char)*p);

  if (!strcmp(tok[0], "PING") && nt == 1)
    return snprintf(reply, reply_size, "OK ctrl v1\n");

  if (!strcmp(tok[0], "GET") && nt == 1)
    return reply_status(st, ues, n_ues, reply, reply_size);

  if (!strcmp(tok[0], "RESET") && nt == 1) {
    nr_ctrl_init(st);
    st->version++;
    return reply_ok(st, reply, reply_size);
  }

  if (!strcmp(tok[0], "POLICY") && nt == 2) {
    for (char *p = tok[1]; *p; p++)
      *p = (char)tolower((unsigned char)*p);
    int pol = -1;
    if (!strcmp(tok[1], "pf"))
      pol = NR_CTRL_POLICY_PF;
    else if (!strcmp(tok[1], "rr"))
      pol = NR_CTRL_POLICY_RR;
    else if (!strcmp(tok[1], "wshare"))
      pol = NR_CTRL_POLICY_WSHARE;
    else if (!strcmp(tok[1], "wrr"))
      pol = NR_CTRL_POLICY_WRR;
    else if (!strcmp(tok[1], "slice"))
      pol = NR_CTRL_POLICY_SLICE;
    if (pol < 0)
      return reply_err(reply, reply_size, "unknown_policy");
    st->policy = pol;
    st->version++;
    return reply_ok(st, reply, reply_size);
  }

  if (!strcmp(tok[0], "WEIGHT") && nt == 3) {
    unsigned long w;
    if (!parse_ulong(tok[2], 10, NR_CTRL_MAX_WEIGHT, &w) || w < NR_CTRL_MIN_WEIGHT)
      return reply_err(reply, reply_size, "weight_out_of_range");
    if (!strcmp(tok[1], "*")) {
      st->default_weight = (uint32_t)w;
      st->version++;
      return reply_ok(st, reply, reply_size);
    }
    uint16_t rnti;
    if (!parse_rnti(tok[1], &rnti))
      return reply_err(reply, reply_size, "bad_rnti");
    if (!is_connected(ues, n_ues, rnti))
      return reply_err(reply, reply_size, "unknown_rnti");
    nr_ctrl_ue_t *u = get_or_add(st, rnti);
    if (!u)
      return reply_err(reply, reply_size, "table_full");
    u->weight = (uint32_t)w;
    st->version++;
    return reply_ok(st, reply, reply_size);
  }

  if (!strcmp(tok[0], "CAP") && nt == 3) {
    unsigned long c;
    if (!parse_ulong(tok[2], 10, NR_CTRL_MAX_CAP, &c) || (c != 0 && c < NR_CTRL_MIN_RB))
      return reply_err(reply, reply_size, "cap_out_of_range");
    uint16_t rnti;
    if (!parse_rnti(tok[1], &rnti))
      return reply_err(reply, reply_size, "bad_rnti");
    if (!is_connected(ues, n_ues, rnti))
      return reply_err(reply, reply_size, "unknown_rnti");
    nr_ctrl_ue_t *u = get_or_add(st, rnti);
    if (!u)
      return reply_err(reply, reply_size, "table_full");
    u->cap = (int)c;
    st->version++;
    return reply_ok(st, reply, reply_size);
  }

  if (!strcmp(tok[0], "SLICE") && nt == 4) {
    unsigned long id;
    if (!parse_ulong(tok[1], 10, NR_CTRL_MAX_SLICE - 1, &id))
      return reply_err(reply, reply_size, "bad_slice");
    for (char *p = tok[2]; *p; p++)
      *p = (char)toupper((unsigned char)*p);
    unsigned long v;
    if (!strcmp(tok[2], "WEIGHT")) {
      if (!parse_ulong(tok[3], 10, NR_CTRL_MAX_WEIGHT, &v) || v < NR_CTRL_MIN_WEIGHT)
        return reply_err(reply, reply_size, "weight_out_of_range");
      st->slice[id].weight = (uint32_t)v;
      st->slice[id].configured = 1;
    } else if (!strcmp(tok[2], "CAP")) {
      if (!parse_ulong(tok[3], 10, NR_CTRL_MAX_CAP, &v) || (v != 0 && v < NR_CTRL_MIN_RB))
        return reply_err(reply, reply_size, "cap_out_of_range");
      st->slice[id].cap = (int)v;
      st->slice[id].configured = 1;
    } else {
      return reply_err(reply, reply_size, "unknown_command");
    }
    st->version++;
    return reply_ok(st, reply, reply_size);
  }

  if (!strcmp(tok[0], "ASSOC") && nt == 3) {
    uint16_t rnti;
    if (!parse_rnti(tok[1], &rnti))
      return reply_err(reply, reply_size, "bad_rnti");
    if (!is_connected(ues, n_ues, rnti))
      return reply_err(reply, reply_size, "unknown_rnti");
    unsigned long id = 0;
    int remove = !strcmp(tok[2], "-");
    if (!remove && !parse_ulong(tok[2], 10, NR_CTRL_MAX_SLICE - 1, &id))
      return reply_err(reply, reply_size, "bad_slice");
    nr_ctrl_ue_t *u = get_or_add(st, rnti);
    if (!u)
      return reply_err(reply, reply_size, "table_full");
    u->assoc = remove ? 0 : 1;
    u->slice = remove ? 0 : (uint8_t)id;
    st->version++;
    return reply_ok(st, reply, reply_size);
  }

  if (!strcmp(tok[0], "NSSAIMAP") && nt == 4) {
    unsigned long sst, sd, id = 0;
    if (!parse_ulong(tok[1], 10, 255, &sst))
      return reply_err(reply, reply_size, "bad_sst");
    for (char *p = tok[2]; *p; p++)
      *p = (char)tolower((unsigned char)*p);
    if (!strcmp(tok[2], "none"))
      sd = NR_CTRL_NO_SD;
    else if (!parse_ulong(tok[2], 16, 0xFFFFFFu, &sd))
      return reply_err(reply, reply_size, "bad_sd");
    int remove = !strcmp(tok[3], "-");
    if (!remove && !parse_ulong(tok[3], 10, NR_CTRL_MAX_SLICE - 1, &id))
      return reply_err(reply, reply_size, "bad_slice");
    int at = -1;
    for (int i = 0; i < st->n_rule; i++)
      if (st->rule[i].sst == sst && st->rule[i].sd == sd)
        at = i;
    if (remove) {
      if (at < 0)
        return reply_err(reply, reply_size, "no_such_rule");
      for (int i = at + 1; i < st->n_rule; i++)
        st->rule[i - 1] = st->rule[i];
      st->n_rule--;
    } else {
      if (at < 0) {
        if (st->n_rule >= NR_CTRL_MAX_RULE)
          return reply_err(reply, reply_size, "table_full");
        at = st->n_rule++;
      }
      st->rule[at] = (nr_ctrl_rule_t){.sst = (uint8_t)sst, .sd = (uint32_t)sd, .slice = (uint8_t)id};
    }
    st->version++;
    return reply_ok(st, reply, reply_size);
  }

  return reply_err(reply, reply_size, "unknown_command");
}
