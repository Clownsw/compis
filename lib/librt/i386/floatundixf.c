//===-- floatundixf.c - Implement __floatundixf ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __floatundixf for the compiler_rt library.
//
//===----------------------------------------------------------------------===//

#if !_ARCH_PPC

#include "int_lib.h"

// Returns: convert a to a long double, rounding toward even.

// Assumption: long double is a IEEE 80 bit floating point type padded to 128
// bits du_int is a 64 bit integral type

// gggg gggg gggg gggg gggg gggg gggg gggg | gggg gggg gggg gggg seee eeee eeee
// eeee | 1mmm mmmm mmmm mmmm mmmm mmmm mmmm mmmm | mmmm mmmm mmmm mmmm mmmm
// mmmm mmmm mmmm
COMPILER_RT_ABI long double __floatundixf(du_int a) {
  if (a == 0)
    return 0.0;
  const unsigned N = sizeof(du_int) * CHAR_BIT;
  int clz = __builtin_clzll(a);
  int e = (N - 1) - clz; // exponent
  long_double_bits fb;
  fb.u.high.s.low = (e + 16383); // exponent
  fb.u.low.all = a << clz;       // mantissa
  return fb.f;
}

#endif // _ARCH_PPC
