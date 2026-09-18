#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../src/retrieval/retrieval.h"

#include "../src/kernels/arm64/lisa_asm.h"

static int compare(int n, int dim, int k, unsigned seed) {
    srand(seed);

    float* vectors = malloc((size_t)n * dim * sizeof(float));
    float* query = malloc(dim * sizeof(float));
    for (int i = 0; i < n * dim; i++) vectors[i] = (float)rand() / (float)RAND_MAX;
    for (int i = 0; i < dim; i++) query[i] = (float)rand() / (float)RAND_MAX;

    int k_eff = (k < n) ? k : n;
    int* idx_s = malloc(k_eff * sizeof(int));
    float* dist_s = malloc(k_eff * sizeof(float));
    int* idx_a = malloc(k_eff * sizeof(int));
    float* dist_a = malloc(k_eff * sizeof(float));

    lisa_result_t r_s = { .indices = idx_s, .dists = dist_s, .k = k_eff, .n_returned = 0 };
    lisa_result_t r_a = { .indices = idx_a, .dists = dist_a, .k = k_eff, .n_returned = 0 };

    int rc_s = lisa_search_exact(query, vectors, n, dim, k, &r_s);
    int rc_a = lisa_search_exact_asm(query, vectors, n, dim, k, &r_a);

    int fail = 0;
    if (rc_s != 0 || rc_a != 0) {
        printf("FAIL: n=%d dim=%d k=%d seed=%u scalar_rc=%d asm_rc=%d\n",
               n, dim, k, seed, rc_s, rc_a);
        fail = 1;
    } else {
        for (int i = 0; i < k_eff; i++) {
            float diff = fabsf(dist_s[i] - dist_a[i]);
            if (diff > 1e-3f) {
                printf("FAIL: n=%d dim=%d k=%d seed=%u pos=%d scalar=%.6f asm=%.6f\n",
                       n, dim, k, seed, i, dist_s[i], dist_a[i]);
                fail = 1;
                break;
            }
        }
    }

    free(vectors); free(query);
    free(idx_s); free(dist_s);
    free(idx_a); free(dist_a);
    return fail;
}

int main(void) {
    int failures = 0;

    failures += compare(100, 8, 5, 1);
    failures += compare(1000, 16, 10, 2);
    failures += compare(10000, 64, 5, 3);
    failures += compare(10000, 768, 5, 4);
    failures += compare(100, 128, 20, 5);

    if (failures == 0) {
        printf("PASS: scalar and assembly agree\n");
        return 0;
    }
    printf("FAIL: %d mismatches\n", failures);
    return 1;
}
