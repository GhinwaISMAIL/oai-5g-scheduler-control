/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nr_dl_ctrl_alloc.h"

#include <stddef.h>

#define CTRL_MAX_USERS NR_CTRL_SPLIT_MAX

static double clamp_d(double v, double lo, double hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

int nr_ctrl_split(int n, const uint32_t *weight, const int *demand, int min_rb, int total, int *out)
{
  if (n <= 0)
    return 0;
  if (n > CTRL_MAX_USERS)
    n = CTRL_MAX_USERS;
  for (int i = 0; i < n; i++)
    out[i] = 0;
  if (min_rb < 1 || total < min_rb)
    return 0;

  int m = total / min_rb;
  if (m > n)
    m = n;

  double w[CTRL_MAX_USERS];
  int hi[CTRL_MAX_USERS];
  long sum_hi = 0;
  double wmax_ratio = 0.0;
  for (int i = 0; i < m; i++) {
    w[i] = weight[i] ? (double)weight[i] : 1.0;
    hi[i] = demand[i] < min_rb ? min_rb : demand[i];
    sum_hi += hi[i];
    double r = hi[i] / w[i];
    if (r > wmax_ratio)
      wmax_ratio = r;
  }

  if (sum_hi <= total) {
    int sum = 0;
    for (int i = 0; i < m; i++) {
      out[i] = hi[i];
      sum += hi[i];
    }
    return sum;
  }

  /* Level lambda such that sum(clamp(lambda * w, min_rb, hi)) = total.
   * f(0) = m * min_rb <= total < sum_hi = f(wmax_ratio), and f is
   * non-decreasing, so bisection converges.  The lower end is kept so the
   * rounded sum never exceeds total. */
  double lo_l = 0.0;
  double hi_l = wmax_ratio;
  for (int it = 0; it < 100; it++) {
    double mid = 0.5 * (lo_l + hi_l);
    double f = 0.0;
    for (int i = 0; i < m; i++)
      f += clamp_d(mid * w[i], (double)min_rb, (double)hi[i]);
    if (f <= (double)total)
      lo_l = mid;
    else
      hi_l = mid;
  }

  double frac[CTRL_MAX_USERS];
  int granted = 0;
  for (int i = 0; i < m; i++) {
    double x = clamp_d(lo_l * w[i], (double)min_rb, (double)hi[i]);
    int base = (int)x;
    if (base < min_rb)
      base = min_rb;
    if (base > hi[i])
      base = hi[i];
    out[i] = base;
    frac[i] = x - (double)base;
    granted += base;
  }

  /* largest remainder: one resource block at a time to the user with the
   * largest fractional part that still has headroom */
  while (granted < total) {
    int best = -1;
    for (int i = 0; i < m; i++) {
      if (out[i] >= hi[i])
        continue;
      if (best < 0 || frac[i] > frac[best])
        best = i;
    }
    if (best < 0)
      break;
    out[best]++;
    frac[best] = -1.0; /* each user gets another block only after all others in this pass */
    granted++;
    int any = 0;
    for (int i = 0; i < m; i++)
      if (out[i] < hi[i] && frac[i] >= 0.0)
        any = 1;
    if (!any)
      for (int i = 0; i < m; i++)
        if (out[i] < hi[i])
          frac[i] = 0.0;
  }
  return granted;
}
