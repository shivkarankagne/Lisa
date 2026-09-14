#include "retrieval.h"
#include <stdlib.h>
#include <string.h>
#include <float.h>

/*
 * LISA Retrieval — scalar reference implementation
 *
 * This is the trusted reference. All optimized implementations must
 * produce identical results (up to floating-point tolerance).
 *
 * No SIMD. No assembly. Just correct C.
 */

float lisa_l2_squared(const float* a, const float* b, int dim) {
    float sum = 0.0f;
    for (int i = 0; i < dim; i++) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

/*
 * Max-heap for top-k selection.
 *
 * We keep the k smallest distances seen so far. The heap's root is the
 * largest of those k — so a new candidate only enters if it is smaller
 * than the root.
 */
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

        if (left < size && heap[left].dist > heap[largest].dist) {
            largest = left;
        }
        if (right < size && heap[right].dist > heap[largest].dist) {
            largest = right;
        }
        if (largest == idx) {
            break;
        }
        heap_swap(&heap[idx], &heap[largest]);
        idx = largest;
    }
}

int lisa_search_exact(
    const float* query,
    const float* vectors,
    int n,
    int dim,
    int k,
    lisa_result_t* result
) {
    /* Input validation */
    if (query == NULL || vectors == NULL || result == NULL) {
        return -1;
    }
    if (n <= 0 || dim <= 0 || k <= 0) {
        return -2;
    }
    if (result->indices == NULL || result->dists == NULL) {
        return -3;
    }

    /* Clamp k to n */
    int k_eff = (k < n) ? k : n;

    /* Allocate heap on stack for small k, heap for large k */
    heap_node_t* heap = NULL;
    heap_node_t stack_heap[256];
    if (k_eff <= 256) {
        heap = stack_heap;
    } else {
        heap = (heap_node_t*)malloc(k_eff * sizeof(heap_node_t));
        if (heap == NULL) {
            return -4;
        }
    }

    int heap_size = 0;

    /* Scan all vectors */
    for (int i = 0; i < n; i++) {
        const float* vec = vectors + (size_t)i * dim;
        float dist = lisa_l2_squared(query, vec, dim);

        if (heap_size < k_eff) {
            heap[heap_size].dist = dist;
            heap[heap_size].index = i;
            heap_size++;
            heapify_up(heap, heap_size - 1);
        } else if (dist < heap[0].dist) {
            heap[0].dist = dist;
            heap[0].index = i;
            heapify_down(heap, k_eff, 0);
        }
    }

    /*
     * Extract results in ascending order.
     *
     * The heap is a max-heap, so the root is the largest. We repeatedly
     * remove the root and place it at the end of the output, then
     * shrink the heap. This produces ascending order.
     */
    for (int i = k_eff - 1; i >= 0; i--) {
        result->indices[i] = heap[0].index;
        result->dists[i] = heap[0].dist;
        heap[0] = heap[heap_size - 1];
        heap_size--;
        if (heap_size > 0) {
            heapify_down(heap, heap_size, 0);
        }
    }

    result->n_returned = k_eff;

    if (k_eff > 256) {
        free(heap);
    }

    return 0;
}
