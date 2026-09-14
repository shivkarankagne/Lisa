#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../src/retrieval/retrieval.h"

/*
 * Differential test harness.
 *
 * Compares lisa_search_exact against a naive brute-force sort.
 * If they disagree, the test fails.
 */

typedef struct {
    float dist;
    int index;
} ref_result_t;

static int cmp_ref(const void* a, const void* b) {
    float da = ((const ref_result_t*)a)->dist;
    float db = ((const ref_result_t*)b)->dist;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/* Naive reference: compute all distances, sort, take top-k */
static void reference_topk(
    const float* query,
    const float* vectors,
    int n,
    int dim,
    int k,
    int* out_indices,
    float* out_dists
) {
    ref_result_t* all = malloc(n * sizeof(ref_result_t));
    for (int i = 0; i < n; i++) {
        all[i].index = i;
        all[i].dist = 0.0f;
        for (int j = 0; j < dim; j++) {
            float diff = query[j] - vectors[(size_t)i * dim + j];
            all[i].dist += diff * diff;
        }
    }
    qsort(all, n, sizeof(ref_result_t), cmp_ref);
    int k_eff = (k < n) ? k : n;
    for (int i = 0; i < k_eff; i++) {
        out_indices[i] = all[i].index;
        out_dists[i] = all[i].dist;
    }
    free(all);
}

static int test_case(int n, int dim, int k, unsigned seed) {
    srand(seed);

    float* vectors = malloc((size_t)n * dim * sizeof(float));
    float* query = malloc(dim * sizeof(float));
    for (int i = 0; i < n * dim; i++) {
        vectors[i] = (float)rand() / (float)RAND_MAX;
    }
    for (int i = 0; i < dim; i++) {
        query[i] = (float)rand() / (float)RAND_MAX;
    }

    int k_eff = (k < n) ? k : n;

    int* lisa_idx = malloc(k_eff * sizeof(int));
    float* lisa_dist = malloc(k_eff * sizeof(float));
    int* ref_idx = malloc(k_eff * sizeof(int));
    float* ref_dist = malloc(k_eff * sizeof(float));

    lisa_result_t result = {
        .indices = lisa_idx,
        .dists = lisa_dist,
        .k = k_eff,
        .n_returned = 0
    };

    int rc = lisa_search_exact(query, vectors, n, dim, k, &result);
    if (rc != 0) {
        printf("FAIL: lisa_search_exact returned %d\n", rc);
        free(vectors); free(query);
        free(lisa_idx); free(lisa_dist);
        free(ref_idx); free(ref_dist);
        return 1;
    }

    reference_topk(query, vectors, n, dim, k, ref_idx, ref_dist);

    /* Compare */
    for (int i = 0; i < k_eff; i++) {
        float diff = fabsf(lisa_dist[i] - ref_dist[i]);
        if (diff > 1e-4f) {
            printf("FAIL: n=%d dim=%d k=%d seed=%u\n", n, dim, k, seed);
            printf("  position %d: lisa=%.6f ref=%.6f\n",
                   i, lisa_dist[i], ref_dist[i]);
            free(vectors); free(query);
            free(lisa_idx); free(lisa_dist);
            free(ref_idx); free(ref_dist);
            return 1;
        }
    }

    free(vectors); free(query);
    free(lisa_idx); free(lisa_dist);
    free(ref_idx); free(ref_dist);
    return 0;
}

int main(void) {
    int failures = 0;

    /* Basic cases */
    failures += test_case(10, 4, 3, 1);
    failures += test_case(100, 8, 5, 2);
    failures += test_case(1000, 16, 10, 3);
    failures += test_case(10000, 64, 5, 4);

    /* Edge cases */
    failures += test_case(1, 4, 1, 5);        /* n=1 */
    failures += test_case(5, 4, 5, 6);        /* k=n */
    failures += test_case(5, 4, 10, 7);       /* k>n */
    failures += test_case(100, 3, 1, 8);      /* k=1 */
    failures += test_case(100, 128, 20, 9);   /* larger dim */
    failures += test_case(100, 768, 5, 10);   /* realistic dim */

    /* Random cases */
    for (int t = 0; t < 20; t++) {
        int n = 1 + rand() % 500;
        int dim = 1 + rand() % 256;
        int k = 1 + rand() % 20;
        failures += test_case(n, dim, k, 100 + t);
    }

    if (failures == 0) {
        printf("PASS: all tests passed\n");
        return 0;
    } else {
        printf("FAIL: %d test(s) failed\n", failures);
        return 1;
    }
}
