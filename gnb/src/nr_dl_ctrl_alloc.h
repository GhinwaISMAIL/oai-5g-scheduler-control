/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Weighted resource-block split used by the controllable downlink policy.
 * No dependency on the gNB sources, so it can be unit tested natively.
 */
#ifndef NR_DL_CTRL_ALLOC_H
#define NR_DL_CTRL_ALLOC_H

#include <stdint.h>

/* most users nr_ctrl_split() handles in one call */
#define NR_CTRL_SPLIT_MAX 64

/*
 * Split `total` contiguous resource blocks between `n` backlogged users.
 *
 * Inputs
 *   weight[i]  relative share of user i (0 is treated as 1)
 *   demand[i]  resource blocks user i can use this slot (values below
 *              min_rb are raised to min_rb)
 *   min_rb     smallest grant a user can receive
 *   total      free resource blocks
 * Output
 *   out[i]     0, or a value in [min_rb, demand[i]]
 *
 * Users are admitted in array order: when total / min_rb < n only the first
 * total / min_rb users receive anything, so the caller orders the array by
 * priority.  Among admitted users the result is the weighted max-min fair
 * split: each user receives weight[i] * level resource blocks, clamped between
 * min_rb and demand[i], with the level chosen so the grants add up to `total`
 * whenever the admitted demand is at least `total`.  Surplus left by users with
 * small demand is redistributed (work conserving).  Fractions are resolved
 * with the largest-remainder rule, ties going to the lower index.
 *
 * Returns the number of resource blocks granted (sum of out[]).
 */
int nr_ctrl_split(int n, const uint32_t *weight, const int *demand, int min_rb, int total, int *out);

#endif /* NR_DL_CTRL_ALLOC_H */
