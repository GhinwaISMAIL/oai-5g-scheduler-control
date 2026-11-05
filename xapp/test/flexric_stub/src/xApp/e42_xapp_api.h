/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stand-in for the part of FlexRIC's xApp interface (src/xApp/e42_xapp_api.h and the
 * headers it includes) that gnb-side tools use: the signatures are those of the pinned
 * revision, the slice types are those of src/sm/slice_sm/ie/slice_data_ie.h as in
 * gnb/test/flexric_stub. Used only by the native test of xapp/xapp_sched_ctrl.c, which
 * links it with xapp/test/stub_api.c instead of the RIC.
 */
#ifndef FLEXRIC_STUB_E42_XAPP_API_H
#define FLEXRIC_STUB_E42_XAPP_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openair2/E2AP/flexric/src/sm/sm_io.h"

typedef enum { ngran_eNB = 0, ngran_gNB = 2, ngran_gNB_DU = 4 } ngran_node_t;

typedef struct {
  ngran_node_t type;
} global_e2_node_id_t;

typedef struct {
  uint16_t id;
} sm_ran_function_t;

typedef struct {
  global_e2_node_id_t id;
  size_t len_rf;
  sm_ran_function_t* rf;
} e2_node_connected_xapp_t;

typedef struct {
  uint8_t len;
  e2_node_connected_xapp_t* n;
} e2_node_arr_xapp_t;

typedef struct {
  int argc;
} fr_args_t;

typedef struct {
  uint32_t dummy;
} mac_ctrl_hdr_t;
typedef struct {
  uint32_t action;
} mac_ctrl_msg_t;
typedef struct {
  mac_ctrl_hdr_t hdr;
  mac_ctrl_msg_t msg;
} mac_ctrl_req_data_t;

typedef enum { INDICATION_MSG_AGENT_IF_ANS_V0 = 1 } sm_ag_if_rd_e;
typedef enum { SLICE_STATS_V0 = 1 } sm_ind_e;
typedef struct {
  sm_ind_e type;
  slice_ind_data_t slice;
} sm_ind_t;
typedef struct {
  sm_ag_if_rd_e type;
  sm_ind_t ind;
} sm_ag_if_rd_t;

typedef void (*sm_cb)(sm_ag_if_rd_t const*);

typedef union {
  char* reason;
  int handle;
} sm_ans_xapp_u;
typedef struct {
  sm_ans_xapp_u u;
  bool success;
} sm_ans_xapp_t;

fr_args_t init_fr_args(int argc, char* argv[]);
void init_xapp_api(fr_args_t const*);
bool try_stop_xapp_api(void);
e2_node_arr_xapp_t e2_nodes_xapp_api(void);
void free_e2_node_arr_xapp(e2_node_arr_xapp_t*);
sm_ans_xapp_t report_sm_xapp_api(global_e2_node_id_t* id, uint32_t rf_id, void* data, sm_cb handler);
void rm_report_sm_xapp_api(int const handle);
sm_ans_xapp_t control_sm_xapp_api(global_e2_node_id_t* id, uint32_t rf_id, void* wr);

void free_slice_ctrl_msg(slice_ctrl_msg_t* src);
slice_ind_msg_t cp_slice_ind_msg(slice_ind_msg_t const* src);
void free_slice_ind_msg(slice_ind_msg_t* src);

#endif
