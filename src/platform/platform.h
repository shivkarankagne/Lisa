/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LISA_PLATFORM_H
#define LISA_PLATFORM_H

/*
 * LISA platform layer.
 *
 * Every operating-system call LISA makes goes through this header, so
 * that supporting a new OS means adding one implementation file
 * (platform_windows.c, ...) without touching callers (plan §2a P1).
 *
 * Implementations in this build: platform_posix.c (macOS, Linux).
 *
 * Standard C I/O (fopen/fread/fwrite/fclose) is portable and may be used
 * directly. Everything else — directories, file metadata, durability,
 * memory mapping, locking, clocks — goes through here.
 *
 * Paths are UTF-8. All functions are thread-safe.
 *
 * Return codes: 0 on success, or one of the negative LISA_PLAT_E* codes.
 */

#include <stdint.h>
#include <stdio.h>

#define LISA_PLAT_OK        0
#define LISA_PLAT_EINVAL   -1   /* invalid argument */
#define LISA_PLAT_ENOENT   -2   /* path does not exist */
#define LISA_PLAT_EEXIST   -3   /* path already exists */
#define LISA_PLAT_EIO      -4   /* other I/O or OS error */
#define LISA_PLAT_ENOMEM   -5   /* allocation failure */
#define LISA_PLAT_EBUSY    -6   /* lock held by someone else */

/* ---- paths ---------------------------------------------------------- */

/* The path separator for this OS ('/' on POSIX). */
char lisa_path_sep(void);

/*
 * dir + separator + name. Returns a malloc'd string the caller frees, or
 * NULL on invalid argument or allocation failure.
 */
char* lisa_path_join(const char* dir, const char* name);

/* 1 if path exists and is a directory, else 0. */
int lisa_path_is_dir(const char* path);

/* 1 if path exists (file, directory, or other), else 0. */
int lisa_path_exists(const char* path);

/* ---- directories and files ------------------------------------------ */

/* Create one directory (parent must exist). EEXIST if it already exists. */
int lisa_mkdir(const char* path);

/* Size of a regular file in bytes, or a negative LISA_PLAT_E* code. */
int64_t lisa_file_size(const char* path);

/*
 * Flush a stdio stream and force its data to stable storage (fsync).
 * The stream stays open.
 */
int lisa_file_sync(FILE* f);

/*
 * Set the position of a stdio stream to an absolute 64-bit byte offset.
 * (fseek takes a long, which is 32-bit on some platforms.)
 */
int lisa_file_seek(FILE* f, int64_t offset);

/*
 * Atomically replace `to` with `from` (both on the same filesystem).
 * After a crash, `to` is either the old or the new file, never partial.
 */
int lisa_rename_replace(const char* from, const char* to);

/* Remove a file. ENOENT if it does not exist. */
int lisa_remove_file(const char* path);

/* ---- memory mapping ------------------------------------------------- */

typedef struct lisa_map lisa_map_t;

/*
 * Map a whole existing file into memory.
 *
 *   writable = 0: read-only mapping
 *   writable = 1: read-write, shared (writes reach the file)
 *
 * On success *out receives a handle the caller releases with
 * lisa_unmap(). An empty file maps successfully with size 0 and a NULL
 * data pointer.
 */
int lisa_map_file(const char* path, int writable, lisa_map_t** out);

/* Mapped bytes (valid until lisa_unmap). */
void* lisa_map_data(const lisa_map_t* map);

/* Mapped size in bytes. */
int64_t lisa_map_size(const lisa_map_t* map);

/* Flush a writable mapping to stable storage. */
int lisa_map_sync(lisa_map_t* map);

/* Release a mapping. NULL is ignored. */
void lisa_unmap(lisa_map_t* map);

/* ---- locking -------------------------------------------------------- */

typedef struct lisa_lock lisa_lock_t;

/*
 * Take an exclusive advisory lock on `path` (created if missing), shared
 * between processes. If wait is 0 and another process or handle holds
 * the lock, returns EBUSY immediately. The lock is released by
 * lisa_lock_release() or when the process exits.
 */
int lisa_lock_acquire(const char* path, int wait, lisa_lock_t** out);

/* Release a lock. NULL is ignored. */
void lisa_lock_release(lisa_lock_t* lock);

/* ---- time ----------------------------------------------------------- */

/* Monotonic clock in nanoseconds. Only differences are meaningful. */
int64_t lisa_time_monotonic_ns(void);

#endif /* LISA_PLATFORM_H */
