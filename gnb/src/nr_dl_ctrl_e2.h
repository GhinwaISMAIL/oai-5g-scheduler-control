/*
 * E2 transport of the downlink scheduler commands.
 *
 * The pinned MAC service model carries two 32-bit numbers in a control message:
 * the header word (mac_ctrl_hdr_t.dummy) and the message word
 * (mac_ctrl_msg_t.action).  A scheduler command is packed into them as
 *
 *   header word   0x5343 << 16 | arg << 8 | opcode   ("SC" marks a scheduler command)
 *   message word  depends on the opcode
 *
 *   opcode 1  POLICY        arg 0       message = policy id   0 pf, 1 rr, 2 wshare, 3 wrr, 4 slice
 *   opcode 2  WEIGHT        arg 0       message = rnti << 16 | weight   (rnti 0: default weight; weight 1..65535)
 *   opcode 3  CAP           arg 0       message = rnti << 16 | cap      (cap 0, or 5..275 resource blocks)
 *   opcode 4  RESET         arg 0       message = 0
 *   opcode 5  SLICE WEIGHT  arg = slice (0..7)   message = weight 1..65535
 *   opcode 6  SLICE CAP     arg = slice (0..7)   message = cap (0, or 5..275)
 *   opcode 7  ASSOC         arg = slice (0..7), 0xFF removes   message = rnti << 16
 *   opcode 8  NSSAI MAP     arg = slice (0..7), 0xFF deletes   message = sst << 24 | sd (24 bits, 0xFFFFFF: no sd)
 *   opcode 9  SLICE DELETE  arg = slice (0..7)   message = 0
 *
 * The opcodes 1 to 4 have arg 0, so words built before the slice commands existed keep their meaning.
 *
 * The gNB turns a command into the text protocol of the local control endpoint
 * (nr_dl_ctrl_state.h) and sends it to that endpoint over loopback UDP, so the
 * E2 path and bin/sched-ctl.py reach the same state through the same code.
 *
 * Everything here is static inline and uses only the C library, so the header
 * can be included by the gNB's E2 agent and by an xApp without linking either
 * against the scheduler.
 */
#ifndef NR_DL_CTRL_E2_H
#define NR_DL_CTRL_E2_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Largest reply of the control endpoint, as in nr_dl_ctrl_state.h. */
#ifndef NR_CTRL_REPLY_SIZE
#define NR_CTRL_REPLY_SIZE 16384
#endif

#define NR_CTRL_E2_MAGIC 0x5343u

enum {
  NR_CTRL_E2_OP_POLICY = 1,
  NR_CTRL_E2_OP_WEIGHT = 2,
  NR_CTRL_E2_OP_CAP = 3,
  NR_CTRL_E2_OP_RESET = 4,
  NR_CTRL_E2_OP_SLICE_WEIGHT = 5,
  NR_CTRL_E2_OP_SLICE_CAP = 6,
  NR_CTRL_E2_OP_ASSOC = 7,
  NR_CTRL_E2_OP_NSSAI_MAP = 8,
  NR_CTRL_E2_OP_SLICE_DEL = 9,
};

#define NR_CTRL_E2_NO_SLICE 0xFFu
#define NR_CTRL_E2_MAX_SLICE 7u
#define NR_CTRL_E2_N_POLICY 5u

#define NR_CTRL_E2_MAX_WEIGHT 65535u
#define NR_CTRL_E2_MAX_CAP 275u
#define NR_CTRL_E2_MIN_CAP 5u

static inline const char *nr_ctrl_e2_policy_name(uint32_t id)
{
  static const char *const names[] = {"pf", "rr", "wshare", "wrr", "slice"};
  return id < NR_CTRL_E2_N_POLICY ? names[id] : NULL;
}

static inline int nr_ctrl_e2_policy_id(const char *name)
{
  for (uint32_t i = 0; i < NR_CTRL_E2_N_POLICY; i++)
    if (name && !strcmp(name, nr_ctrl_e2_policy_name(i)))
      return (int)i;
  return -1;
}

