#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../src/storage/storage.h"
#include "../src/retrieval/retrieval.h"

#include "../src/kernels/arm64/lisa_asm.h"

static int failures = 0;

static void check(const char* name, int cond) {
    if (cond) {
        printf("  PASS: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

static int rm_rf(const char* path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    return system(cmd);
}

int main(void) {
    const char* dir = "/tmp/lisa_storage_test_collection";
    int n = 128;
    int dim = 32;
    int k = 5;

    printf("Storage tests\n");

    /* clean any leftover */
    rm_rf(dir);

    /* prepare data */
    float* vectors = malloc((size_t)n * dim * sizeof(float));
    float* query = malloc(dim * sizeof(float));
    for (int i = 0; i < n * dim; i++) vectors[i] = (float)rand() / (float)RAND_MAX;
    for (int i = 0; i < dim; i++) query[i] = (float)rand() / (float)RAND_MAX;

    /* create */
    int rc = storage_create(dir, n, dim, vectors);
    check("storage_create returns 0", rc == 0);
    check("directory exists", access(dir, F_OK) == 0);

    /* create on existing directory must fail */
    rc = storage_create(dir, n, dim, vectors);
    check("storage_create on existing directory fails", rc != 0);

    /* open */
    int h = storage_open(dir);
    check("storage_open returns handle > 0", h > 0);

    /* metadata */
    check("storage_get_n", storage_get_n(h) == n);
    check("storage_get_dim", storage_get_dim(h) == dim);

    /* vectors identical */
    const float* stored = storage_get_vectors(h);
    check("storage_get_vectors non-null", stored != NULL);
    int same = 1;
    for (int i = 0; i < n * dim; i++) {
        if (stored[i] != vectors[i]) { same = 0; break; }
    }
    check("stored vectors identical to input", same == 1);

    /* search on stored vectors matches search on in-RAM vectors */
    int* idx_ram = malloc(k * sizeof(int));
    float* d_ram = malloc(k * sizeof(float));
    int* idx_st  = malloc(k * sizeof(int));
    float* d_st  = malloc(k * sizeof(float));

    lisa_result_t r_ram = { .indices = idx_ram, .dists = d_ram, .k = k, .n_returned = 0 };
    lisa_result_t r_st  = { .indices = idx_st,  .dists = d_st,  .k = k, .n_returned = 0 };

    int rcs = lisa_search_exact_asm(query, vectors, n, dim, k, &r_ram);
    int rst = lisa_search_exact_asm(query, stored,  n, dim, k, &r_st);
    check("search on in-RAM returns 0", rcs == 0);
    check("search on stored returns 0", rst == 0);
    check("n_returned matches", r_ram.n_returned == r_st.n_returned);

    int match = 1;
    for (int i = 0; i < k; i++) {
        if (idx_ram[i] != idx_st[i]) { match = 0; break; }
        if (fabsf(d_ram[i] - d_st[i]) > 1e-4f) { match = 0; break; }
    }
    check("top-k identical between RAM and stored", match == 1);

    /* close */
    rc = storage_close(h);
    check("storage_close returns 0", rc == 0);

    /* reopen */
    int h2 = storage_open(dir);
    check("reopen returns handle > 0", h2 > 0);
    check("reopen n matches", storage_get_n(h2) == n);
    check("reopen dim matches", storage_get_dim(h2) == dim);

    const float* stored2 = storage_get_vectors(h2);
    int same2 = 1;
    for (int i = 0; i < n * dim; i++) {
        if (stored2[i] != vectors[i]) { same2 = 0; break; }
    }
    check("reopened vectors identical", same2 == 1);

    storage_close(h2);

    /* corrupted header */
    const char* bad_dir = "/tmp/lisa_storage_test_bad";
    rm_rf(bad_dir);
    mkdir(bad_dir, 0755);
    {
        FILE* f = fopen("/tmp/lisa_storage_test_bad/header.bin", "wb");
        if (f) {
            unsigned char junk[16] = { 'X','X','X','X', 0,0,0,0, 0,0,0,0, 0,0,0,0 };
            fwrite(junk, 1, 16, f);
            fclose(f);
        }
    }
    int bad = storage_open(bad_dir);
    check("open on bad magic fails", bad < 0);

    rm_rf(dir);
    rm_rf(bad_dir);

    free(vectors); free(query);
    free(idx_ram); free(d_ram);
    free(idx_st);  free(d_st);

    if (failures == 0) {
        printf("PASS: all storage tests passed\n");
        return 0;
    }
    printf("FAIL: %d storage failures\n", failures);
    return 1;
}
