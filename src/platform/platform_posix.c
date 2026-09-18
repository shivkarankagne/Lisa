/* SPDX-License-Identifier: Apache-2.0 */
/*
 * POSIX implementation of the platform layer (macOS, Linux).
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int from_errno(int e) {
    switch (e) {
    case ENOENT:  return LISA_PLAT_ENOENT;
    case EEXIST:  return LISA_PLAT_EEXIST;
    case ENOMEM:  return LISA_PLAT_ENOMEM;
    case EINVAL:  return LISA_PLAT_EINVAL;
    case EWOULDBLOCK: return LISA_PLAT_EBUSY;
    default:      return LISA_PLAT_EIO;
    }
}

/* ---- paths ---------------------------------------------------------- */

char lisa_path_sep(void) {
    return '/';
}

char* lisa_path_join(const char* dir, const char* name) {
    if (dir == NULL || name == NULL) return NULL;
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    char* p = (char*)malloc(dl + 1 + nl + 1);
    if (p == NULL) return NULL;
    memcpy(p, dir, dl);
    p[dl] = '/';
    memcpy(p + dl + 1, name, nl);
    p[dl + 1 + nl] = '\0';
    return p;
}

int lisa_path_is_dir(const char* path) {
    struct stat st;
    if (path == NULL || stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int lisa_path_exists(const char* path) {
    struct stat st;
    if (path == NULL) return 0;
    return stat(path, &st) == 0 ? 1 : 0;
}

/* ---- directories and files ------------------------------------------ */

int lisa_mkdir(const char* path) {
    if (path == NULL || path[0] == '\0') return LISA_PLAT_EINVAL;
    if (mkdir(path, 0755) != 0) return from_errno(errno);
    return LISA_PLAT_OK;
}

int64_t lisa_file_size(const char* path) {
    struct stat st;
    if (path == NULL) return LISA_PLAT_EINVAL;
    if (stat(path, &st) != 0) return from_errno(errno);
    if (!S_ISREG(st.st_mode)) return LISA_PLAT_EINVAL;
    return (int64_t)st.st_size;
}

int lisa_file_sync(FILE* f) {
    if (f == NULL) return LISA_PLAT_EINVAL;
    if (fflush(f) != 0) return LISA_PLAT_EIO;
    int fd = fileno(f);
    if (fd < 0) return LISA_PLAT_EIO;
#ifdef F_FULLFSYNC
    /* On macOS, fsync does not flush the drive cache; F_FULLFSYNC does. */
    if (fcntl(fd, F_FULLFSYNC) == 0) return LISA_PLAT_OK;
#endif
    if (fsync(fd) != 0) return from_errno(errno);
    return LISA_PLAT_OK;
}

int lisa_rename_replace(const char* from, const char* to) {
    if (from == NULL || to == NULL) return LISA_PLAT_EINVAL;
    /* POSIX rename() atomically replaces an existing target. */
    if (rename(from, to) != 0) return from_errno(errno);
    return LISA_PLAT_OK;
}

int lisa_remove_file(const char* path) {
    if (path == NULL) return LISA_PLAT_EINVAL;
    if (unlink(path) != 0) return from_errno(errno);
    return LISA_PLAT_OK;
}

/* ---- memory mapping ------------------------------------------------- */

struct lisa_map {
    void* data;
    int64_t size;
};

int lisa_map_file(const char* path, int writable, lisa_map_t** out) {
    if (path == NULL || out == NULL) return LISA_PLAT_EINVAL;
    *out = NULL;

    int fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) return from_errno(errno);

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int rc = from_errno(errno);
        close(fd);
        return rc;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return LISA_PLAT_EINVAL;
    }

    lisa_map_t* m = (lisa_map_t*)calloc(1, sizeof(*m));
    if (m == NULL) {
        close(fd);
        return LISA_PLAT_ENOMEM;
    }
    m->size = (int64_t)st.st_size;

    if (m->size > 0) {
        int prot = PROT_READ | (writable ? PROT_WRITE : 0);
        void* p = mmap(NULL, (size_t)m->size, prot, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            int rc = from_errno(errno);
            free(m);
            close(fd);
            return rc;
        }
        m->data = p;
    }
    close(fd); /* the mapping stays valid after close */
    *out = m;
    return LISA_PLAT_OK;
}

void* lisa_map_data(const lisa_map_t* map) {
    return map ? map->data : NULL;
}

int64_t lisa_map_size(const lisa_map_t* map) {
    return map ? map->size : 0;
}

int lisa_map_sync(lisa_map_t* map) {
    if (map == NULL) return LISA_PLAT_EINVAL;
    if (map->size == 0) return LISA_PLAT_OK;
    if (msync(map->data, (size_t)map->size, MS_SYNC) != 0) return from_errno(errno);
    return LISA_PLAT_OK;
}

void lisa_unmap(lisa_map_t* map) {
    if (map == NULL) return;
    if (map->size > 0) munmap(map->data, (size_t)map->size);
    free(map);
}

/* ---- locking -------------------------------------------------------- */

struct lisa_lock {
    int fd;
};

int lisa_lock_acquire(const char* path, int wait, lisa_lock_t** out) {
    if (path == NULL || out == NULL) return LISA_PLAT_EINVAL;
    *out = NULL;

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return from_errno(errno);

    /*
     * flock locks belong to the open file description, so two handles in
     * the same process exclude each other, as do separate processes.
     */
    int rc;
    do {
        rc = flock(fd, LOCK_EX | (wait ? 0 : LOCK_NB));
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        int err = from_errno(errno);
        close(fd);
        return err;
    }

    lisa_lock_t* l = (lisa_lock_t*)malloc(sizeof(*l));
    if (l == NULL) {
        close(fd); /* closing releases the lock */
        return LISA_PLAT_ENOMEM;
    }
    l->fd = fd;
    *out = l;
    return LISA_PLAT_OK;
}

void lisa_lock_release(lisa_lock_t* lock) {
    if (lock == NULL) return;
    flock(lock->fd, LOCK_UN);
    close(lock->fd);
    free(lock);
}

/* ---- time ----------------------------------------------------------- */

int64_t lisa_time_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}
