/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Kernel registry: selects one kernel table per process.
 */

#include "kernels_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const lisa_kernel_table_t k_scalar = { "scalar", lisa_l2_batch_scalar };

#ifdef LISA_HAVE_NEON
static const lisa_kernel_table_t k_neon = { "neon", lisa_l2_batch_neon };
#endif

/* Available tables, best first. */
static const lisa_kernel_table_t* const k_tables[] = {
#ifdef LISA_HAVE_NEON
    &k_neon,  /* NEON is architecturally mandatory on ARM64. */
#endif
    &k_scalar,
};

#define K_NUM_TABLES (sizeof(k_tables) / sizeof(k_tables[0]))

const lisa_kernel_table_t* lisa_kernels_by_name(const char* name) {
    if (name == NULL) return NULL;
    for (size_t i = 0; i < K_NUM_TABLES; i++) {
        if (strcmp(k_tables[i]->name, name) == 0) return k_tables[i];
    }
    return NULL;
}

static const lisa_kernel_table_t* select_table(void) {
    const lisa_kernel_table_t* t = lisa_kernels_by_name(getenv("LISA_KERNEL"));
    return t ? t : k_tables[0];
}

/*
 * The selection is a pure function of the build and the environment, so
 * concurrent first calls compute the same pointer.
 */
static _Atomic(const lisa_kernel_table_t*) g_selected = NULL;

const lisa_kernel_table_t* lisa_kernels(void) {
    const lisa_kernel_table_t* t =
        atomic_load_explicit(&g_selected, memory_order_acquire);
    if (t == NULL) {
        t = select_table();
        atomic_store_explicit(&g_selected, t, memory_order_release);
    }
    return t;
}
