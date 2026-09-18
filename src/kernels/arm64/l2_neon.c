/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ARM64 NEON intrinsics kernels.
 *
 * Intrinsics rather than assembly (plan §2a P3): the same source builds on
 * macOS, Linux, and Windows ARM64, and the compiler handles the calling
 * convention. Handles any dim: 16 floats per step with four independent
 * accumulators, then 4 floats per step, then a scalar tail. No padding,
 * no copies, no allocation.
 */

#include "../kernels_internal.h"

#ifdef LISA_HAVE_NEON

#include <arm_neon.h>

static inline float l2_one(const float* q, const float* v, int64_t dim) {
    float32x4_t acc0 = vdupq_n_f32(0.0f);
    float32x4_t acc1 = vdupq_n_f32(0.0f);
    float32x4_t acc2 = vdupq_n_f32(0.0f);
    float32x4_t acc3 = vdupq_n_f32(0.0f);
    int64_t j = 0;

    for (; j + 16 <= dim; j += 16) {
        float32x4_t d0 = vsubq_f32(vld1q_f32(q + j),      vld1q_f32(v + j));
        float32x4_t d1 = vsubq_f32(vld1q_f32(q + j + 4),  vld1q_f32(v + j + 4));
        float32x4_t d2 = vsubq_f32(vld1q_f32(q + j + 8),  vld1q_f32(v + j + 8));
        float32x4_t d3 = vsubq_f32(vld1q_f32(q + j + 12), vld1q_f32(v + j + 12));
        acc0 = vfmaq_f32(acc0, d0, d0);
        acc1 = vfmaq_f32(acc1, d1, d1);
        acc2 = vfmaq_f32(acc2, d2, d2);
        acc3 = vfmaq_f32(acc3, d3, d3);
    }
    for (; j + 4 <= dim; j += 4) {
        float32x4_t d = vsubq_f32(vld1q_f32(q + j), vld1q_f32(v + j));
        acc0 = vfmaq_f32(acc0, d, d);
    }

    float sum = vaddvq_f32(vaddq_f32(vaddq_f32(acc0, acc1),
                                     vaddq_f32(acc2, acc3)));
    for (; j < dim; j++) {
        float d = q[j] - v[j];
        sum += d * d;
    }
    return sum;
}

void lisa_l2_batch_neon(
    const float* query,
    const float* vectors,
    int64_t n,
    int64_t dim,
    float* out_dists
) {
    for (int64_t i = 0; i < n; i++) {
        out_dists[i] = l2_one(query, vectors + i * dim, dim);
    }
}

#endif /* LISA_HAVE_NEON */
