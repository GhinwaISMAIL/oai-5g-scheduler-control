/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Declarations of the slice handler, as in OAI's ran_func_slice.h, for the native test.
 */
#ifndef RAN_FUNC_SM_SLICE_READ_WRITE_AGENT_H
#define RAN_FUNC_SM_SLICE_READ_WRITE_AGENT_H

#include "openair2/E2AP/flexric/src/sm/sm_io.h"

bool read_slice_sm(void*);

void read_slice_setup_sm(void* data);

sm_ag_if_ans_t write_ctrl_slice_sm(void const* data);

#endif
