#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../src/retrieval/retrieval.h"

extern int lisa_search_exact_asm(const float*, const float*, int, int, int, lisa_result_t*);

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void bench(
    const char* name,
    int (*fn)(const float*, const float*, int, int, int, lisa_result_t*),
    const float* query,
    const float* vectors,
    int n, int dim, int k, int q
) {
    int* idx = malloc(k * sizeof(int));
    float* dist = malloc(k * sizeof(float));
    double sum = 0.0, min_t = 1e18, max_t = 0.0;
    for (int i = 0; i < q; i++) {
        lisa_result_t r = { .indices = idx, .dists = dist, .k = k, .n_returned = 0 };
        double t0 = now_ms();
        fn(query, vectors, n, dim, k, &r);
        double t1 = now_ms();
        double dt = t1 - t0;
        sum += dt;
        if (dt < min_t) min_t = dt;
        if (dt > max_t) max_t = dt;
    }
    printf("%-20s min=%.3f ms  max=%.3f ms  mean=%.3f ms\n", name, min_t, max_t, sum / q);
    free(idx); free(dist);
}

int main(int argc, char** argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 10000;
    int dim = (argc > 2) ? atoi(argv[2]) : 768;
    int k = (argc > 3) ? atoi(argv[3]) : 5;
    int q = (argc > 4) ? atoi(argv[4]) : 100;

    printf("LISA scalar vs assembly benchmark\n");
    printf("  n=%d dim=%d k=%d q=%d seed=42\n\n", n, dim, k, q);

    srand(42);
    float* vectors = malloc((size_t)n * dim * sizeof(float));
    float* query = malloc(dim * sizeof(float));
    for (int i = 0; i < n * dim; i++) vectors[i] = (float)rand() / RAND_MAX;
    for (int i = 0; i < dim; i++) query[i] = (float)rand() / RAND_MAX;

    bench("scalar", lisa_search_exact, query, vectors, n, dim, k, q);
    bench("assembly", lisa_search_exact_asm, query, vectors, n, dim, k, q);

    free(vectors); free(query);
    return 0;
}
