/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_kernel_abi — AAPCS64 conformance of the ARM64 assembly kernel.
 *
 * Regression test for two ABI violations found in lisa_ultra_mac.s:
 *
 *   1. The kernel used the full 64-bit x2/x3 registers for the 32-bit
 *      int arguments n and dim. The upper 32 bits are undefined on entry.
 *
 *   2. The kernel clobbered v8 (d8), which is callee-saved. At -O2 the
 *      caller kept a float constant in d8 across the call, which made
 *      comparisons in the fuzzer return wrong results.
 *
 * kernel_abi_probe.s calls the kernel with garbage in the upper halves of
 * x2/x3 and sentinels in d8-d15. This test checks that the sentinels
 * survive and that the kernel's distances match the scalar reference.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../src/retrieval/retrieval.h"

extern int abi_probe(const float* query, const float* vectors, int n,
                     int dim, int k, float* out_dists, int* out_indices);

static int check_case(int n, int dim) {
    float* vectors = malloc((size_t)n * dim * sizeof(float));
    float* query = malloc((size_t)dim * sizeof(float));
    float* dists = malloc((size_t)n * sizeof(float));
    int* indices = malloc((size_t)n * sizeof(int));
    if (!vectors || !query || !dists || !indices) {
        printf("FAIL: allocation\n");
        free(vectors); free(query); free(dists); free(indices);
        return 1;
    }

    srand(42);
    for (int i = 0; i < n * dim; i++) vectors[i] = (float)rand() / (float)RAND_MAX;
    for (int i = 0; i < dim; i++) query[i] = (float)rand() / (float)RAND_MAX;
    for (int i = 0; i < n; i++) { dists[i] = -1.0f; indices[i] = -1; }

    int fail = 0;

    if (abi_probe(query, vectors, n, dim, 1, dists, indices) != 0) {
        printf("FAIL: n=%d dim=%d callee-saved d8-d15 clobbered\n", n, dim);
        fail = 1;
    }

    for (int i = 0; i < n && !fail; i++) {
        float expect = lisa_l2_squared(query, vectors + (size_t)i * dim, dim);
        float tol = 1e-5f * (expect > 1.0f ? expect : 1.0f);
        if (indices[i] != i || fabsf(dists[i] - expect) > tol) {
            printf("FAIL: n=%d dim=%d i=%d index=%d dist=%.9g expected=%.9g\n",
                   n, dim, i, indices[i], dists[i], expect);
            fail = 1;
        }
    }

    free(vectors); free(query); free(dists); free(indices);
    return fail;
}

int main(void) {
    /* The kernel requires dim % 4 == 0 (the wrapper pads). */
    static const int cases[][2] = { {1, 4}, {7, 8}, {37, 20}, {256, 768} };
    int fail = 0;

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        if (check_case(cases[c][0], cases[c][1])) {
            fail = 1;
        } else {
            printf("  PASS: n=%d dim=%d\n", cases[c][0], cases[c][1]);
        }
    }

    if (fail) {
        printf("FAIL: kernel ABI test\n");
        return 1;
    }
    printf("PASS: kernel ABI test\n");
    return 0;
}
