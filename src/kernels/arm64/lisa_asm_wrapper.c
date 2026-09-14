#include "../../retrieval/retrieval.h"
#include <stdlib.h>
#include <string.h>

/*
 * Assembly kernel wrapper.
 *
 * The assembly kernel (lisa_search_ultra) processes dim in multiples of 4.
 * To support arbitrary dimensions, this wrapper:
 *   1. Pads each vector and the query to the next multiple of 4 (with zeros).
 *   2. Calls the assembly kernel on the padded data.
 *   3. Performs top-k selection.
 *
 * Zero-padding is safe for L2 squared distance: padded elements contribute
 * (0 - 0)^2 = 0 to the sum.
 */

extern void lisa_search_ultra(
    const float* query,
    const float* vectors,
    int n,
    int dim,
    int k,
    float* out_dists,
    int* out_indices
);

typedef struct {
    float dist;
    int index;
} heap_node_t;

static void heap_swap(heap_node_t* a, heap_node_t* b) {
    heap_node_t tmp = *a;
    *a = *b;
    *b = tmp;
}

static void heapify_up(heap_node_t* heap, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (heap[parent].dist < heap[idx].dist) {
            heap_swap(&heap[parent], &heap[idx]);
            idx = parent;
        } else {
            break;
        }
    }
}

static void heapify_down(heap_node_t* heap, int size, int idx) {
    while (1) {
        int left = idx * 2 + 1;
        int right = idx * 2 + 2;
        int largest = idx;
        if (left < size && heap[left].dist > heap[largest].dist) largest = left;
        if (right < size && heap[right].dist > heap[largest].dist) largest = right;
        if (largest == idx) break;
        heap_swap(&heap[idx], &heap[largest]);
        idx = largest;
    }
}

int lisa_search_exact_asm(
    const float* query,
    const float* vectors,
    int n,
    int dim,
    int k,
    lisa_result_t* result
) {
    if (query == NULL || vectors == NULL || result == NULL) return -1;
    if (n <= 0 || dim <= 0 || k <= 0) return -2;
    if (result->indices == NULL || result->dists == NULL) return -3;

    int k_eff = (k < n) ? k : n;

    /* Padded dimension: next multiple of 4 */
    int dim_padded = ((dim + 3) / 4) * 4;

    /* Allocate padded query */
    float* query_padded = malloc(dim_padded * sizeof(float));
    if (!query_padded) return -4;
    memset(query_padded, 0, dim_padded * sizeof(float));
    memcpy(query_padded, query, dim * sizeof(float));

    /* Allocate padded vectors */
    float* vectors_padded = malloc((size_t)n * dim_padded * sizeof(float));
    if (!vectors_padded) {
        free(query_padded);
        return -4;
    }

    /* Copy each vector, zero-padding the tail */
    for (int i = 0; i < n; i++) {
        float* dst = vectors_padded + (size_t)i * dim_padded;
        const float* src = vectors + (size_t)i * dim;
        memset(dst, 0, dim_padded * sizeof(float));
        memcpy(dst, src, dim * sizeof(float));
    }

    /* Allocate temporary distance/index arrays */
    float* all_dists = malloc(n * sizeof(float));
    int* all_indices = malloc(n * sizeof(int));
    if (!all_dists || !all_indices) {
        free(query_padded);
        free(vectors_padded);
        free(all_dists);
        free(all_indices);
        return -4;
    }

    /* Call assembly kernel on padded data */
    lisa_search_ultra(query_padded, vectors_padded, n, dim_padded, k_eff, all_dists, all_indices);

    /* Top-k selection */
    heap_node_t* heap = NULL;
    heap_node_t stack_heap[256];
    if (k_eff <= 256) {
        heap = stack_heap;
    } else {
        heap = malloc(k_eff * sizeof(heap_node_t));
        if (!heap) {
            free(query_padded);
            free(vectors_padded);
            free(all_dists);
            free(all_indices);
            return -4;
        }
    }

    int heap_size = 0;
    for (int i = 0; i < n; i++) {
        float dist = all_dists[i];
        if (heap_size < k_eff) {
            heap[heap_size].dist = dist;
            heap[heap_size].index = all_indices[i];
            heap_size++;
            heapify_up(heap, heap_size - 1);
        } else if (dist < heap[0].dist) {
            heap[0].dist = dist;
            heap[0].index = all_indices[i];
            heapify_down(heap, k_eff, 0);
        }
    }

    for (int i = k_eff - 1; i >= 0; i--) {
        result->indices[i] = heap[0].index;
        result->dists[i] = heap[0].dist;
        heap[0] = heap[heap_size - 1];
        heap_size--;
        if (heap_size > 0) heapify_down(heap, heap_size, 0);
    }

    result->n_returned = k_eff;

    if (k_eff > 256) free(heap);
    free(query_padded);
    free(vectors_padded);
    free(all_dists);
    free(all_indices);
    return 0;
}
