/*
 * Translation between the standard FlexRIC slice service model and the slice
 * commands of the downlink scheduler control (nr_dl_ctrl_state.h).
 *
 * A slice control message (add or modify, delete, UE association) is turned into
 * a list of text commands; the list is checked completely before anything is sent,
 * so a message with an invalid part changes nothing.  The reply of the endpoint to
 * the GET command is read back into a small table for the indication.
 *
 *   ADD/MOD, per DL slice
 *     NVS capacity, pct_reserved p in (0,1]   SLICE id WEIGHT round(p * 1000), SLICE id CAP 0
 *     STATIC, pos_low..pos_high               SLICE id WEIGHT 1000, SLICE id CAP (pos_high - pos_low + 1)
 *                                             (the width is a limit on the resource blocks of the
 *                                             slice per call; the position is not used)
 *     other algorithms, UL slices             refused
 *     no slice at all (algorithm NONE)        SLICEDEL 0..7
 *   DEL, per DL id                            SLICEDEL id
 *   UE association, per UE                    ASSOC rnti dl_id
 *
 * Plain C, no FlexRIC or OAI types, so it is tested natively; the handler in the
 * gNB (ran_func_slice.c) converts the FlexRIC structures to the ones below.
 */
#ifndef NR_DL_CTRL_SLICE_SM_H
#define NR_DL_CTRL_SLICE_SM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NR_CTRL_SM_MAX_SLICE 8
#define NR_CTRL_SM_MAX_UE 32
#define NR_CTRL_SM_MAX_CMD 40
#define NR_CTRL_SM_CMD_LEN 40
#define NR_CTRL_SM_DEFAULT_WEIGHT 1000 /* NR_CTRL_DEFAULT_WEIGHT of the scheduler state */
#define NR_CTRL_SM_MIN_CAP 5
#define NR_CTRL_SM_MAX_CAP 275

typedef enum { NR_CTRL_SM_ALG_NVS_CAPACITY = 0, NR_CTRL_SM_ALG_STATIC = 1, NR_CTRL_SM_ALG_UNSUPPORTED = 2 } nr_ctrl_sm_alg_t;

typedef struct {
  uint32_t id;
  nr_ctrl_sm_alg_t alg;
  float pct_reserved;
  uint32_t pos_low;
  uint32_t pos_high;
} nr_ctrl_sm_slice_t;

typedef struct {
  uint32_t rnti;
  uint32_t dl_id;
} nr_ctrl_sm_assoc_t;

typedef struct {
  int n;
  char text[NR_CTRL_SM_MAX_CMD][NR_CTRL_SM_CMD_LEN];
} nr_ctrl_sm_plan_t;

/* A refused message leaves an empty plan, so nothing of it can be sent by mistake. */
static inline int nr_ctrl_sm_fail(nr_ctrl_sm_plan_t *plan, char *err, size_t size, const char *what, unsigned long value)
{
  plan->n = 0;
  if (err && size)
    snprintf(err, size, "%s (%lu)", what, value);
  return -1;
}

enum { NR_CTRL_SM_CMD_WEIGHT, NR_CTRL_SM_CMD_CAP, NR_CTRL_SM_CMD_DEL, NR_CTRL_SM_CMD_ASSOC };

static inline int nr_ctrl_sm_push(nr_ctrl_sm_plan_t *plan, int cmd, unsigned a, unsigned b)
{
  if (plan->n >= NR_CTRL_SM_MAX_CMD)
    return -1;
  char *out = plan->text[plan->n];
  int n = -1;
  switch (cmd) {
    case NR_CTRL_SM_CMD_WEIGHT:
      n = snprintf(out, NR_CTRL_SM_CMD_LEN, "SLICE %u WEIGHT %u", a, b);
      break;
    case NR_CTRL_SM_CMD_CAP:
      n = snprintf(out, NR_CTRL_SM_CMD_LEN, "SLICE %u CAP %u", a, b);
      break;
    case NR_CTRL_SM_CMD_DEL:
      n = snprintf(out, NR_CTRL_SM_CMD_LEN, "SLICEDEL %u", a);
      break;
    case NR_CTRL_SM_CMD_ASSOC:
      n = snprintf(out, NR_CTRL_SM_CMD_LEN, "ASSOC %04x %u", a, b);
      break;
    default:
      break;
  }
  if (n <= 0 || n >= NR_CTRL_SM_CMD_LEN)
    return -1;
  plan->n++;
  return 0;
}

