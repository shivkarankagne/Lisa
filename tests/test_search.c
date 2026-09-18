/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_search — lisa_search (production path) vs lisa_search_exact
 * (scalar reference).
 *
 * Run by ctest twice: with the default kernel and with LISA_KERNEL=scalar.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/kernels/kernels.h"
#include "../src/retrieval/retrieval.h"

static int g_fail = 0;

static void check(int cond, const char* what) {
    if (cond) {
        printf("  PASS: %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        g_fail = 1;
    }
}

static int near(float a, float b) {
    float m = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
    return fabsf(a - b) <= 1e-5f * (m > 1.0f ? m : 1.0f);
}

/* Returns 0 if lisa_search agrees with the reference for this case. */
static int compare_case(int n, int dim, int k, unsigned seed) {
    float* v = malloc((size_t)n * dim * sizeof(float));
    float* q = malloc((size_t)dim * sizeof(float));
    int k_eff = k < n ? k : n;
    int* ir = malloc((size_t)k_eff * sizeof(int));
    float* dr = malloc((size_t)k_eff * sizeof(float));
    int* is = malloc((size_t)k_eff * sizeof(int));
    float* ds = malloc((size_t)k_eff * sizeof(float));
    if (!v || !q || !ir || !dr || !is || !ds) {
        free(v); free(q); free(ir); free(dr); free(is); free(ds);
        return 1;
    }
    srand(seed);
    for (int i = 0; i < n * dim; i++) v[i] = (float)rand() / (float)RAND_MAX;
    for (int i = 0; i < dim; i++) q[i] = (float)rand() / (float)RAND_MAX;

    lisa_result_t ref = { ir, dr, k_eff, 0 };
    lisa_result_t got = { is, ds, k_eff, 0 };
    int rc_r = lisa_search_exact(q, v, n, dim, k, &ref);
    int rc_s = lisa_search(q, v, n, dim, k, &got);

    int fail = 0;
    if (rc_r != 0 || rc_s != 0 || ref.n_returned != got.n_returned) {
        printf("  FAIL: n=%d dim=%d k=%d rc=%d/%d count=%d/%d\n",
               n, dim, k, rc_r, rc_s, ref.n_returned, got.n_returned);
        fail = 1;
    }
    for (int i = 0; i < k_eff && !fail; i++) {
        /*
         * Distances at each rank must agree. Indices may differ only where
         * two candidates are tied to within rounding, which near() allows.
         */
        if (!near(dr[i], ds[i])) {
            printf("  FAIL: n=%d dim=%d k=%d seed=%u pos=%d ref=(%d,%.9g) got=(%d,%.9g)\n",
                   n, dim, k, seed, i, ir[i], dr[i], is[i], ds[i]);
            fail = 1;
        }
    }
    for (int i = 1; i < k_eff && !fail; i++) {
        if (ds[i] < ds[i - 1]) {
            printf("  FAIL: results not ascending at %d\n", i);
            fail = 1;
        }
    }
    free(v); free(q); free(ir); free(dr); free(is); free(ds);
    return fail;
}

/*
 * Masked search vs the reference run on a compacted copy containing only
 * the live vectors. Returns 0 on agreement.
 */
static int compare_masked(int n, int dim, int k, unsigned seed, int live_pct) {
    float* v = malloc((size_t)n * dim * sizeof(float));
    float* c = malloc((size_t)n * dim * sizeof(float));
    int* map = malloc((size_t)n * sizeof(int));
    uint8_t* live = malloc((size_t)n);
    float* q = malloc((size_t)dim * sizeof(float));
    int* ir = malloc((size_t)k * sizeof(int));
    float* dr = malloc((size_t)k * sizeof(float));
    int* is = malloc((size_t)k * sizeof(int));
    float* ds = malloc((size_t)k * sizeof(float));
    int fail = 0;
    if (!v || !c || !map || !live || !q || !ir || !dr || !is || !ds) { fail = 1; goto out; }

    srand(seed);
    for (int i = 0; i < n * dim; i++) v[i] = (float)rand() / (float)RAND_MAX;
    for (int i = 0; i < dim; i++) q[i] = (float)rand() / (float)RAND_MAX;
    int m = 0;
    for (int i = 0; i < n; i++) {
        live[i] = (uint8_t)((rand() % 100) < live_pct);
        if (live[i]) {
            memcpy(c + (size_t)m * dim, v + (size_t)i * dim, (size_t)dim * sizeof(float));
            map[m++] = i;
        }
    }

    lisa_result_t got = { is, ds, k, 0 };
    if (lisa_search_masked(q, v, n, dim, k, live, &got) != 0) { fail = 1; goto out; }
    if (m == 0) { fail = got.n_returned != 0; goto out; }

    lisa_result_t ref = { ir, dr, k, 0 };
    if (lisa_search_exact(q, c, m, dim, k, &ref) != 0 || ref.n_returned != got.n_returned) {
        fail = 1; goto out;
    }
    for (int i = 0; i < got.n_returned; i++) {
        if (!live[is[i]] || !near(dr[i], ds[i])) {
            printf("  FAIL: masked n=%d dim=%d k=%d pos=%d ref=(%d,%.9g) got=(%d,%.9g)\n",
                   n, dim, k, i, map[ir[i]], dr[i], is[i], ds[i]);
            fail = 1;
            break;
        }
    }
out:
    free(v); free(c); free(map); free(live); free(q);
    free(ir); free(dr); free(is); free(ds);
    return fail;
}

int main(void) {
    printf("Search tests (kernel: %s)\n", lisa_kernels()->name);

    float q[4] = { 0 }, v[8] = { 0 }, d[2];
    int idx[2];
    lisa_result_t r = { idx, d, 2, 0 };
    lisa_result_t r_null_idx = { NULL, d, 2, 0 };
    lisa_result_t r_null_dist = { idx, NULL, 2, 0 };

    check(lisa_search(NULL, v, 2, 4, 2, &r) == -1, "NULL query -> -1");
    check(lisa_search(q, NULL, 2, 4, 2, &r) == -1, "NULL vectors -> -1");
    check(lisa_search(q, v, 2, 4, 2, NULL) == -1, "NULL result -> -1");
    check(lisa_search(q, v, 0, 4, 2, &r) == -2, "n=0 -> -2");
    check(lisa_search(q, v, 2, 0, 2, &r) == -2, "dim=0 -> -2");
    check(lisa_search(q, v, 2, 4, 0, &r) == -2, "k=0 -> -2");
    check(lisa_search(q, v, -1, 4, 2, &r) == -2, "n<0 -> -2");
    check(lisa_search(q, v, (int64_t)INT32_MAX + 1, 4, 2, &r) == -2,
          "n > INT32_MAX -> -2 (no read)");
    check(lisa_search(q, v, 2, 4, 2, &r_null_idx) == -3, "NULL indices -> -3");
    check(lisa_search(q, v, 2, 4, 2, &r_null_dist) == -3, "NULL dists -> -3");

    /* k > n clamps to n */
    v[4] = 1.0f;
    check(lisa_search(q, v, 2, 4, 10, &r) == 0 && r.n_returned == 2 &&
          idx[0] == 0 && idx[1] == 1, "k > n clamps; ascending order");

    /*
     * Exact ties: the order among equal distances is not index order (heap
     * extraction is not stable), but it must be identical to the reference.
     */
    float tv[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  2, 0, 0, 0 };
    float tq[4] = { 0, 0, 0, 0 };
    int ti[4], ri[4]; float td[4], rd[4];
    lisa_result_t tr = { ti, td, 4, 0 };
    lisa_result_t rr = { ri, rd, 4, 0 };
    int tie_ok = lisa_search(tq, tv, 4, 4, 4, &tr) == 0 &&
                 lisa_search_exact(tq, tv, 4, 4, 4, &rr) == 0;
    for (int i = 0; i < 4 && tie_ok; i++) {
        tie_ok = ti[i] == ri[i] && td[i] == rd[i];
    }
    check(tie_ok && ti[3] == 3, "exact ties ordered identically to reference");

    /* Differential: blocks boundaries (255/256/257/513), k > 256, odd dims. */
    static const int cases[][3] = {
        { 1, 1, 1 }, { 5, 3, 2 }, { 255, 17, 10 }, { 256, 64, 256 },
        { 257, 65, 257 }, { 513, 768, 5 }, { 1000, 384, 300 },
        { 2000, 7, 50 }, { 4096, 1024, 16 },
    };
    int fail = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        fail |= compare_case(cases[c][0], cases[c][1], cases[c][2], 1000u + (unsigned)c);
    }
    for (unsigned s = 0; s < 200; s++) {
        srand(s * 7919u + 1u);
        int n = 1 + rand() % 1500;
        int dim = 1 + rand() % 600;
        int k = 1 + rand() % 60;
        fail |= compare_case(n, dim, k, s);
    }
    check(!fail, "agrees with scalar reference on 209 cases");

    /* Masked search. */
    uint8_t none[2] = { 0, 0 };
    check(lisa_search_masked(q, v, 2, 4, 2, none, &r) == 0 && r.n_returned == 0,
          "masked: nothing live -> 0 results");
    uint8_t second[2] = { 0, 1 };
    check(lisa_search_masked(q, v, 2, 4, 2, second, &r) == 0 && r.n_returned == 1 &&
          idx[0] == 1, "masked: dead vector never returned, k clamps to live count");
    check(lisa_search_masked(NULL, v, 2, 4, 2, second, &r) == -1, "masked: NULL query -> -1");

    int mfail = 0;
    for (unsigned s2 = 0; s2 < 60; s2++) {
        srand(s2 * 104729u + 7u);
        int n = 1 + rand() % 1200;
        int dim = 1 + rand() % 300;
        int k = 1 + rand() % 40;
        int pct = (int)(s2 % 4) * 30 + 5;   /* 5%, 35%, 65%, 95% live */
        mfail |= compare_masked(n, dim, k, s2, pct);
    }
    check(!mfail, "masked search agrees with reference on compacted data (60 cases)");

    if (g_fail) {
        printf("FAIL: search tests\n");
        return 1;
    }
    printf("PASS: search tests\n");
    return 0;
}
