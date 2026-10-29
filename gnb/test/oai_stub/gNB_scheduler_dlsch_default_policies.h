#ifndef STUB_DEFAULT_POLICIES_H
#define STUB_DEFAULT_POLICIES_H
#include "LAYER2/NR_MAC_gNB/nr_mac_gNB.h"
int nr_dl_proportional_fair(const nr_dl_sched_params_t *params, nr_dl_candidate_t *candidates, int n_candidates);
#endif
