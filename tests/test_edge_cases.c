#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/retrieval/retrieval.h"

extern int lisa_search_exact_asm(const float*, const float*, int, int, int, lisa_result_t*);

static int failures = 0;

static void expect_rc(const char* name, int got, int want) {
    if (got == want) {
        printf("  PASS: %s -> rc=%d\n", name, got);
    } else {
        printf("  FAIL: %s -> got rc=%d, want rc=%d\n", name, got, want);
        failures++;
    }
}

static void test_null_query(void) {
    float v[4] = {0};
    int idx[1];
    float d[1];
    lisa_result_t r = { .indices = idx, .dists = d, .k = 1, .n_returned = 0 };
    expect_rc("NULL query", lisa_search_exact(NULL, v, 1, 4, 1, &r), -1);
    expect_rc("NULL query (asm)", lisa_search_exact_asm(NULL, v, 1, 4, 1, &r), -1);
}

static void test_null_vectors(void) {
    float q[4] = {0};
    int idx[1];
    float d[1];
    lisa_result_t r = { .indices = idx, .dists = d, .k = 1, .n_returned = 0 };
    expect_rc("NULL vectors", lisa_search_exact(q, NULL, 1, 4, 1, &r), -1);
    expect_rc("NULL vectors (asm)", lisa_search_exact_asm(q, NULL, 1, 4, 1, &r), -1);
}

static void test_null_result(void) {
    float q[4] = {0};
    float v[4] = {0};
    expect_rc("NULL result", lisa_search_exact(q, v, 1, 4, 1, NULL), -1);
    expect_rc("NULL result (asm)", lisa_search_exact_asm(q, v, 1, 4, 1, NULL), -1);
}

static void test_null_arrays(void) {
    float q[4] = {0};
    float v[4] = {0};
    lisa_result_t r = { .indices = NULL, .dists = NULL, .k = 1, .n_returned = 0 };
    expect_rc("NULL result arrays", lisa_search_exact(q, v, 1, 4, 1, &r), -3);
    expect_rc("NULL result arrays (asm)", lisa_search_exact_asm(q, v, 1, 4, 1, &r), -3);
}

static void test_zero_or_negative(void) {
    float q[4] = {0};
    float v[4] = {0};
    int idx[1];
    float d[1];
    lisa_result_t r = { .indices = idx, .dists = d, .k = 1, .n_returned = 0 };
    expect_rc("n=0", lisa_search_exact(q, v, 0, 4, 1, &r), -2);
    expect_rc("n=-1", lisa_search_exact(q, v, -1, 4, 1, &r), -2);
    expect_rc("dim=0", lisa_search_exact(q, v, 1, 0, 1, &r), -2);
    expect_rc("dim=-1", lisa_search_exact(q, v, 1, -1, 1, &r), -2);
    expect_rc("k=0", lisa_search_exact(q, v, 1, 4, 0, &r), -2);
    expect_rc("k=-1", lisa_search_exact(q, v, 1, 4, -1, &r), -2);
}

static void test_k_greater_than_n(void) {
    int n = 3, dim = 4, k = 10;
    float q[4] = {1, 0, 0, 0};
    float v[12] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0
    };
    int idx[10];
    float d[10];
    lisa_result_t r = { .indices = idx, .dists = d, .k = k, .n_returned = 0 };
    int rc = lisa_search_exact(q, v, n, dim, k, &r);
    if (rc != 0) {
        printf("  FAIL: k>n scalar rc=%d\n", rc);
        failures++;
    } else if (r.n_returned != n) {
        printf("  FAIL: k>n scalar n_returned=%d want=%d\n", r.n_returned, n);
        failures++;
    } else {
        printf("  PASS: k>n scalar n_returned=%d\n", r.n_returned);
    }

    int idx2[10];
    float d2[10];
    lisa_result_t r2 = { .indices = idx2, .dists = d2, .k = k, .n_returned = 0 };
    rc = lisa_search_exact_asm(q, v, n, dim, k, &r2);
    if (rc != 0) {
        printf("  FAIL: k>n asm rc=%d\n", rc);
        failures++;
    } else if (r2.n_returned != n) {
        printf("  FAIL: k>n asm n_returned=%d want=%d\n", r2.n_returned, n);
        failures++;
    } else {
        printf("  PASS: k>n asm n_returned=%d\n", r2.n_returned);
    }
}