/* Parses a hexadecimal RNTI (1..ffff); returns -1 when invalid. */
static inline long nr_ctrl_e2_parse_rnti(const char *s)
{
  char *end = NULL;
  if (!s || !*s)
    return -1;
  unsigned long v = strtoul(s, &end, 16);
  return (*end || v == 0 || v > 0xffff) ? -1 : (long)v;
}

static inline long nr_ctrl_e2_parse_uint(const char *s, unsigned long max)
{
  char *end = NULL;
  if (!s || !*s)
    return -1;
  unsigned long v = strtoul(s, &end, 10);
  return (*end || v > max) ? -1 : (long)v;
}

/* Slice id (0..7) or "none" for the commands that can remove an association; -1 when invalid. */
static inline long nr_ctrl_e2_parse_slice_or_none(const char *s)
{
  if (s && !strcmp(s, "none"))
    return (long)NR_CTRL_E2_NO_SLICE;
  return nr_ctrl_e2_parse_uint(s, NR_CTRL_E2_MAX_SLICE);
}

/* Packs a command given as words (the xApp's command line).  Returns 0, or -1 with a
 * short reason in *err. */
static inline int nr_ctrl_e2_pack(const char *verb, const char *a1, const char *a2, const char *a3, uint32_t *hdr, uint32_t *act, const char **err)
{
  const char *dummy_err;
  if (!err)
    err = &dummy_err;
  if (!verb) {
    *err = "missing command";
    return -1;
  }
  if (!strcmp(verb, "policy")) {
    int id = nr_ctrl_e2_policy_id(a1);
    if (id < 0) {
      *err = "policy must be pf, rr, wrr, wshare or slice";
      return -1;
    }
    *hdr = (NR_CTRL_E2_MAGIC << 16) | NR_CTRL_E2_OP_POLICY;
    *act = (uint32_t)id;
    return 0;
  }
  if (!strcmp(verb, "weight")) {
    long rnti = (a1 && !strcmp(a1, "*")) ? 0 : nr_ctrl_e2_parse_rnti(a1);
    long w = nr_ctrl_e2_parse_uint(a2, NR_CTRL_E2_MAX_WEIGHT);
    if (rnti < 0) {
      *err = "weight needs a hexadecimal RNTI (or *)";
      return -1;
    }
    if (w < 1) {
      *err = "weight must be 1..65535";
      return -1;
    }
    *hdr = (NR_CTRL_E2_MAGIC << 16) | NR_CTRL_E2_OP_WEIGHT;
    *act = ((uint32_t)rnti << 16) | (uint32_t)w;
    return 0;
  }
  if (!strcmp(verb, "cap")) {
    long rnti = nr_ctrl_e2_parse_rnti(a1);
    long c = nr_ctrl_e2_parse_uint(a2, NR_CTRL_E2_MAX_CAP);
    if (rnti < 0) {
      *err = "cap needs a hexadecimal RNTI";
      return -1;
    }
    if (c < 0 || (c != 0 && c < (long)NR_CTRL_E2_MIN_CAP)) {
      *err = "cap must be 0 or 5..275";
      return -1;
    }
    *hdr = (NR_CTRL_E2_MAGIC << 16) | NR_CTRL_E2_OP_CAP;
    *act = ((uint32_t)rnti << 16) | (uint32_t)c;
    return 0;
  }
  if (!strcmp(verb, "reset")) {
    *hdr = (NR_CTRL_E2_MAGIC << 16) | NR_CTRL_E2_OP_RESET;
    *act = 0;
    return 0;
  }
  if (!strcmp(verb, "slice-weight") || !strcmp(verb, "slice-cap")) {
    int is_weight = !strcmp(verb, "slice-weight");
    long id = nr_ctrl_e2_parse_uint(a1, NR_CTRL_E2_MAX_SLICE);
    long v = nr_ctrl_e2_parse_uint(a2, is_weight ? NR_CTRL_E2_MAX_WEIGHT : NR_CTRL_E2_MAX_CAP);
    if (id < 0) {
      *err = "slice must be 0..7";
      return -1;
    }
    if (is_weight ? v < 1 : (v < 0 || (v != 0 && v < (long)NR_CTRL_E2_MIN_CAP))) {
      *err = is_weight ? "slice weight must be 1..65535" : "slice cap must be 0 or 5..275";
      return -1;
    }
    *hdr = (NR_CTRL_E2_MAGIC << 16) | ((uint32_t)id << 8) | (is_weight ? NR_CTRL_E2_OP_SLICE_WEIGHT : NR_CTRL_E2_OP_SLICE_CAP);
    *act = (uint32_t)v;
    return 0;
  }
  if (!strcmp(verb, "slice-del")) {
    long id = nr_ctrl_e2_parse_uint(a1, NR_CTRL_E2_MAX_SLICE);
    if (id < 0) {
      *err = "slice must be 0..7";
      return -1;
    }
    *hdr = (NR_CTRL_E2_MAGIC << 16) | ((uint32_t)id << 8) | NR_CTRL_E2_OP_SLICE_DEL;
    *act = 0;
    return 0;
  }
  if (!strcmp(verb, "assoc")) {
    long rnti = nr_ctrl_e2_parse_rnti(a1);
    long id = nr_ctrl_e2_parse_slice_or_none(a2);
    if (rnti < 0) {
      *err = "assoc needs a hexadecimal RNTI";
      return -1;
    }
    if (id < 0) {
      *err = "slice must be 0..7 or none";
      return -1;
    }
    *hdr = (NR_CTRL_E2_MAGIC << 16) | ((uint32_t)id << 8) | NR_CTRL_E2_OP_ASSOC;
    *act = (uint32_t)rnti << 16;
    return 0;
  }
  if (!strcmp(verb, "nssai-map")) {
    long sst = nr_ctrl_e2_parse_uint(a1, 255);
    long sd = -1;
    if (a2 && !strcmp(a2, "none")) {
      sd = 0xFFFFFF;
    } else if (a2 && *a2) {
      char *end = NULL;
      unsigned long v = strtoul(a2, &end, 16);
      if (!*end && v <= 0xFFFFFFul)
        sd = (long)v;
    }
    long id = nr_ctrl_e2_parse_slice_or_none(a3);
    if (sst < 0) {
      *err = "sst must be 0..255";
      return -1;
    }
    if (sd < 0) {
      *err = "sd must be hexadecimal up to ffffff, or none";
      return -1;
    }
    if (id < 0) {
      *err = "slice must be 0..7 or none";
      return -1;
    }
    *hdr = (NR_CTRL_E2_MAGIC << 16) | ((uint32_t)id << 8) | NR_CTRL_E2_OP_NSSAI_MAP;
    *act = ((uint32_t)sst << 24) | (uint32_t)sd;
    return 0;
  }
  *err = "command must be policy, weight, cap, reset, slice-weight, slice-cap, slice-del, assoc or nssai-map";
  return -1;
}

