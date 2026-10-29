#ifndef STUB_NR_MAC_GNB_H
#define STUB_NR_MAC_GNB_H
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef MAX_MOBILES_PER_GNB
#define MAX_MOBILES_PER_GNB 32
#endif
#define MAX_NUM_BEAM_PERIODS 4
#define NR_MAX_NUM_LCID 32
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

typedef int frame_t;
typedef int slot_t;
typedef uint16_t rnti_t;
typedef int uid_t_stub;
typedef enum { PDSCH_TYPE0 = 0, PDSCH_TYPE1 = 1 } pdsch_alloc_type_t;

#define AssertFatal(cond, ...)           \
  do {                                   \
    if (!(cond)) {                       \
      fprintf(stderr, "AssertFatal\n");  \
      abort();                           \
    }                                    \
  } while (0)

#define NR_SCHED_LOCK(lock)                                        \
  do {                                                             \
    int rc = pthread_mutex_lock(lock);                             \
    AssertFatal(rc == 0, "error while locking scheduler mutex, pthread_mutex_lock() returned %d\n", rc); \
  } while (0)

#define NR_SCHED_UNLOCK(lock)                                      \
  do {                                                             \
    int rc = pthread_mutex_unlock(lock);                           \
    AssertFatal(rc == 0, "error while locking scheduler mutex, pthread_mutex_unlock() returned %d\n", rc); \
  } while (0)

typedef struct {
  uint64_t total_sdu_bytes;
  uint32_t total_rbs;
  uint32_t total_rbs_retx;
} NR_mac_dir_stats_t;
typedef struct {
  NR_mac_dir_stats_t dl;
  NR_mac_dir_stats_t ul;
} NR_mac_stats_t;

typedef struct {
  int startSymbolIndex;
  int nrOfSymbols;
} NR_tda_info_t;
typedef struct {
  uint8_t N_PRB_DMRS;
  uint8_t N_DMRS_SLOT;
} NR_pdsch_dmrs_t;
typedef struct {
  int alloc_type;
  uint16_t rbStart;
  uint16_t rbSize;
  uint8_t mcs;
  uint8_t nrOfLayers;
  NR_tda_info_t tda_info;
} NR_sched_pdsch_t;
typedef struct {
  bool ta_apply;
} NR_UE_sched_ctrl_t;
typedef struct {
  int BWPSize;
} NR_UE_DL_BWP_t;
typedef struct {
  rnti_t rnti;
  NR_UE_sched_ctrl_t UE_sched_ctrl;
  NR_UE_DL_BWP_t current_DL_BWP;
  NR_mac_stats_t mac_stats;
} NR_UE_info_t;
typedef struct {
  NR_UE_info_t *connected_ue_list[MAX_MOBILES_PER_GNB + 1];
} NR_UEs_t;
#define UE_iterator(BaSe, VaR) for (NR_UE_info_t **VaR##pptr = BaSe, *VaR = *VaR##pptr; VaR; VaR = *(++VaR##pptr))
#define FOR_EACH_CANDIDATE(VaR, ArR, N) for (__typeof__(*(ArR)) *VaR = (ArR); VaR < (ArR) + (N); VaR++)

typedef struct {
  void *ServingCellConfigCommon;
} NR_COMMON_channels_t;

struct gNB_MAC_INST_s;
typedef struct gNB_MAC_INST_s gNB_MAC_INST;
typedef struct nr_dl_candidate nr_dl_candidate_t;
typedef struct nr_dl_sched_params nr_dl_sched_params_t;
struct nr_dl_sched_params {
  struct gNB_MAC_INST_s *mac;
  int CC_id;
  frame_t frame;
  slot_t slot;
  int num_beams;
  int max_num_ue;
  uint16_t *vrb_map[MAX_NUM_BEAM_PERIODS];
  int n_rb_avail[MAX_NUM_BEAM_PERIODS];
};
struct nr_dl_candidate {
  NR_UE_info_t *UE;
  uint16_t rnti;
  bool is_retx;
  int8_t retx_harq_pid;
  int retx_rbSize;
  uint32_t pending_bytes;
  int mcs_table;
  int bwp_start;
  int bwp_size;
  bool skipped;
  bool scheduled;
  NR_sched_pdsch_t sched_pdsch;
  uint16_t alloc_slbitmap;
  int alloc_beam_idx;
};
typedef int (*nr_dl_rb_alloc_fn)(const nr_dl_sched_params_t *params, nr_dl_candidate_t *candidates, int n_candidates);
struct gNB_MAC_INST_s {
  NR_COMMON_channels_t common_channels[1];
  NR_UEs_t UE_info;
  nr_dl_rb_alloc_fn dl_rb_alloc;
  pthread_mutex_t sched_lock;
};
#endif
