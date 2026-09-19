/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_FUSION_H
#define LISA_FUSION_H

/*
 * Reciprocal rank fusion (RRF) of ranked result lists.
 *
 * Each input list is ranked best-first. An item's fused score is
 *   sum over lists of  weight_list / (k + rank_in_list)
 * with 1-based ranks. Items missing from a list get nothing from it.
 * Standard k = 60 (Cormack et al., 2009).
 */

#include <stdint.h>

#define LISA_RRF_K 60.0

typedef struct {
    uint64_t id;
    double   score;       /* fused score, higher is better */
    int32_t  rank_a;      /* 1-based rank in list A; 0 if absent */
    int32_t  rank_b;      /* 1-based rank in list B; 0 if absent */
} lisa_fused_t;

/*
 * Fuse two ranked ID lists (duplicates within a list: first occurrence
 * counts). Writes up to `capacity` fused items, best first, into out;
 * ties keep list A's order, then list B's. Returns the number written,
 * or -1 on allocation failure.
 */
int64_t lisa_rrf_fuse(const uint64_t* a, int64_t na, double weight_a,
                      const uint64_t* b, int64_t nb, double weight_b,
                      lisa_fused_t* out, int64_t capacity);

#endif /* LISA_FUSION_H */
