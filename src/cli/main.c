#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../retrieval/retrieval.h"

/*
 * LISA CLI
 *
 * Usage:
 *   lisa --index <file> --dim <int> --topk <int> --query <file>
 *
 * Inputs:
 *   --index <file>  Binary. Header: int32 n, int32 dim (little-endian).
 *                   Body:   n * dim float32 little-endian, row-major.
 *   --query <file>  Text. Floats separated by whitespace or commas.
 *
 * Output:
 *   One line per result, ascending by distance:
 *       <index> <distance>
 *
 * Exit codes:
 *   0  success
 *   1  invalid arguments
 *   2  index file error
 *   3  query file error
 *   4  dimension mismatch
 *   5  retrieval engine error
 */

extern int lisa_search_exact_asm(
    const float* query,
    const float* vectors,
    int n,
    int dim,
    int k,
    lisa_result_t* result
);

static void usage(const char* prog) {
    fprintf(stderr,
        "usage: %s --index <file> --dim <int> --query <file> [--topk <int>]\n",
        prog);
}

static int load_index(const char* path, float** out_vectors, int* out_n, int* out_dim) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open index file: %s\n", path);
        return -1;
    }

    int32_t header[2];
    if (fread(header, sizeof(int32_t), 2, f) != 2) {
        fprintf(stderr, "error: index file too small for header: %s\n", path);
        fclose(f);
        return -1;
    }

    int n = (int)header[0];
    int dim = (int)header[1];
    if (n <= 0 || dim <= 0) {
        fprintf(stderr, "error: invalid index header: n=%d dim=%d\n", n, dim);
        fclose(f);
        return -1;
    }

    size_t count = (size_t)n * (size_t)dim;
    float* vectors = (float*)malloc(count * sizeof(float));
    if (!vectors) {
        fprintf(stderr, "error: cannot allocate %zu floats\n", count);
        fclose(f);
        return -1;
    }

    if (fread(vectors, sizeof(float), count, f) != count) {
        fprintf(stderr, "error: index file body is truncated\n");
        free(vectors);
        fclose(f);
        return -1;
    }

    fclose(f);
    *out_vectors = vectors;
    *out_n = n;
    *out_dim = dim;
    return 0;
}

static int load_query(const char* path, int dim, float** out_query) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error: cannot open query file: %s\n", path);
        return -1;
    }

    float* query = (float*)malloc((size_t)dim * sizeof(float));
    if (!query) {
        fprintf(stderr, "error: cannot allocate query\n");
        fclose(f);
        return -1;
    }

    int count = 0;
    float val;
    while (count < dim && fscanf(f, " %f", &val) == 1) {
        query[count++] = val;
        int c = fgetc(f);
        if (c == ',') {
            continue;
        }
        if (c != EOF) {
            ungetc(c, f);
        }
    }

    fclose(f);

    if (count != dim) {
        fprintf(stderr, "error: query has %d values, expected %d\n", count, dim);
        free(query);
        return -1;
    }

    *out_query = query;
    return 0;
}

int main(int argc, char** argv) {
    const char* index_path = NULL;
    const char* query_path = NULL;
    int dim_arg = 0;
    int topk = 5;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index_path = argv[++i];
        } else if (strcmp(argv[i], "--query") == 0 && i + 1 < argc) {
            query_path = argv[++i];
        } else if (strcmp(argv[i], "--dim") == 0 && i + 1 < argc) {
            dim_arg = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--topk") == 0 && i + 1 < argc) {
            topk = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "error: unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!index_path || !query_path || dim_arg <= 0 || topk <= 0) {
        usage(argv[0]);
        return 1;
    }

    float* vectors = NULL;
    int n = 0;
    int dim = 0;
    if (load_index(index_path, &vectors, &n, &dim) != 0) {
        return 2;
    }

    if (dim != dim_arg) {
        fprintf(stderr, "error: index dim=%d, --dim=%d\n", dim, dim_arg);
        free(vectors);
        return 4;
    }

    float* query = NULL;
    if (load_query(query_path, dim, &query) != 0) {
        free(vectors);
        return 3;
    }

    int k_eff = (topk < n) ? topk : n;

    int* indices = (int*)malloc((size_t)k_eff * sizeof(int));
    float* dists = (float*)malloc((size_t)k_eff * sizeof(float));
    if (!indices || !dists) {
        fprintf(stderr, "error: cannot allocate result arrays\n");
        free(vectors); free(query);
        free(indices); free(dists);
        return 5;
    }

    lisa_result_t result = {
        .indices = indices,
        .dists = dists,
        .k = k_eff,
        .n_returned = 0
    };

    int rc = lisa_search_exact_asm(query, vectors, n, dim, k_eff, &result);
    if (rc != 0) {
        fprintf(stderr, "error: retrieval engine returned %d\n", rc);
        free(vectors); free(query);
        free(indices); free(dists);
        return 5;
    }

    for (int i = 0; i < result.n_returned; i++) {
        printf("%d %.6f\n", result.indices[i], result.dists[i]);
    }

    free(vectors);
    free(query);
    free(indices);
    free(dists);
    return 0;
}
