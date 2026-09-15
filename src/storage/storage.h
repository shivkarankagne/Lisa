#ifndef LISA_STORAGE_H
#define LISA_STORAGE_H

/*
 * LISA Storage v1 — minimal persistent local collection.
 *
 * A collection is a directory containing:
 *
 *   header.bin   16 bytes
 *                offset 0  : 4 bytes magic "LISA"
 *                offset 4  : 4 bytes uint32 LE format version (1)
 *                offset 8  : 4 bytes int32  LE n
 *                offset 12 : 4 bytes int32  LE dim
 *
 *   vectors.bin  n * dim float32 LE, row-major
 *
 * The vector layout is identical to the CLI index file, so stored
 * vectors can be handed to lisa_search_exact_asm without conversion.
 *
 * Storage does not compute distances, does not do top-k, and does not
 * search. It stores and returns vectors. Retrieval owns search.
 *
 * Handles:
 *   A handle is a small integer that refers to an opened collection.
 *   Handles are allocated by storage_open and released by storage_close.
 *   Handle 0 is never valid.
 *
 * Return codes (all functions):
 *   0   success
 *   -1  invalid argument
 *   -2  file or directory error
 *   -3  format error (bad magic, bad version, truncated)
 *   -4  dimension or size mismatch
 *   -5  allocation failure
 *   -6  internal state error
 */

#include <stdint.h>

#define LISA_STORAGE_FORMAT_VERSION 1u

/*
 * Create a new collection on disk.
 *
 * path      : directory to create. Must not already exist.
 * n         : number of vectors, must be > 0
 * dim       : dimension of each vector, must be > 0
 * vectors   : pointer to n * dim float32, row-major
 *
 * Returns 0 on success, negative on error.
 */
int storage_create(const char* path, int n, int dim, const float* vectors);

/*
 * Open an existing collection on disk.
 *
 * path      : directory previously created by storage_create
 *
 * Returns a handle > 0 on success, negative on error.
 */
int storage_open(const char* path);

/*
 * Get the dimension of the collection.
 * Returns dim > 0, or negative on error.
 */
int storage_get_dim(int handle);

/*
 * Get the number of vectors in the collection.
 * Returns n > 0, or negative on error.
 */
int storage_get_n(int handle);

/*
 * Get a pointer to the vectors.
 * The pointer is valid until storage_close on this handle.
 * Returns NULL on error.
 */
const float* storage_get_vectors(int handle);

/*
 * Close the collection.
 * Returns 0 on success, negative on error.
 */
int storage_close(int handle);

#endif /* LISA_STORAGE_H */