static void test_dim_one(void) {
    int n = 4, dim = 1, k = 3;
    float q[1] = {0.5f};
    float v[4] = {0.0f, 1.0f, 0.5f, 2.0f};
    int idx[3];
    float d[3];
    lisa_result_t r = { .indices = idx, .dists = d, .k = k, .n_returned = 0 };
    int rc = lisa_search_exact(q, v, n, dim, k, &r);
    if (rc != 0) {
        printf("  FAIL: dim=1 scalar rc=%d\n", rc);
        failures++;
    } else {
        printf("  PASS: dim=1 scalar rc=0\n");
    }

    int idx2[3];
    float d2[3];
    lisa_result_t r2 = { .indices = idx2, .dists = d2, .k = k, .n_returned = 0 };
    rc = lisa_search_exact_asm(q, v, n, dim, k, &r2);
    if (rc != 0) {
        printf("  FAIL: dim=1 asm rc=%d\n", rc);
        failures++;
    } else {
        printf("  PASS: dim=1 asm rc=0\n");
    }
}

static void test_dim_not_multiple_of_four(void) {
    int dims[] = {2, 3, 5, 6, 7, 11, 13, 17, 19, 23};
    int num_dims = sizeof(dims) / sizeof(dims[0]);
    for (int t = 0; t < num_dims; t++) {
        int dim = dims[t];
        int n = 8, k = 3;
        float* q = malloc(dim * sizeof(float));
        float* v = malloc(n * dim * sizeof(float));
        int* idx = malloc(k * sizeof(int));
        float* d = malloc(k * sizeof(float));
        int* idx2 = malloc(k * sizeof(int));
        float* d2 = malloc(k * sizeof(float));
        for (int i = 0; i < dim; i++) q[i] = (float)(i % 5) * 0.1f;
        for (int i = 0; i < n * dim; i++) v[i] = (float)(i % 7) * 0.1f;

        lisa_result_t r  = { .indices = idx,  .dists = d,  .k = k, .n_returned = 0 };
        lisa_result_t r2 = { .indices = idx2, .dists = d2, .k = k, .n_returned = 0 };

        int rc1 = lisa_search_exact(q, v, n, dim, k, &r);
        int rc2 = lisa_search_exact_asm(q, v, n, dim, k, &r2);

        int ok = (rc1 == 0) && (rc2 == 0) && (r.n_returned == r2.n_returned);
        if (ok) {
            for (int i = 0; i < k; i++) {
                float diff = d[i] - d2[i];
                if (diff < 0) diff = -diff;
                if (diff > 0.0001f) { ok = 0; break; }
            }
        }
        if (ok) {
            printf("  PASS: dim=%d scalar and asm agree\n", dim);
        } else {
            printf("  FAIL: dim=%d scalar rc=%d asm rc=%d\n", dim, rc1, rc2);
            failures++;
        }

        free(q); free(v);
        free(idx); free(d);
        free(idx2); free(d2);
    }
}

int main(void) {
    printf("Edge-case tests\n\n");

    printf("NULL pointer validation:\n");
    test_null_query();
    test_null_vectors();
    test_null_result();
    test_null_arrays();
    printf("\n");

    printf("Zero / negative inputs:\n");
    test_zero_or_negative();
    printf("\n");

    printf("k > n:\n");
    test_k_greater_than_n();
    printf("\n");

    printf("dim = 1:\n");
    test_dim_one();
    printf("\n");

    printf("dim not multiple of 4:\n");
    test_dim_not_multiple_of_four();
    printf("\n");

    if (failures == 0) {
        printf("PASS: all edge-case tests passed\n");
        return 0;
    }
    printf("FAIL: %d edge-case failures\n", failures);
    return 1;
}
