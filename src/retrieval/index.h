/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_INDEX_H
#define LISA_INDEX_H

/*
 * Vector index seam (plan §7). Callers search through a lisa_index_ops_t,
 * never a specific algorithm, so an approximate index (L4: HNSW / IVF)
 * can be added without changing them. The exact index is the only
 * implementation in 1.0.
 */

#include <stdint.h>

#include "retrieval.h"

typedef struct {
    const char* name;
    /*
     * Top-k nearest vectors among n rows (dim floats each) whose mask
     * entry is non-zero (mask may be NULL for all). Same result contract
     * and errors as lisa_search_masked.
     */
    int (*search)(const float* query, const float* vectors, int64_t n, int64_t dim,
                  int64_t k, const uint8_t* mask, lisa_result_t* result);
} lisa_index_ops_t;

/* Exact search through the kernel registry. */
const lisa_index_ops_t* lisa_index_exact(void);

#endif /* LISA_INDEX_H */
