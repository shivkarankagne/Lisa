#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../src/storage/storage.h"

/*
 * build_collection — test helper.
 *
 * Usage:
 *   build_collection <index_file> <collection_dir>
 *
 * Reads a binary index file (int32 n, int32 dim, then n*dim float32 LE)
 * and calls storage_create on <collection_dir>.
 *
 * Exit codes:
 *   0  success
 *   1  invalid arguments
 *   2  index file error
 *   3  storage error
 */

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <index_file> <collection_dir>\n", argv[0]);
        return 1;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open index file: %s\n", argv[1]);
        return 2;
    }

    int32_t header[2];
    if (fread(header, sizeof(int32_t), 2, f) != 2) {
        fprintf(stderr, "error: index header read failed\n");
        fclose(f);
        return 2;
    }

    int n = (int)header[0];
    int dim = (int)header[1];
    if (n <= 0 || dim <= 0) {
        fprintf(stderr, "error: invalid header: n=%d dim=%d\n", n, dim);
        fclose(f);
        return 2;
    }

    size_t count = (size_t)n * (size_t)dim;
    float* vectors = (float*)malloc(count * sizeof(float));
    if (!vectors) {
        fprintf(stderr, "error: allocation failed\n");
        fclose(f);
        return 2;
    }

    if (fread(vectors, sizeof(float), count, f) != count) {
        fprintf(stderr, "error: index body truncated\n");
        free(vectors);
        fclose(f);
        return 2;
    }
    fclose(f);

    int rc = storage_create(argv[2], n, dim, vectors);
    free(vectors);
    if (rc != 0) {
        fprintf(stderr, "error: storage_create failed: %d\n", rc);
        return 3;
    }
    return 0;
}
