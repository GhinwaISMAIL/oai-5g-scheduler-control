/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GNB_SCHEDULER_DLSCH_CTRL_POLICY_H
#define GNB_SCHEDULER_DLSCH_CTRL_POLICY_H

#include "LAYER2/NR_MAC_gNB/nr_mac_gNB.h"

/* Installs the controllable policy as mac->dl_rb_alloc and starts the control
 * endpoint.  Until a POLICY command arrives the behaviour is the stock
 * proportional-fair scheduler. */
void nr_dl_ctrl_policy_init(gNB_MAC_INST *mac);

int nr_dl_ctrl_policy(const nr_dl_sched_params_t *params, nr_dl_candidate_t *candidates, int n_candidates);

#endif /* GNB_SCHEDULER_DLSCH_CTRL_POLICY_H */
