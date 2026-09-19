/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * POSIX implementation of the platform layer (macOS, Linux).
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "platform.h"

#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>   /* _NSGetExecutablePath */
#include <sys/random.h>    /* getentropy */
#endif

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

char* lisa_path_absolute(const char* path) {
    if (path == NULL) return NULL;
    char buf[PATH_MAX];
    if (realpath(path, buf) == NULL) return NULL;
    size_t n = strlen(buf) + 1;
    char* out = (char*)malloc(n);
    if (out) memcpy(out, buf, n);
    return out;
}

/* ---- directories and files ------------------------------------------ */

static int64_t mtime_ns_of(const struct stat* st) {
#ifdef __APPLE__
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
#else
    return (int64_t)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#endif
}

int lisa_file_info(const char* path, lisa_file_info_t* out) {
    if (path == NULL || out == NULL) return LISA_PLAT_EINVAL;
    memset(out, 0, sizeof(*out));
    struct stat ls, st;
    if (lstat(path, &ls) != 0) return from_errno(errno);
    out->is_symlink = S_ISLNK(ls.st_mode) ? 1 : 0;
    if (stat(path, &st) != 0) return from_errno(errno);  /* dangling symlink */
    out->is_file = S_ISREG(st.st_mode) ? 1 : 0;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->size = (int64_t)st.st_size;
    out->mtime_ns = mtime_ns_of(&st);
    return LISA_PLAT_OK;
}

static int cmp_names(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* Sorted entry names of dir (without "." and ".."); caller frees. */
static int read_names(const char* dir, int include_hidden, char*** out, size_t* out_n) {
    *out = NULL;
    *out_n = 0;
    DIR* d = opendir(dir);
    if (d == NULL) return from_errno(errno);

    char** names = NULL;
    size_t n = 0, cap = 0;
    int rc = LISA_PLAT_OK;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!include_hidden && e->d_name[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            char** g = (char**)realloc(names, cap * sizeof(char*));
            if (g == NULL) {
                rc = LISA_PLAT_ENOMEM;
                break;
            }
            names = g;
        }
        names[n] = strdup(e->d_name);
        if (names[n] == NULL) {
            rc = LISA_PLAT_ENOMEM;
            break;
        }
        n++;
    }
    closedir(d);
    if (rc != LISA_PLAT_OK) {
        for (size_t i = 0; i < n; i++) free(names[i]);
        free(names);
        return rc;
    }
    if (n > 0) qsort(names, n, sizeof(char*), cmp_names);
    *out = names;
    *out_n = n;
    return LISA_PLAT_OK;
}

static int walk(const char* dir, int include_hidden, lisa_walk_fn fn, void* user, int depth) {
    if (depth > 256) return LISA_PLAT_OK;  /* pathological nesting */
    char** names = NULL;
    size_t n = 0;
    int rc = read_names(dir, include_hidden, &names, &n);
    if (rc != LISA_PLAT_OK) return rc;

    for (size_t i = 0; rc == LISA_PLAT_OK && i < n; i++) {
        char* path = lisa_path_join(dir, names[i]);
        if (path == NULL) {
            rc = LISA_PLAT_ENOMEM;
            break;
        }
        lisa_file_info_t info;
        if (lisa_file_info(path, &info) == LISA_PLAT_OK) {
            if (info.is_dir && !info.is_symlink) {
                rc = walk(path, include_hidden, fn, user, depth + 1);
                if (rc < 0 && rc != LISA_PLAT_ENOMEM) rc = LISA_PLAT_OK;  /* unreadable subdir: skip */
            } else if (info.is_file) {
                int stop = fn(user, path, &info);
                if (stop != 0) rc = stop;
            }
        }
        free(path);
    }
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
    return rc;
}

int lisa_dir_walk(const char* root, int include_hidden, lisa_walk_fn fn, void* user) {
    if (root == NULL || fn == NULL) return LISA_PLAT_EINVAL;
    lisa_file_info_t info;
    int rc = lisa_file_info(root, &info);
    if (rc != LISA_PLAT_OK) return rc;
    if (!info.is_dir) return LISA_PLAT_EINVAL;
    return walk(root, include_hidden, fn, user, 0);
}

int lisa_dir_list(const char* dir, lisa_list_fn fn, void* user) {
    if (dir == NULL || fn == NULL) return LISA_PLAT_EINVAL;
    char** names = NULL;
    size_t n = 0;
    int rc = read_names(dir, 1, &names, &n);
    for (size_t i = 0; rc == LISA_PLAT_OK && i < n; i++) {
        char* path = lisa_path_join(dir, names[i]);
        if (path == NULL) {
            rc = LISA_PLAT_ENOMEM;
            break;
        }
        int stop = fn(user, names[i], lisa_path_is_dir(path));
        free(path);
        if (stop != 0) rc = stop;
    }
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
    return rc;
}

