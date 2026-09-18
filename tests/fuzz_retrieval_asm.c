#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../src/retrieval/retrieval.h"

#include "../src/kernels/arm64/lisa_asm.h"

static int fuzz_one(unsigned seed) {
    srand(seed);

    int n = 1 + rand() % 2000;
    int dim = 1 + rand() % 512;
    int k = 1 + rand() % (n < 50 ? n : 50);

    float* vectors = malloc((size_t)n * dim * sizeof(float));
    float* query = malloc(dim * sizeof(float));
    if (!vectors || !query) {
        free(vectors); free(query);
        return 0;
    }

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
        printf("FUZZ FAIL (rc): n=%d dim=%d k=%d seed=%u scalar_rc=%d asm_rc=%d\n",
               n, dim, k, seed, rc_s, rc_a);
        fail = 1;
    } else if (r_s.n_returned != r_a.n_returned) {
        printf("FUZZ FAIL (count): n=%d dim=%d k=%d seed=%u scalar_n=%d asm_n=%d\n",
               n, dim, k, seed, r_s.n_returned, r_a.n_returned);
        fail = 1;
    } else {
        for (int i = 0; i < k_eff; i++) {
            float s = dist_s[i];
            float a = dist_a[i];
            float diff = (s > a) ? (s - a) : (a - s);
            int condition = (diff > 0.0001f);
            if (condition) {
                printf("FUZZ FAIL (dist): n=%d dim=%d k=%d seed=%u pos=%d "
                       "scalar=%.9f asm=%.9f diff=%.9g cond=%d\n",
                       n, dim, k, seed, i, s, a, diff, condition);
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

int main(int argc, char** argv) {
    int num_iterations = (argc > 1) ? atoi(argv[1]) : 1000;
    unsigned base_seed = (argc > 2) ? (unsigned)atoi(argv[2]) : 12345u;

    printf("Fuzzing scalar vs assembly\n");
    printf("  iterations = %d\n", num_iterations);
    printf("  base seed  = %u\n\n", base_seed);

    int failures = 0;
    for (int i = 0; i < num_iterations; i++) {
        if (fuzz_one(base_seed + i)) {
            failures++;
            if (failures >= 5) {
                printf("Stopping after 5 failures.\n");
                break;
            }
        }
    }

    if (failures == 0) {
        printf("PASS: %d iterations, no mismatches\n", num_iterations);
        return 0;
    }
    printf("FAIL: %d mismatches in %d iterations\n", failures, num_iterations);
    return 1;
}
