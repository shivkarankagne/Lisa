/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * search_asm_portable.c — a portable stand-in for the ARM64 assembly
 * search kernel, compiled everywhere the real one is not (i.e. off
 * Apple ARM64).
 *
 * The assembly kernel (src/kernels/arm64/) is a test-only path (plan
 * D2): several storage and edge-case tests call lisa_search_exact_asm as
 * a second implementation to check the product's results against. Off
 * Apple ARM64 that symbol does not exist, so those tests would not link.
 * Here it simply forwards to lisa_search_exact, the scalar reference,
 * which has the identical contract. The tests that exist specifically to
 * exercise the assembly (test_retrieval_asm, fuzz_retrieval_asm,
 * test_kernel_abi) are built only where the assembly is, so they never
 * reach this stand-in.
 */

#include "arm64/lisa_asm.h"
#include "../retrieval/retrieval.h"

int lisa_search_exact_asm(const float* query, const float* vectors, int n, int dim, int k,
                          lisa_result_t* result) {
    return lisa_search_exact(query, vectors, n, dim, k, result);
}
