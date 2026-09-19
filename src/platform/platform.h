/* SPDX-License-Identifier: BUSL-1.1 */
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

#include <stddef.h>
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

/* Absolute, canonical form of an existing path (symlinks resolved).
 * Returns a malloc'd string, or NULL if the path does not exist. */
char* lisa_path_absolute(const char* path);

/* ---- directories and files ------------------------------------------ */

typedef struct {
    int64_t size;       /* bytes (regular files) */
    int64_t mtime_ns;   /* last modification, nanoseconds since the epoch */
    int     is_file;    /* regular file */
    int     is_dir;
    int     is_symlink; /* the path itself is a symbolic link */
} lisa_file_info_t;

/* Information about path (following symlinks for size/type). */
int lisa_file_info(const char* path, lisa_file_info_t* out);

/*
 * Called for each regular file found by lisa_dir_walk. path is valid only
 * during the call. Return 0 to continue, non-zero to stop the walk (the
 * walk then returns that value).
 */
typedef int (*lisa_walk_fn)(void* user, const char* path, const lisa_file_info_t* info);

/*
 * Visit every regular file under root, recursively, in a deterministic
 * order (entries sorted by name). Names starting with "." are skipped
 * unless include_hidden is set. Symbolic links to directories are not
 * followed (no cycles); symbolic links to files are visited.
 */
int lisa_dir_walk(const char* root, int include_hidden, lisa_walk_fn fn, void* user);

/* Create one directory (parent must exist). EEXIST if it already exists. */
int lisa_mkdir(const char* path);

/* Create a directory and any missing parents. OK if it already exists. */
int lisa_mkdirs(const char* path);

/*
 * Visit the entries of one directory (not recursive), sorted by name,
 * without "." and "..". Return non-zero from fn to stop (that value is
 * returned).
 */
typedef int (*lisa_list_fn)(void* user, const char* name, int is_dir);
int lisa_dir_list(const char* dir, lisa_list_fn fn, void* user);

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

/* ---- threads -------------------------------------------------------- */

typedef struct lisa_thread lisa_thread_t;

/* Start fn(arg) on a new thread. Join it with lisa_thread_join. */
int lisa_thread_start(void (*fn)(void* arg), void* arg, lisa_thread_t** out);

/* Wait for the thread to finish and release it. NULL is ignored. */
void lisa_thread_join(lisa_thread_t* t);

typedef struct lisa_mutex lisa_mutex_t;

int  lisa_mutex_create(lisa_mutex_t** out);
void lisa_mutex_lock(lisa_mutex_t* m);
void lisa_mutex_unlock(lisa_mutex_t* m);
void lisa_mutex_destroy(lisa_mutex_t* m);   /* NULL is ignored */

/* ---- process and environment --------------------------------------- */

/*
 * The per-user directory for LISA's data, malloc'd (NULL if it cannot be
 * determined): ~/Library/Application Support/LISA on macOS,
 * $XDG_DATA_HOME/lisa or ~/.local/share/lisa elsewhere. Not created.
 */
char* lisa_default_data_dir(void);

/* Absolute path of the running executable, malloc'd; NULL if unknown. */
char* lisa_executable_path(void);

/* Fill buf with n cryptographically secure random bytes. */
int lisa_random_bytes(void* buf, size_t n);

/*
 * After lisa_stop_signals_install(), an interrupt or termination request
 * (Ctrl-C, SIGTERM) sets a flag instead of killing the process;
 * lisa_stop_requested() reports it.
 */
int lisa_stop_signals_install(void);
int lisa_stop_requested(void);

/* Sleep for ms milliseconds. */
void lisa_sleep_ms(int64_t ms);

/* ---- time ----------------------------------------------------------- */

/* Monotonic clock in nanoseconds. Only differences are meaningful. */
int64_t lisa_time_monotonic_ns(void);

#endif /* LISA_PLATFORM_H */
