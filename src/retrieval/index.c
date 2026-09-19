/* SPDX-License-Identifier: BUSL-1.1 */
#include "index.h"

static const lisa_index_ops_t k_exact = { "exact", lisa_search_masked };

const lisa_index_ops_t* lisa_index_exact(void) {
    return &k_exact;
}