/* Add or modify the DL slices of one message.  n may be 0 (no slice: all slices deleted). */
static inline int nr_ctrl_sm_plan_add(const nr_ctrl_sm_slice_t *s, unsigned long n, unsigned long n_ul, nr_ctrl_sm_plan_t *plan, char *err, size_t esize)
{
  plan->n = 0;
  if (n_ul > 0)
    return nr_ctrl_sm_fail(plan, err, esize, "uplink slices are not supported, UL slices in the message", n_ul);
  if (n > NR_CTRL_SM_MAX_SLICE)
    return nr_ctrl_sm_fail(plan, err, esize, "more than 8 slices in the message", n);
  if (n == 0) {
    for (unsigned i = 0; i < NR_CTRL_SM_MAX_SLICE; i++)
      if (nr_ctrl_sm_push(plan, NR_CTRL_SM_CMD_DEL, i, 0) != 0)
        return nr_ctrl_sm_fail(plan, err, esize, "too many commands", i);
    return 0;
  }
  unsigned seen = 0;
  for (unsigned long k = 0; k < n; k++) {
    const nr_ctrl_sm_slice_t *x = &s[k];
    if (x->id >= NR_CTRL_SM_MAX_SLICE)
      return nr_ctrl_sm_fail(plan, err, esize, "slice id outside 0..7", x->id);
    if (seen & (1u << x->id))
      return nr_ctrl_sm_fail(plan, err, esize, "slice id given twice", x->id);
    seen |= 1u << x->id;
    unsigned weight = NR_CTRL_SM_DEFAULT_WEIGHT, cap = 0;
    if (x->alg == NR_CTRL_SM_ALG_NVS_CAPACITY) {
      if (!(x->pct_reserved > 0.0f && x->pct_reserved <= 1.0f))
        return nr_ctrl_sm_fail(plan, err, esize, "NVS pct_reserved must be in (0,1], slice", x->id);
      weight = (unsigned)(x->pct_reserved * (float)NR_CTRL_SM_DEFAULT_WEIGHT + 0.5f);
      if (weight < 1)
        weight = 1;
    } else if (x->alg == NR_CTRL_SM_ALG_STATIC) {
      if (x->pos_high < x->pos_low)
        return nr_ctrl_sm_fail(plan, err, esize, "static slice with pos_high below pos_low, slice", x->id);
      unsigned long width = (unsigned long)x->pos_high - x->pos_low + 1ul;
      if (width < NR_CTRL_SM_MIN_CAP || width > NR_CTRL_SM_MAX_CAP)
        return nr_ctrl_sm_fail(plan, err, esize, "static slice width must be 5..275 resource blocks, slice", x->id);
      cap = (unsigned)width;
    } else {
      return nr_ctrl_sm_fail(plan, err, esize, "only STATIC and NVS capacity slices are supported, slice", x->id);
    }
    if (nr_ctrl_sm_push(plan, NR_CTRL_SM_CMD_WEIGHT, x->id, weight) != 0 || nr_ctrl_sm_push(plan, NR_CTRL_SM_CMD_CAP, x->id, cap) != 0)
      return nr_ctrl_sm_fail(plan, err, esize, "too many commands", x->id);
  }
  return 0;
}

static inline int nr_ctrl_sm_plan_del(const uint32_t *dl, unsigned long n, unsigned long n_ul, nr_ctrl_sm_plan_t *plan, char *err, size_t esize)
{
  plan->n = 0;
  if (n_ul > 0)
    return nr_ctrl_sm_fail(plan, err, esize, "uplink slices are not supported, UL ids in the message", n_ul);
  if (n == 0)
    return nr_ctrl_sm_fail(plan, err, esize, "no slice id in the message", 0);
  if (n > NR_CTRL_SM_MAX_SLICE)
    return nr_ctrl_sm_fail(plan, err, esize, "more than 8 slice ids in the message", n);
  for (unsigned long k = 0; k < n; k++) {
    if (dl[k] >= NR_CTRL_SM_MAX_SLICE)
      return nr_ctrl_sm_fail(plan, err, esize, "slice id outside 0..7", dl[k]);
    if (nr_ctrl_sm_push(plan, NR_CTRL_SM_CMD_DEL, (unsigned)dl[k], 0) != 0)
      return nr_ctrl_sm_fail(plan, err, esize, "too many commands", dl[k]);
  }
  return 0;
}