int lisa_mkdirs(const char* path) {
    if (path == NULL || path[0] == '\0') return LISA_PLAT_EINVAL;
    char* p = strdup(path);
    if (p == NULL) return LISA_PLAT_ENOMEM;
    int rc = LISA_PLAT_OK;
    for (char* c = p + 1; rc == LISA_PLAT_OK; c++) {
        if (*c != '/' && *c != '\0') continue;
        char saved = *c;
        *c = '\0';
        if (mkdir(p, 0755) != 0 && errno != EEXIST) rc = from_errno(errno);
        *c = saved;
        if (saved == '\0') break;
    }
    free(p);
    if (rc == LISA_PLAT_OK && !lisa_path_is_dir(path)) rc = LISA_PLAT_EEXIST;   /* a file is in the way */
    return rc;
}

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

int lisa_file_seek(FILE* f, int64_t offset) {
    if (f == NULL || offset < 0) return LISA_PLAT_EINVAL;
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) return from_errno(errno);
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

/* ---- threads -------------------------------------------------------- */

struct lisa_thread {
    pthread_t id;
    void    (*fn)(void*);
    void*    arg;
};

static void* thread_main(void* p) {
    lisa_thread_t* t = (lisa_thread_t*)p;
    t->fn(t->arg);
    return NULL;
}

int lisa_thread_start(void (*fn)(void* arg), void* arg, lisa_thread_t** out) {
    if (fn == NULL || out == NULL) return LISA_PLAT_EINVAL;
    *out = NULL;
    lisa_thread_t* t = (lisa_thread_t*)calloc(1, sizeof(*t));
    if (t == NULL) return LISA_PLAT_ENOMEM;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->id, NULL, thread_main, t) != 0) {
        free(t);
        return LISA_PLAT_EIO;
    }
    *out = t;
    return LISA_PLAT_OK;
}

void lisa_thread_join(lisa_thread_t* t) {
    if (t == NULL) return;
    pthread_join(t->id, NULL);
    free(t);
}

struct lisa_mutex {
    pthread_mutex_t m;
};

int lisa_mutex_create(lisa_mutex_t** out) {
    if (out == NULL) return LISA_PLAT_EINVAL;
    *out = NULL;
    lisa_mutex_t* m = (lisa_mutex_t*)calloc(1, sizeof(*m));
    if (m == NULL) return LISA_PLAT_ENOMEM;
    if (pthread_mutex_init(&m->m, NULL) != 0) {
        free(m);
        return LISA_PLAT_EIO;
    }
    *out = m;
    return LISA_PLAT_OK;
}

void lisa_mutex_lock(lisa_mutex_t* m) {
    pthread_mutex_lock(&m->m);
}

void lisa_mutex_unlock(lisa_mutex_t* m) {
    pthread_mutex_unlock(&m->m);
}

void lisa_mutex_destroy(lisa_mutex_t* m) {
    if (m == NULL) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

/* ---- process and environment --------------------------------------- */

static char* concat(const char* a, const char* b) {
    size_t na = strlen(a), nb = strlen(b);
    char* r = (char*)malloc(na + nb + 1);
    if (r == NULL) return NULL;
    memcpy(r, a, na);
    memcpy(r + na, b, nb + 1);
    return r;
}

char* lisa_default_data_dir(void) {
    const char* home = getenv("HOME");
#ifdef __APPLE__
    if (home == NULL || home[0] == '\0') return NULL;
    return concat(home, "/Library/Application Support/LISA");
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg != NULL && xdg[0] == '/') return concat(xdg, "/lisa");
    if (home == NULL || home[0] == '\0') return NULL;
    return concat(home, "/.local/share/lisa");
#endif
}

char* lisa_executable_path(void) {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char* raw = (char*)malloc(size + 1);
    if (raw == NULL) return NULL;
    if (_NSGetExecutablePath(raw, &size) != 0) {
        free(raw);
        return NULL;
    }
    char* real = realpath(raw, NULL);
    free(raw);
    return real;
#else
    return realpath("/proc/self/exe", NULL);
#endif
}

int lisa_random_bytes(void* buf, size_t n) {
    if (buf == NULL && n > 0) return LISA_PLAT_EINVAL;
    unsigned char* p = (unsigned char*)buf;
    while (n > 0) {
        size_t k = n < 256 ? n : 256;   /* getentropy's limit per call */
        if (getentropy(p, k) != 0) return LISA_PLAT_EIO;
        p += k;
        n -= k;
    }
    return LISA_PLAT_OK;
}

static volatile sig_atomic_t g_stop;

static void on_stop_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

int lisa_stop_signals_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) return LISA_PLAT_EIO;
    signal(SIGPIPE, SIG_IGN);   /* a closed client socket must not kill the server */
    return LISA_PLAT_OK;
}

int lisa_stop_requested(void) {
    return g_stop != 0;
}

void lisa_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !g_stop) {}
}

/* ---- time ----------------------------------------------------------- */

int64_t lisa_time_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}
