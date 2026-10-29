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
                     "OK version=%u policy=%s default_weight=%u min_rb=%d calls=%llu\n",
                     (unsigned)st->version,
                     nr_ctrl_policy_name(st->policy),
                     (unsigned)st->default_weight,
                     NR_CTRL_MIN_RB,
                     (unsigned long long)st->calls);
  for (int k = 0; k < n_ues && len > 0 && (size_t)len < size; k++) {
    const nr_ctrl_ue_t *u = find_ue(st, ues[k].rnti);
    len += snprintf(reply + len,
                    size - (size_t)len,
                    "UE rnti=0x%04x weight=%u explicit=%d cap=%d granted_rbs=%llu granted_slots=%llu "
                    "mac_dl_rbs=%llu mac_dl_rbs_retx=%llu mac_dl_sdu_bytes=%llu\n",
                    ues[k].rnti,
                    (unsigned)nr_ctrl_weight_of(st, ues[k].rnti),
                    (u && u->weight) ? 1 : 0,
                    u ? u->cap : 0,
                    (unsigned long long)(u ? u->granted_rbs : 0),
                    (unsigned long long)(u ? u->granted_slots : 0),
                    (unsigned long long)ues[k].mac_dl_rbs,
                    (unsigned long long)ues[k].mac_dl_rbs_retx,
                    (unsigned long long)ues[k].mac_dl_sdu_bytes);
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

  return reply_err(reply, reply_size, "unknown_command");
}
