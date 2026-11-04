/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stand-in for the part of the FlexRIC agent interface that the slice handler
 * (gnb/patches/0004-e2-slice-control.patch) uses: the slice service model types
 * of src/sm/slice_sm/ie/slice_data_ie.h with the same fields and layout, and the
 * control answer.  Used only by the native test of the handler; the gNB build
 * uses the FlexRIC headers.
 */
#ifndef FLEXRIC_STUB_SM_IO_H
#define FLEXRIC_STUB_SM_IO_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { SLICE_ALG_SM_V0_NONE = 0, SLICE_ALG_SM_V0_STATIC = 1, SLICE_ALG_SM_V0_NVS = 2, SLICE_ALG_SM_V0_SCN19 = 3, SLICE_ALG_SM_V0_EDF = 4 } slice_algorithm_e;

typedef struct {
  uint32_t pos_low;
  uint32_t pos_high;
} static_slice_t;

typedef enum { SLICE_SM_NVS_V0_RATE, SLICE_SM_NVS_V0_CAPACITY } nvs_slice_conf_e;
typedef union {
  uint8_t mbps_required_byte[4];
  float mbps_required;
} nvs_rate_req_u;
typedef union {
  uint8_t mbps_reference_byte[4];
  float mbps_reference;
} nvs_rate_ref_u;
typedef struct {
  nvs_rate_req_u u1;
  nvs_rate_ref_u u2;
} nvs_rate_t;
typedef union {
  uint8_t pct_reserved_byte[4];
  float pct_reserved;
} nvs_capacity_u;
typedef struct {
  nvs_capacity_u u;
} nvs_capacity_t;
typedef union {
  nvs_rate_t rate;
  nvs_capacity_t capacity;
} nvs_slice_u;
typedef struct {
  nvs_slice_conf_e conf;
  nvs_slice_u u;
} nvs_slice_t;

typedef struct {
  uint32_t deadline;
  uint32_t guaranteed_prbs;
  uint32_t max_replenish;
  uint32_t len_over;
  uint32_t* over;
} edf_slice_t;

typedef union {
  static_slice_t sta;
  nvs_slice_t nvs;
  edf_slice_t edf;
} slice_params_u;
typedef struct {
  slice_algorithm_e type;
  slice_params_u u;
} slice_params_t;

typedef struct {
  uint32_t id;
  uint32_t len_label;
  char* label;
  uint32_t len_sched;
  char* sched;
  slice_params_t params;
} fr_slice_t;

typedef struct {
  uint32_t len_slices;
  fr_slice_t* slices;
  uint32_t len_sched_name;
  char* sched_name;
} ul_dl_slice_conf_t;
typedef struct {
  ul_dl_slice_conf_t dl;
  ul_dl_slice_conf_t ul;
} slice_conf_t;

typedef struct {
  uint32_t dl_id;
  uint32_t ul_id;
  uint16_t rnti;
} ue_slice_assoc_t;
typedef struct {
  uint32_t len_ue_slice;
  ue_slice_assoc_t* ues;
} ue_slice_conf_t;

typedef struct {
  slice_conf_t slice_conf;
  ue_slice_conf_t ue_slice_conf;
  int64_t tstamp;
} slice_ind_msg_t;
typedef struct {
  uint32_t dummy;
} slice_ind_hdr_t;
typedef struct {
  uint32_t dummy;
} slice_call_proc_id_t;
typedef struct {
  slice_ind_hdr_t hdr;
  slice_ind_msg_t msg;
  slice_call_proc_id_t* proc_id;
} slice_ind_data_t;

typedef enum { SLICE_CTRL_SM_V0_ADD, SLICE_CTRL_SM_V0_DEL, SLICE_CTRL_SM_V0_UE_SLICE_ASSOC } slice_ctrl_msg_e;
typedef struct {
  uint32_t len_dl;
  uint32_t* dl;
  uint32_t len_ul;
  uint32_t* ul;
} del_slice_conf_t;
typedef union {
  slice_conf_t add_mod_slice;
  del_slice_conf_t del_slice;
  ue_slice_conf_t ue_slice;
} slice_ctrl_msg_u;
typedef struct {
  slice_ctrl_msg_e type;
  slice_ctrl_msg_u u;
} slice_ctrl_msg_t;
typedef struct {
  uint32_t dummy;
} slice_ctrl_hdr_t;
typedef struct {
  slice_ctrl_hdr_t hdr;
  slice_ctrl_msg_t msg;
} slice_ctrl_req_data_t;

typedef enum { CTRL_OUTCOME_SM_AG_IF_ANS_V0 = 0 } sm_ag_if_ans_e;
enum { SLICE_AGENT_IF_CTRL_ANS_V0 = 1 };
typedef struct {
  sm_ag_if_ans_e type;
  struct {
    int type;
  } ctrl_out;
} sm_ag_if_ans_t;

#endif
