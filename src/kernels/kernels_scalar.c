/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Scalar kernels. Portable C, available on every platform.
 */

#include "kernels_internal.h"

void lisa_l2_batch_scalar(
    const float* query,
    const float* vectors,
    int64_t n,
    int64_t dim,
    float* out_dists
) {
    for (int64_t i = 0; i < n; i++) {
        const float* v = vectors + i * dim;
        float sum = 0.0f;
        for (int64_t j = 0; j < dim; j++) {
            float d = query[j] - v[j];
            sum += d * d;
        }
        out_dists[i] = sum;
    }
}
