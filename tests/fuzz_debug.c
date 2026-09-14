#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../src/retrieval/retrieval.h"

extern int lisa_search_exact_asm(const float*, const float*, int, int, int, lisa_result_t*);

int main(void) {
    int n = 416, dim = 1, k = 17;
    unsigned seed = 12345;
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

    printf("rc_s=%d rc_a=%d\n", rc_s, rc_a);
    printf("n_returned: scalar=%d asm=%d\n", r_s.n_returned, r_a.n_returned);
    printf("k_eff=%d\n", k_eff);

    for (int i = 0; i < k_eff && i < 5; i++) {
        float diff = fabsf(dist_s[i] - dist_a[i]);
        float tol = 1e-3f * (1.0f + fabsf(dist_s[i]));
        printf("i=%d idx_s=%d idx_a=%d dist_s=%.9f dist_a=%.9f diff=%.9g tol=%.9g %s\n",
               i, idx_s[i], idx_a[i], dist_s[i], dist_a[i], diff, tol,
               (diff > tol) ? "FAIL" : "ok");
    }

    free(vectors); free(query);
    free(idx_s); free(dist_s);
    free(idx_a); free(dist_a);
    return 0;
}
