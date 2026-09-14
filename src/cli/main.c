#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void lisa_search_ultra(float* query, float* vectors, int n, int dim, int k, float* out_dists, int* out_indices);

int main() {
    int n = 10000, dim = 768, k = 5;
    float* vectors = malloc(n * dim * sizeof(float));
    float* query = malloc(dim * sizeof(float));
    float* dists = malloc(n * sizeof(float));
    int* indices = malloc(n * sizeof(int));

    for (int i = 0; i < n * dim; i++) vectors[i] = (float)rand() / RAND_MAX;
    for (int i = 0; i < dim; i++) query[i] = (float)rand() / RAND_MAX;

    clock_t start = clock();
    lisa_search_ultra(query, vectors, n, dim, k, dists, indices);
    clock_t end = clock();

    printf("Time: %.3f ms\n", (double)(end - start) / CLOCKS_PER_SEC * 1000);
    for (int i = 0; i < k; i++) {
        printf("  %d: %f\n", indices[i], dists[i]);
    }

    free(vectors); free(query); free(dists); free(indices);
    return 0;
}
