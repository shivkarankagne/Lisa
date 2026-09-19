/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_ASM_H
#define LISA_ASM_H

#include "../../retrieval/retrieval.h"

/*
 * Exact top-k search using the ARM64 NEON assembly kernel
 * (lisa_ultra_mac.s). Same contract and error codes as
 * lisa_search_exact() in retrieval.h, plus:
 *
 *   -4: allocation failure
 *
 * Arbitrary dimensions are supported by zero-padding to a multiple of 4.
 */
int lisa_search_exact_asm(
    const float* query,
    const float* vectors,
    int n,
    int dim,
    int k,
    lisa_result_t* result
);

#endif /* LISA_ASM_H */