static inline int nr_ctrl_sm_plan_assoc(const nr_ctrl_sm_assoc_t *a, unsigned long n, nr_ctrl_sm_plan_t *plan, char *err, size_t esize)
{
  plan->n = 0;
  if (n == 0)
    return nr_ctrl_sm_fail(plan, err, esize, "no UE in the message", 0);
  if (n > NR_CTRL_SM_MAX_UE)
    return nr_ctrl_sm_fail(plan, err, esize, "more than 32 UEs in the message", n);
  for (unsigned long k = 0; k < n; k++) {
    if (a[k].rnti == 0 || a[k].rnti > 0xffffu)
      return nr_ctrl_sm_fail(plan, err, esize, "RNTI outside 1..65535", a[k].rnti);
    if (a[k].dl_id >= NR_CTRL_SM_MAX_SLICE)
      return nr_ctrl_sm_fail(plan, err, esize, "slice id outside 0..7", a[k].dl_id);
    if (nr_ctrl_sm_push(plan, NR_CTRL_SM_CMD_ASSOC, (unsigned)a[k].rnti, (unsigned)a[k].dl_id) != 0)
      return nr_ctrl_sm_fail(plan, err, esize, "too many commands", a[k].rnti);
  }
  return 0;
}

/* Sends the commands in order and stops at the first one the endpoint refuses.  Returns the
 * number of commands applied; *refused is the index of the refused command, or -1. */
typedef int (*nr_ctrl_sm_send_fn)(const char *text, char *reply, size_t reply_size, void *ctx);

static inline int nr_ctrl_sm_apply(const nr_ctrl_sm_plan_t *plan, nr_ctrl_sm_send_fn send, void *ctx, char *reply, size_t reply_size, int *refused)
{
  *refused = -1;
  for (int i = 0; i < plan->n; i++) {
    if (send(plan->text[i], reply, reply_size, ctx) != 0) {
      *refused = i;
      return i;
    }
  }
  return plan->n;
}

/* ---- the reply of GET, as far as the slice indication needs it ---- */

typedef struct {
  int id;
  unsigned weight;
  int cap;
} nr_ctrl_sm_slice_view_t;

typedef struct {
  unsigned rnti;
  int slice;
} nr_ctrl_sm_ue_view_t;

typedef struct {
  char policy[16];
  int n_slice;
  nr_ctrl_sm_slice_view_t slice[NR_CTRL_SM_MAX_SLICE];
  int n_ue;
  nr_ctrl_sm_ue_view_t ue[NR_CTRL_SM_MAX_UE];
} nr_ctrl_sm_view_t;

/* Returns 0 when the reply is a status reply.  A line cut off by a short buffer is skipped. */
static inline int nr_ctrl_sm_parse_get(const char *reply, nr_ctrl_sm_view_t *v)
{
  memset(v, 0, sizeof(*v));
  if (strncmp(reply, "OK ", 3) != 0)
    return -1;
  const char *line = reply;
  int first = 1;
  while (*line) {
    const char *end = strchr(line, '\n');
    if (!end)
      break; /* last line cut off */
    char buf[512];
    size_t len = (size_t)(end - line);
    if (len >= sizeof(buf))
      len = sizeof(buf) - 1;
    memcpy(buf, line, len);
    buf[len] = '\0';
    if (first) {
      const char *p = strstr(buf, " policy=");
      if (p) {
        p += 8;
        size_t i = 0;
        while (p[i] && p[i] != ' ' && i < sizeof(v->policy) - 1) {
          v->policy[i] = p[i];
          i++;
        }
        v->policy[i] = '\0';
      }
      first = 0;
    } else if (!strncmp(buf, "SLICE ", 6)) {
      int id, cap;
      unsigned w;
      if (sscanf(buf, "SLICE id=%d weight=%u cap=%d", &id, &w, &cap) == 3 && id >= 0 && id < NR_CTRL_SM_MAX_SLICE && v->n_slice < NR_CTRL_SM_MAX_SLICE)
        v->slice[v->n_slice++] = (nr_ctrl_sm_slice_view_t){.id = id, .weight = w, .cap = cap};
    } else if (!strncmp(buf, "UE ", 3)) {
      unsigned r;
      int s;
      const char *p = strstr(buf, " slice=");
      if (p && sscanf(buf, "UE rnti=0x%x", &r) == 1 && sscanf(p, " slice=%d", &s) == 1 && s >= 0 && s < NR_CTRL_SM_MAX_SLICE && v->n_ue < NR_CTRL_SM_MAX_UE)
        v->ue[v->n_ue++] = (nr_ctrl_sm_ue_view_t){.rnti = r, .slice = s};
    }
    line = end + 1;
  }
  return v->policy[0] ? 0 : -1;
}

#endif
