/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stand-in for FlexRIC's defer.h: runs the block when the enclosing scope ends.
 * Used only by the native test of the xApp.
 */
#ifndef FLEXRIC_STUB_DEFER_H
#define FLEXRIC_STUB_DEFER_H
#define DEFER_CAT_(a, b) a##b
#define DEFER_CAT(a, b) DEFER_CAT_(a, b)
#define defer(...)                                                                                  \
  auto void DEFER_CAT(defer_fn_, __LINE__)(int*);                                                   \
  int DEFER_CAT(defer_var_, __LINE__) __attribute__((cleanup(DEFER_CAT(defer_fn_, __LINE__)))) = 0; \
  void DEFER_CAT(defer_fn_, __LINE__)(__attribute__((unused)) int* defer_unused) __VA_ARGS__
#endif
