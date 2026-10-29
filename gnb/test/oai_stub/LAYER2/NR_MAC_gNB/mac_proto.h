#ifndef STUB_MAC_PROTO_H
#define STUB_MAC_PROTO_H
#include "nr_mac_gNB.h"
int find_largest_free_block(const uint16_t *vrb_map, uint16_t slbitmap, int bwp_start, int bwp_size, int *out_start);
bool get_rb_alloc(int rbSize_min, int rbSize_max, int bwpStart, int bwpSize, const uint16_t *vrb_map, uint16_t sym_mask, int *rbStart_ptr, int *rbSize_ptr);
bool commit_alloc(const nr_dl_sched_params_t *params, nr_dl_candidate_t *cand);
bool nr_find_nb_rb(uint16_t Qm, uint16_t R, long transform_precoding, uint8_t nrOfLayers, uint16_t nb_symb_sch, uint16_t nb_dmrs_prb,
                   uint32_t bytes, uint16_t nb_rb_min, uint16_t nb_rb_max, uint32_t *tbs, uint16_t *nb_rb);
NR_pdsch_dmrs_t get_dl_dmrs_params(const void *scc, const NR_UE_DL_BWP_t *dl_bwp, const NR_tda_info_t *tda_info, int Layers);
uint8_t nr_get_Qm_dl(uint8_t Imcs, uint8_t table_idx);
uint16_t nr_get_code_rate_dl(uint8_t Imcs, uint8_t table_idx);
#endif
