/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * test_kernels — every kernel table vs the scalar reference distance.
 *
 * Buffers are allocated to their exact size, so under ASan any read past
 * the end of a vector (e.g. a missing tail loop) is reported.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/kernels/kernels.h"
#include "../src/retrieval/retrieval.h"

static int g_fail = 0;

static void check(int cond, const char* what) {
    if (!cond) {
        printf("  FAIL: %s\n", what);
        g_fail = 1;
    }
}

static int check_table(const lisa_kernel_table_t* t, int64_t n, int64_t dim, unsigned seed) {
    float* q = malloc((size_t)dim * sizeof(float));
    float* v = malloc((size_t)(n * dim) * sizeof(float));
    float* out = malloc((size_t)n * sizeof(float));
    if (!q || !v || !out) {
        free(q); free(v); free(out);
        printf("  FAIL: allocation\n");
        return 1;
    }
    srand(seed);
    for (int64_t i = 0; i < dim; i++) q[i] = (float)rand() / (float)RAND_MAX * 2.0f - 1.0f;
    for (int64_t i = 0; i < n * dim; i++) v[i] = (float)rand() / (float)RAND_MAX * 2.0f - 1.0f;

    t->l2_batch(q, v, n, dim, out);

    int fail = 0;
    for (int64_t i = 0; i < n; i++) {
        float expect = lisa_l2_squared(q, v + i * dim, (int)dim);
        float tol = 1e-5f * (expect > 1.0f ? expect : 1.0f);
        if (fabsf(out[i] - expect) > tol) {
            printf("  FAIL: %s n=%lld dim=%lld i=%lld got=%.9g expected=%.9g\n",
                   t->name, (long long)n, (long long)dim, (long long)i,
                   out[i], expect);
            fail = 1;
            break;
        }
    }
    free(q); free(v); free(out);
    return fail;
}

int main(void) {
    const char* names[] = { "scalar", "neon" };
    static const int64_t dims[] = { 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 33,
                                    63, 64, 65, 384, 768, 1023, 1024, 1536 };
    static const int64_t ns[] = { 1, 3, 257 };

    printf("Kernel tests\n");

    check(lisa_kernels() != NULL, "lisa_kernels() returns a table");
    check(lisa_kernels_by_name(NULL) == NULL, "by_name(NULL) is NULL");
    check(lisa_kernels_by_name("no-such-kernel") == NULL, "unknown name is NULL");
    check(lisa_kernels_by_name("scalar") != NULL, "scalar is always available");
#if defined(__aarch64__) || defined(_M_ARM64)
    check(lisa_kernels_by_name("neon") != NULL, "neon available on ARM64");
#endif

    for (size_t t = 0; t < sizeof(names) / sizeof(names[0]); t++) {
        const lisa_kernel_table_t* table = lisa_kernels_by_name(names[t]);
        if (table == NULL) {
            printf("  (kernel %s not in this build)\n", names[t]);
            continue;
        }
        int fail = 0;
        for (size_t d = 0; d < sizeof(dims) / sizeof(dims[0]); d++) {
            for (size_t i = 0; i < sizeof(ns) / sizeof(ns[0]); i++) {
                fail |= check_table(table, ns[i], dims[d], (unsigned)(d * 31 + i));
            }
        }
        if (fail) g_fail = 1;
        else printf("  PASS: %s matches reference on %zu shapes\n", names[t],
                    (sizeof(dims) / sizeof(dims[0])) * (sizeof(ns) / sizeof(ns[0])));
    }

    if (g_fail) {
        printf("FAIL: kernel tests\n");
        return 1;
    }
    printf("PASS: kernel tests\n");
    return 0;
}
