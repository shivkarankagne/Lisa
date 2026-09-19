/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Reciprocal rank fusion. Inputs are short (top candidates of each
 * list), so a simple quadratic merge is fine and keeps the order stable.
 */

#include "fusion.h"

#include <stdlib.h>

static int64_t find(const lisa_fused_t* items, int64_t n, uint64_t id) {
    for (int64_t i = 0; i < n; i++) {
        if (items[i].id == id) return i;
    }
    return -1;
}

int64_t lisa_rrf_fuse(const uint64_t* a, int64_t na, double weight_a,
                      const uint64_t* b, int64_t nb, double weight_b,
                      lisa_fused_t* out, int64_t capacity) {
    if (capacity <= 0 || (na > 0 && a == NULL) || (nb > 0 && b == NULL) || na < 0 || nb < 0) return 0;
    lisa_fused_t* all = (lisa_fused_t*)malloc((size_t)(na + nb + 1) * sizeof(lisa_fused_t));
    if (all == NULL) return -1;
    int64_t n = 0;

    for (int64_t i = 0; i < na; i++) {
        if (find(all, n, a[i]) >= 0) continue;
        all[n].id = a[i];
        all[n].score = weight_a / (LISA_RRF_K + (double)(i + 1));
        all[n].rank_a = (int32_t)(i + 1);
        all[n].rank_b = 0;
        n++;
    }
    for (int64_t i = 0; i < nb; i++) {
        int64_t at = find(all, n, b[i]);
        if (at >= 0) {
            if (all[at].rank_b == 0) {
                all[at].score += weight_b / (LISA_RRF_K + (double)(i + 1));
                all[at].rank_b = (int32_t)(i + 1);
            }
            continue;
        }
        all[n].id = b[i];
        all[n].score = weight_b / (LISA_RRF_K + (double)(i + 1));
        all[n].rank_a = 0;
        all[n].rank_b = (int32_t)(i + 1);
        n++;
    }

    /* Stable insertion sort by score, descending (n is small). */
    for (int64_t i = 1; i < n; i++) {
        lisa_fused_t x = all[i];
        int64_t j = i - 1;
        while (j >= 0 && all[j].score < x.score) {
            all[j + 1] = all[j];
            j--;
        }
        all[j + 1] = x;
    }
    int64_t m = n < capacity ? n : capacity;
    for (int64_t i = 0; i < m; i++) out[i] = all[i];
    free(all);
    return m;
}
