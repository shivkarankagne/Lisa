#include <stdio.h>
#include <stdlib.h>
#include "../src/retrieval/retrieval.h"

#include "../src/kernels/arm64/lisa_asm.h"

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s n dim k seed\n", argv[0]);
        return 1;
    }
    int n = atoi(argv[1]);
    int dim = atoi(argv[2]);
    int k = atoi(argv[3]);
    unsigned seed = (unsigned)atoi(argv[4]);

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

    lisa_search_exact(query, vectors, n, dim, k, &r_s);
    lisa_search_exact_asm(query, vectors, n, dim, k, &r_a);

    printf("scalar\n");
    for (int i = 0; i < k_eff; i++) printf("%d %.9f\n", idx_s[i], dist_s[i]);
    printf("asm\n");
    for (int i = 0; i < k_eff; i++) printf("%d %.9f\n", idx_a[i], dist_a[i]);

    free(vectors); free(query);
    free(idx_s); free(dist_s);
    free(idx_a); free(dist_a);
    return 0;
}
