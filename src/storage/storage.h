#ifndef LISA_STORAGE_H
#define LISA_STORAGE_H

/*
 * LISA Storage v1.1 — minimal persistent local collection
 * with insert and delete.
 *
 * A collection is a directory containing:
 *
 *   header.bin   20 bytes (version 2):
 *                offset 0  : 4 bytes magic "LISA"
 *                offset 4  : 4 bytes uint32 LE format version
 *                offset 8  : 4 bytes uint32 LE n          (live count)
 *                offset 12 : 4 bytes uint32 LE dim        (dimension)
 *                offset 16 : 4 bytes uint32 LE capacity   (slots allocated)
 *
 *   vectors.bin  capacity * dim float32 LE, row-major.
 *                Only the first n * dim floats are live.
 *                The tail up to capacity * dim is uninitialized.
 *
 * Backward compatibility:
 *   A version 1 file has a 16-byte header (no capacity field).
 *   A version 1 file is readable. capacity is treated as n.
 *   Insert and delete on a version 1 collection fail with -3.
 *
 * Known limitations (documented, not hidden):
 *
 *   1. storage_delete is O((n-1) * dim) I/O. It compacts the file
 *      and shifts all vectors after the deleted index down by one.
 *      Subsequent indices change. Anything that cached an index is
 *      now stale.
 *
 *   2. storage_insert is O(dim) when capacity allows, and
 *      O(capacity * dim) when the collection must grow. Growth is
 *      geometric: new_capacity = max(capacity * 2, capacity + 1).
 *      Amortized O(dim) per insert.
 *
 *   3. No concurrency control. A collection must be opened by one
 *      process at a time. Concurrent open + write can corrupt the file.
 *
 *   4. No crash recovery. A process killed during a growing insert
 *      can leave the collection in a half-written state.
 *
 *   5. The v1/v2 reader branch is a special case. It is tested, but
 *      it is not free.
 *
 * Storage does not compute distances, does not do top-k, and does not
 * search. It stores and returns vectors. Retrieval owns search.
 *
 * Return codes (all functions):
 *   0   success
 *   -1  invalid argument
 *   -2  file or directory error
 *   -3  format error, or operation not supported on this version
 *   -4  dimension or size mismatch
 *   -5  allocation failure
 *   -6  internal state error
 *   -7  index out of range (delete only)
 */

#include <stdint.h>

#define LISA_STORAGE_FORMAT_VERSION 2u

int storage_create(const char* path, int n, int dim, const float* vectors);

int storage_open(const char* path);

int storage_get_dim(int handle);

int storage_get_n(int handle);

const float* storage_get_vectors(int handle);

int storage_close(int handle);

/*
 * Insert one vector of dim values.
 * Returns the index of the inserted vector (>= 0), or negative on error.
 * If the collection is at capacity, it grows geometrically and the
 * file is rewritten.
 */
int storage_insert(int handle, const float* vector);

/*
 * Delete the vector at index.
 * Compacts the file. Subsequent indices shift down by one.
 * Returns 0 on success, negative on error.
 * Returns -7 if index is out of range.
 */
int storage_delete(int handle, int index);

#endif /* LISA_STORAGE_H */