/* Turns the two words of a control message into the text command of the local control
 * endpoint.  Returns 0, or -1 when the words are not a valid scheduler command. */
static inline int nr_ctrl_e2_to_text(uint32_t hdr, uint32_t act, char *text, size_t size)
{
  if ((hdr >> 16) != NR_CTRL_E2_MAGIC)
    return -1;
  int n = -1;
  unsigned arg = (hdr >> 8) & 0xffu;
  switch (hdr & 0xffu) {
    case NR_CTRL_E2_OP_POLICY: {
      if (arg != 0)
        break;
      const char *name = nr_ctrl_e2_policy_name(act);
      if (name)
        n = snprintf(text, size, "POLICY %s", name);
      break;
    }
    case NR_CTRL_E2_OP_WEIGHT: {
      unsigned rnti = act >> 16, w = act & 0xffffu;
      if (arg == 0 && w >= 1) {
        if (rnti == 0)
          n = snprintf(text, size, "WEIGHT * %u", w);
        else
          n = snprintf(text, size, "WEIGHT %04x %u", rnti, w);
      }
      break;
    }
    case NR_CTRL_E2_OP_CAP: {
      unsigned rnti = act >> 16, c = act & 0xffffu;
      if (arg == 0 && rnti != 0 && (c == 0 || (c >= NR_CTRL_E2_MIN_CAP && c <= NR_CTRL_E2_MAX_CAP)))
        n = snprintf(text, size, "CAP %04x %u", rnti, c);
      break;
    }
    case NR_CTRL_E2_OP_RESET:
      if (arg == 0 && act == 0)
        n = snprintf(text, size, "RESET");
      break;
    case NR_CTRL_E2_OP_SLICE_WEIGHT:
      if (arg <= NR_CTRL_E2_MAX_SLICE && act >= 1 && act <= NR_CTRL_E2_MAX_WEIGHT)
        n = snprintf(text, size, "SLICE %u WEIGHT %u", arg, (unsigned)act);
      break;
    case NR_CTRL_E2_OP_SLICE_CAP:
      if (arg <= NR_CTRL_E2_MAX_SLICE && (act == 0 || (act >= NR_CTRL_E2_MIN_CAP && act <= NR_CTRL_E2_MAX_CAP)))
        n = snprintf(text, size, "SLICE %u CAP %u", arg, (unsigned)act);
      break;
    case NR_CTRL_E2_OP_SLICE_DEL:
      if (arg <= NR_CTRL_E2_MAX_SLICE && act == 0)
        n = snprintf(text, size, "SLICEDEL %u", arg);
      break;
    case NR_CTRL_E2_OP_ASSOC: {
      unsigned rnti = act >> 16;
      if (rnti != 0 && (act & 0xffffu) == 0 && (arg <= NR_CTRL_E2_MAX_SLICE || arg == NR_CTRL_E2_NO_SLICE)) {
        if (arg == NR_CTRL_E2_NO_SLICE)
          n = snprintf(text, size, "ASSOC %04x -", rnti);
        else
          n = snprintf(text, size, "ASSOC %04x %u", rnti, arg);
      }
      break;
    }
    case NR_CTRL_E2_OP_NSSAI_MAP:
      if (arg <= NR_CTRL_E2_MAX_SLICE || arg == NR_CTRL_E2_NO_SLICE) {
        unsigned sst = act >> 24, sd = act & 0xffffffu;
        if (arg == NR_CTRL_E2_NO_SLICE)
          n = snprintf(text, size, "NSSAIMAP %u %06x -", sst, sd);
        else
          n = snprintf(text, size, "NSSAIMAP %u %06x %u", sst, sd, arg);
      }
      break;
    default:
      break;
  }
  return (n > 0 && (size_t)n < size) ? 0 : -1;
}

