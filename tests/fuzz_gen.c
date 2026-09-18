#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/retrieval/retrieval.h"

#include "../src/kernels/arm64/lisa_asm.h"

/*
 * fuzz_gen — deterministic per-seed case generator.
 *
 * Usage:
 *   fuzz_gen <n> <dim> <k> <seed> <outfile>
 *
 * Writes both scalar and asm outputs to outfile in a deterministic format.
 * The caller compares outputs across runs; we do not compare inside this program.
 */

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s n dim k seed outfile\n", argv[0]);
        return 2;
    }
    int n = atoi(argv[1]);
    int dim = atoi(argv[2]);
    int k = atoi(argv[3]);
    unsigned seed = (unsigned)atoi(argv[4]);
    const char* outfile = argv[5];

    if (n <= 0 || dim <= 0 || k <= 0) {
        fprintf(stderr, "invalid args\n");
        return 2;
    }

    srand(seed);

    float* vectors = malloc((size_t)n * dim * sizeof(float));
    float* query = malloc(dim * sizeof(float));
    if (!vectors || !query) {
        free(vectors); free(query);
        return 3;
    }

    for (int i = 0; i < n * dim; i++) vectors[i] = (float)rand() / (float)RAND_MAX;
    for (int i = 0; i < dim; i++) query[i] = (float)rand() / (float)RAND_MAX;

    int k_eff = (k < n) ? k : n;

    int* idx_s = malloc(k_eff * sizeof(int));
    float* dist_s = malloc(k_eff * sizeof(float));
    int* idx_a = malloc(k_eff * sizeof(int));
    float* dist_a = malloc(k_eff * sizeof(float));
    if (!idx_s || !dist_s || !idx_a || !dist_a) {
        free(vectors); free(query);
        free(idx_s); free(dist_s); free(idx_a); free(dist_a);
        return 3;
    }

    lisa_result_t r_s = { .indices = idx_s, .dists = dist_s, .k = k_eff, .n_returned = 0 };
    lisa_result_t r_a = { .indices = idx_a, .dists = dist_a, .k = k_eff, .n_returned = 0 };

    int rc_s = lisa_search_exact(query, vectors, n, dim, k, &r_s);
    int rc_a = lisa_search_exact_asm(query, vectors, n, dim, k, &r_a);

    FILE* f = fopen(outfile, "w");
    if (!f) {
        free(vectors); free(query);
        free(idx_s); free(dist_s); free(idx_a); free(dist_a);
        return 4;
    }

    fprintf(f, "case n=%d dim=%d k=%d seed=%u\n", n, dim, k, seed);
    fprintf(f, "rc_s=%d rc_a=%d\n", rc_s, rc_a);
    fprintf(f, "n_returned_s=%d n_returned_a=%d\n", r_s.n_returned, r_a.n_returned);

    fprintf(f, "scalar\n");
    for (int i = 0; i < r_s.n_returned; i++) {
        fprintf(f, "%d %.9f\n", idx_s[i], dist_s[i]);
    }

    fprintf(f, "asm\n");
    for (int i = 0; i < r_a.n_returned; i++) {
        fprintf(f, "%d %.9f\n", idx_a[i], dist_a[i]);
    }

    fclose(f);

    free(vectors); free(query);
    free(idx_s); free(dist_s);
    free(idx_a); free(dist_a);
    return 0;
}
