/* SPDX-License-Identifier: Apache-2.0 */
/*
 * LISA Retrieval — production exact search.
 *
 * Computes distances through the kernel registry in fixed-size blocks and
 * feeds them into a bounded max-heap. Memory use is O(k), independent of
 * n: the dataset is never copied.
 *
 * Tie-breaking matches the scalar reference: vectors are visited in index
 * order, a candidate replaces the heap root only if strictly closer, and
 * results are extracted the same way. The order among exactly equal
 * distances is therefore identical to lisa_search_exact (it is not index
 * order: heap extraction is not stable).
 */

#include "retrieval.h"
#include "../kernels/kernels.h"

#include <stdint.h>
#include <stdlib.h>

#define BLOCK 256

typedef struct {
    float dist;
    int index;
} node_t;

static void sift_up(node_t* h, int64_t i) {
    while (i > 0) {
        int64_t p = (i - 1) / 2;
        if (h[p].dist >= h[i].dist) break;
        node_t t = h[p]; h[p] = h[i]; h[i] = t;
        i = p;
    }
}

static void sift_down(node_t* h, int64_t size, int64_t i) {
    for (;;) {
        int64_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < size && h[l].dist > h[m].dist) m = l;
        if (r < size && h[r].dist > h[m].dist) m = r;
        if (m == i) break;
        node_t t = h[m]; h[m] = h[i]; h[i] = t;
        i = m;
    }
}

int lisa_search(
    const float* query,
    const float* vectors,
    int64_t n,
    int64_t dim,
    int64_t k,
    lisa_result_t* result
) {
    return lisa_search_masked(query, vectors, n, dim, k, NULL, result);
}

int lisa_search_masked(
    const float* query,
    const float* vectors,
    int64_t n,
    int64_t dim,
    int64_t k,
    const uint8_t* live,
    lisa_result_t* result
) {
    if (query == NULL || vectors == NULL || result == NULL) return -1;
    if (n <= 0 || dim <= 0 || k <= 0) return -2;
    if (n > INT32_MAX) return -2;  /* result indices are int */
    if (result->indices == NULL || result->dists == NULL) return -3;

    int64_t candidates = n;
    if (live != NULL) {
        candidates = 0;
        for (int64_t i = 0; i < n; i++) candidates += live[i] ? 1 : 0;
        if (candidates == 0) {
            result->n_returned = 0;
            return 0;
        }
    }
    int64_t k_eff = (k < candidates) ? k : candidates;

    node_t stack_heap[256];
    node_t* heap = stack_heap;
    if (k_eff > 256) {
        heap = (node_t*)malloc((size_t)k_eff * sizeof(node_t));
        if (heap == NULL) return -4;
    }

    lisa_l2_batch_fn l2_batch = lisa_kernels()->l2_batch;
    float dists[BLOCK];
    int64_t size = 0;

    for (int64_t base = 0; base < n; base += BLOCK) {
        int64_t count = (n - base < BLOCK) ? n - base : BLOCK;
        l2_batch(query, vectors + base * dim, count, dim, dists);

        for (int64_t j = 0; j < count; j++) {
            if (live != NULL && !live[base + j]) continue;
            float d = dists[j];
            if (size < k_eff) {
                heap[size].dist = d;
                heap[size].index = (int)(base + j);
                sift_up(heap, size);
                size++;
            } else if (d < heap[0].dist) {
                heap[0].dist = d;
                heap[0].index = (int)(base + j);
                sift_down(heap, size, 0);
            }
        }
    }

    /* Pop the max-heap into the output from the back: ascending order. */
    for (int64_t i = k_eff - 1; i >= 0; i--) {
        result->indices[i] = heap[0].index;
        result->dists[i] = heap[0].dist;
        heap[0] = heap[size - 1];
        size--;
        if (size > 0) sift_down(heap, size, 0);
    }
    result->n_returned = (int)k_eff;

    if (heap != stack_heap) free(heap);
    return 0;
}
