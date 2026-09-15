#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../src/storage/storage.h"
#include "../src/retrieval/retrieval.h"

extern int lisa_search_exact_asm(const float*, const float*, int, int, int, lisa_result_t*);

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

/* compare two collections' on-disk vectors against an expected buffer */
static int verify_vectors_on_disk(const char* path, const float* expect, int n, int dim) {
    char vpath[512];
    snprintf(vpath, sizeof(vpath), "%s/vectors.bin", path);
    FILE* f = fopen(vpath, "rb");
    if (!f) return 0;
    size_t live = (size_t)n * (size_t)dim;
    float* buf = malloc(live * sizeof(float));
    if (!buf) { fclose(f); return 0; }
    int ok = 1;
    if (fread(buf, sizeof(float), live, f) != live) ok = 0;
    if (ok) {
        for (size_t i = 0; i < live; i++) {
            if (buf[i] != expect[i]) { ok = 0; break; }
        }
    }
    free(buf);
    fclose(f);
    return ok;
}

int main(void) {
    const char* dir = "/tmp/lisa_storage_mutation";
    int n0 = 8;
    int dim = 4;

    printf("Storage mutation tests\n");

    rm_rf(dir);

    float vectors[8 * 4] = {
        0,0,0,0,
        1,0,0,0,
        2,0,0,0,
        3,0,0,0,
        4,0,0,0,
        5,0,0,0,
        6,0,0,0,
        7,0,0,0
    };

    int rc = storage_create(dir, n0, dim, vectors);
    check("create", rc == 0);

    int h = storage_open(dir);
    check("open", h > 0);
    check("initial n", storage_get_n(h) == n0);
    check("initial dim", storage_get_dim(h) == dim);

    /* --- insert without growth --- */
    /* capacity == n == 8, so first insert must grow. */
    float v8[4] = { 8,0,0,0 };
    int idx = storage_insert(h, v8);
    check("insert grows: returns index 8", idx == 8);
    check("after insert n == 9", storage_get_n(h) == 9);

    /* --- insert without growth (capacity now 16) --- */
    float v9[4] = { 9,0,0,0 };
    int idx2 = storage_insert(h, v9);
    check("insert in place: returns index 9", idx2 == 9);
    check("after insert n == 10", storage_get_n(h) == 10);

    /* --- verify on-disk vectors --- */
    float expect_after_insert[10 * 4] = {
        0,0,0,0,
        1,0,0,0,
        2,0,0,0,
        3,0,0,0,
        4,0,0,0,
        5,0,0,0,
        6,0,0,0,
        7,0,0,0,
        8,0,0,0,
        9,0,0,0
    };
    check("on-disk vectors after two inserts",
          verify_vectors_on_disk(dir, expect_after_insert, 10, dim));

    /* --- delete middle --- */
    rc = storage_delete(h, 3);
    check("delete index 3 returns 0", rc == 0);
    check("after delete n == 9", storage_get_n(h) == 9);

    float expect_after_delete[9 * 4] = {
        0,0,0,0,
        1,0,0,0,
        2,0,0,0,
        4,0,0,0,
        5,0,0,0,
        6,0,0,0,
        7,0,0,0,
        8,0,0,0,
        9,0,0,0
    };
    check("on-disk vectors after delete index 3",
          verify_vectors_on_disk(dir, expect_after_delete, 9, dim));

    /* --- delete last --- */
    rc = storage_delete(h, 8);
    check("delete last returns 0", rc == 0);
    check("after delete last n == 8", storage_get_n(h) == 8);

    /* --- delete first --- */
    rc = storage_delete(h, 0);
    check("delete first returns 0", rc == 0);
    check("after delete first n == 7", storage_get_n(h) == 7);

    float expect_after_delete_first[7 * 4] = {
        1,0,0,0,
        2,0,0,0,
        4,0,0,0,
        5,0,0,0,
        6,0,0,0,
        7,0,0,0,
        8,0,0,0
    };
    check("on-disk vectors after delete first",
          verify_vectors_on_disk(dir, expect_after_delete_first, 7, dim));

    /* --- out-of-range delete --- */
    rc = storage_delete(h, 1000);
    check("delete out-of-range returns -7", rc == -7);
    rc = storage_delete(h, -1);
    check("delete negative returns -7", rc == -7);

    /* --- close, reopen, verify persistence --- */
    storage_close(h);

    int h2 = storage_open(dir);
    check("reopen", h2 > 0);
    check("reopened n == 7", storage_get_n(h2) == 7);
    check("reopened dim == 4", storage_get_dim(h2) == 4);
    check("reopened on-disk matches", 
          verify_vectors_on_disk(dir, expect_after_delete_first, 7, dim));

    const float* rv = storage_get_vectors(h2);
    int same = 1;
    for (size_t i = 0; i < (size_t)7 * dim; i++) {
        if (rv[i] != expect_after_delete_first[i]) { same = 0; break; }
    }
    check("reopened in-memory matches", same == 1);

    /* --- search on reopened collection matches search on expected --- */
    {
        int k = 3;
        float query[4] = { 5.5f, 0, 0, 0 };
        int* idx_ram = malloc(k * sizeof(int));
        float* d_ram = malloc(k * sizeof(float));
        int* idx_col = malloc(k * sizeof(int));
        float* d_col = malloc(k * sizeof(float));
        lisa_result_t r_ram = { .indices = idx_ram, .dists = d_ram, .k = k, .n_returned = 0 };
        lisa_result_t r_col = { .indices = idx_col, .dists = d_col, .k = k, .n_returned = 0 };
        lisa_search_exact_asm(query, expect_after_delete_first, 7, dim, k, &r_ram);
        lisa_search_exact_asm(query, rv, 7, dim, k, &r_col);
        int match = (r_ram.n_returned == r_col.n_returned);
        if (match) {
            for (int i = 0; i < k; i++) {
                if (idx_ram[i] != idx_col[i]) { match = 0; break; }
                if (fabsf(d_ram[i] - d_col[i]) > 1e-4f) { match = 0; break; }
            }
        }
        check("search on reopened matches search on expected", match == 1);
        free(idx_ram); free(d_ram);
        free(idx_col); free(d_col);
    }

    storage_close(h2);

    /* --- v1 read-only path --- */
    const char* v1dir = "/tmp/lisa_storage_v1";
    rm_rf(v1dir);
    mkdir(v1dir, 0755);
    {
        /* craft a v1 header: magic, version=1, n=2, dim=4 */
        unsigned char hdr[16] = {
            'L','I','S','A',
            1,0,0,0,
            2,0,0,0,
            4,0,0,0
        };
        char p[512];
        snprintf(p, sizeof(p), "%s/header.bin", v1dir);
        FILE* f = fopen(p, "wb");
        if (f) { fwrite(hdr, 1, 16, f); fclose(f); }
        float vv[8] = { 0,0,0,0, 1,0,0,0 };
        snprintf(p, sizeof(p), "%s/vectors.bin", v1dir);
        f = fopen(p, "wb");
        if (f) { fwrite(vv, sizeof(float), 8, f); fclose(f); }
    }
    int hv1 = storage_open(v1dir);
    check("open v1 file", hv1 > 0);
    check("v1 n == 2", storage_get_n(hv1) == 2);
    check("v1 dim == 4", storage_get_dim(hv1) == 4);

    float extra[4] = { 2,0,0,0 };
    rc = storage_insert(hv1, extra);
    check("insert on v1 returns -3", rc == -3);
    rc = storage_delete(hv1, 0);
    check("delete on v1 returns -3", rc == -3);
    storage_close(hv1);

    rm_rf(dir);
    rm_rf(v1dir);

    if (failures == 0) {
        printf("PASS: all storage mutation tests passed\n");
        return 0;
    }
    printf("FAIL: %d mutation failures\n", failures);
    return 1;
}