/* Sends a text command to the local control endpoint (SCHED_CTRL_ADDR and
 * SCHED_CTRL_PORT, default 127.0.0.1:9099) and stores its reply.  Returns 0 when the
 * endpoint answered OK, -1 otherwise (refused, unreachable or no answer within 2 s). */
static inline int nr_ctrl_e2_send(const char *text, char *reply, size_t reply_size)
{
  const char *addr = getenv("SCHED_CTRL_ADDR");
  const char *port = getenv("SCHED_CTRL_PORT");
  if (!addr || !*addr)
    addr = "127.0.0.1";
  int p = (port && *port) ? atoi(port) : 9099;
  if (reply_size > 0)
    reply[0] = '\0';
  if (p <= 0 || p > 65535)
    return -1;

  struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)p)};
  if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1)
    return -1;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return -1;
  struct timeval tv = {.tv_sec = 2};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int rc = -1;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && send(fd, text, strlen(text), 0) >= 0) {
    char buf[NR_CTRL_REPLY_SIZE];
    ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
    if (r > 0) {
      buf[r] = '\0';
      if (reply_size > 0) {
        size_t n = (size_t)r < reply_size - 1 ? (size_t)r : reply_size - 1;
        memcpy(reply, buf, n);
        reply[n] = '\0';
      }
      rc = strncmp(buf, "OK", 2) == 0 ? 0 : -1;
    }
  }
  close(fd);
  return rc;
}

#endif /* NR_DL_CTRL_E2_H */
