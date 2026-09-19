/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_KERNELS_INTERNAL_H
#define LISA_KERNELS_INTERNAL_H

/*
 * Kernel implementations. Internal to src/kernels/; everything else uses
 * the registry in kernels.h.
 */

#include "kernels.h"

void lisa_l2_batch_scalar(const float* query, const float* vectors,
                          int64_t n, int64_t dim, float* out_dists);

#if defined(__aarch64__) || defined(_M_ARM64)
#define LISA_HAVE_NEON 1
void lisa_l2_batch_neon(const float* query, const float* vectors,
                        int64_t n, int64_t dim, float* out_dists);
#endif

#endif /* LISA_KERNELS_INTERNAL_H */
