#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../src/retrieval/retrieval.h"

/*
 * Reproducible benchmark harness.
 *
 * Methodology:
 *   - Fixed dataset size (n) and dimension (dim)
 *   - Fixed number of queries (q)
 *   - Random data, seeded for reproducibility
 *   - Measures wall-clock time per query
 *   - Reports min, max, mean
 */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char** argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 10000;
    int dim = (argc > 2) ? atoi(argv[2]) : 768;
    int k = (argc > 3) ? atoi(argv[3]) : 5;
    int q = (argc > 4) ? atoi(argv[4]) : 100;

    printf("LISA scalar retrieval benchmark\n");
    printf("  n     = %d\n", n);
    printf("  dim   = %d\n", dim);
    printf("  k     = %d\n", k);
    printf("  q     = %d\n", q);
    printf("  seed  = 42\n\n");

    srand(42);

    float* vectors = malloc((size_t)n * dim * sizeof(float));
    float* query = malloc(dim * sizeof(float));
    for (int i = 0; i < n * dim; i++) {
        vectors[i] = (float)rand() / (float)RAND_MAX;
    }

    int* idx = malloc(k * sizeof(int));
    float* dist = malloc(k * sizeof(float));

    double* times = malloc(q * sizeof(double));
    double sum = 0.0;
    double min_t = 1e18;
    double max_t = 0.0;

    for (int i = 0; i < q; i++) {
        for (int j = 0; j < dim; j++) {
            query[j] = (float)rand() / (float)RAND_MAX;
        }
        lisa_result_t result = {
            .indices = idx,
            .dists = dist,
            .k = k,
            .n_returned = 0
        };

        double t0 = now_ms();
        lisa_search_exact(query, vectors, n, dim, k, &result);
        double t1 = now_ms();

        double dt = t1 - t0;
        times[i] = dt;
        sum += dt;
        if (dt < min_t) min_t = dt;
        if (dt > max_t) max_t = dt;
    }

    printf("Results:\n");
    printf("  min   = %.3f ms\n", min_t);
    printf("  max   = %.3f ms\n", max_t);
    printf("  mean  = %.3f ms\n", sum / q);

    free(vectors);
    free(query);
    free(idx);
    free(dist);
    free(times);
    return 0;
}
