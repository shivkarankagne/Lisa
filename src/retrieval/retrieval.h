#ifndef LISA_RETRIEVAL_H
#define LISA_RETRIEVAL_H

#include <stddef.h>

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

#endif /* LISA_RETRIEVAL_H */
