#ifndef LISA_RETRIEVAL_H
#define LISA_RETRIEVAL_H

#include <stddef.h>
#include <stdint.h>

/*
 * LISA Retrieval API — stable public interface
 *
 * All retrieval implementations (scalar, SIMD, assembly) must conform
 * to this interface. This is the contract between the retrieval engine
 * and its consumers.
 */

/*
 * Result of a top-k search.
 *
 * indices: caller-allocated array of size k
 * dists:   caller-allocated array of size k
 *
 * On return, indices[i] and dists[i] contain the i-th nearest neighbor
 * and its L2 squared distance, sorted ascending by distance.
 *
 * If n < k, only n results are written; the caller must check the
 * return value.
 */
typedef struct {
    int* indices;
    float* dists;
    int k;
    int n_returned;
} lisa_result_t;

/*
 * Compute L2 squared distance between two vectors.
 *
 * a, b:   pointers to float arrays of length dim
 * dim:    dimension (must be > 0)
 *
 * Returns: sum((a[i] - b[i])^2)
 */
float lisa_l2_squared(const float* a, const float* b, int dim);

/*
 * Brute-force exact top-k search.
 *
 * query:   pointer to float array of length dim
 * vectors: pointer to n * dim float array (row-major)
 * n:       number of vectors
 * dim:     dimension
 * k:       number of results requested
 * result:  caller-allocated lisa_result_t
 *
 * Returns: 0 on success, negative on error
 *
 * Errors:
 *   -1: query, vectors, or result is NULL
 *   -2: n <= 0, dim <= 0, or k <= 0
 *   -3: result->indices or result->dists is NULL
 */
int lisa_search_exact(
    const float* query,
    const float* vectors,
    int n,
    int dim,
    int k,
    lisa_result_t* result
);

/*
 * Exact top-k search using the fastest kernel available on this machine
 * (see src/kernels/kernels.h). This is the search used by the product;
 * lisa_search_exact above remains the scalar reference it is tested
 * against.
 *
 * Same contract as lisa_search_exact, with 64-bit sizes. Results agree
 * with lisa_search_exact up to floating-point rounding (different
 * summation order). The dataset is never copied; memory use is O(k).
 *
 * Errors:
 *   -1: query, vectors, or result is NULL
 *   -2: n <= 0, dim <= 0, k <= 0, or n > INT32_MAX (indices are int)
 *   -3: result->indices or result->dists is NULL
 *   -4: allocation failure (only when k > 256)
 */
int lisa_search(
    const float* query,
    const float* vectors,
    int64_t n,
    int64_t dim,
    int64_t k,
    lisa_result_t* result
);

/*
 * lisa_search restricted to vectors whose live[i] is non-zero.
 *
 * live: n bytes, or NULL for all vectors (then identical to lisa_search).
 * Skipped vectors are never returned. If no vector is live, returns 0
 * with n_returned = 0. Same errors as lisa_search. Used by storage v2 to
 * skip deleted slots; also the base for metadata filtering (W7).
 */
int lisa_search_masked(
    const float* query,
    const float* vectors,
    int64_t n,
    int64_t dim,
    int64_t k,
    const uint8_t* live,
    lisa_result_t* result
);

#endif /* LISA_RETRIEVAL_H */
