/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_KERNELS_H
#define LISA_KERNELS_H

#include <stdint.h>

/*
 * LISA kernel registry.
 *
 * Every compute kernel is reached through a kernel table. Callers never
 * call a kernel implementation directly, so faster or platform-specific
 * kernels (x86 AVX, GPU, ...) can be added later by registering a new
 * table, without changing callers (plan §7, seam "kernel registry").
 *
 * Tables available in this build:
 *   "scalar"  portable C, always available
 *   "neon"    ARM64 NEON intrinsics (ARM64 builds only)
 */

/*
 * Squared L2 distance from one query to n contiguous vectors.
 *
 *   query:     dim floats
 *   vectors:   n * dim floats, row-major, no padding
 *   out_dists: n floats, caller-allocated
 *
 * Any dim >= 1 is supported. No memory is allocated.
 * Inputs are not validated; callers validate.
 */
typedef void (*lisa_l2_batch_fn)(
    const float* query,
    const float* vectors,
    int64_t n,
    int64_t dim,
    float* out_dists
);

typedef struct {
    const char* name;
    lisa_l2_batch_fn l2_batch;
} lisa_kernel_table_t;

/*
 * The kernel table selected for this machine.
 *
 * Selected once, on first call, by CPU capability. The environment
 * variable LISA_KERNEL (e.g. "scalar") overrides the choice; an unknown
 * name falls back to the default. Never returns NULL.
 * Thread-safe after the first call returns.
 */
const lisa_kernel_table_t* lisa_kernels(void);

/*
 * The kernel table with the given name, or NULL if it is not available
 * in this build. Intended for tests and benchmarks.
 */
const lisa_kernel_table_t* lisa_kernels_by_name(const char* name);

#endif /* LISA_KERNELS_H */
