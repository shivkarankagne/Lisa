/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Windows implementation of the platform layer (Win32).
 *
 * Paths crossing this boundary are UTF-8 (the platform.h contract), but
 * the Win32 file APIs are UTF-16, so every path is converted with the
 * *W functions here. GUI-only entry points (folder picker, drag-drop)
 * and OCR are the no-op stubs in platform_ocr_none.c / the desktop stubs
 * below.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "platform.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>      /* SHGetKnownFolderPath */
#include <shobjidl.h>    /* IFileOpenDialog (folder picker) */
#include <shellapi.h>    /* ShellExecuteW */
#include <io.h>          /* _get_osfhandle */

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int from_win32(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:    return LISA_PLAT_ENOENT;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:    return LISA_PLAT_EEXIST;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:       return LISA_PLAT_ENOMEM;
    case ERROR_INVALID_PARAMETER: return LISA_PLAT_EINVAL;
    case ERROR_LOCK_VIOLATION:
    case ERROR_SHARING_VIOLATION: return LISA_PLAT_EBUSY;
    default:                      return LISA_PLAT_EIO;
    }
}

/* ---- UTF-8 <-> UTF-16 ------------------------------------------------ */

/* UTF-8 -> UTF-16, malloc'd; NULL on failure. */
static wchar_t* to_wide(const char* s) {
    if (s == NULL) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (w == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

/* UTF-16 -> UTF-8, malloc'd; NULL on failure. */
static char* to_utf8(const wchar_t* w) {
    if (w == NULL) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char* s = (char*)malloc((size_t)n);
    if (s == NULL) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
        free(s);
        return NULL;
    }
    return s;
}

/* ---- paths ---------------------------------------------------------- */

char lisa_path_sep(void) {
    return '\\';
}

int lisa_path_is_absolute(const char* path) {
    if (path == NULL) return 0;
    /* UNC path: "\\server\share" or "//server/share". */
    if ((path[0] == '\\' || path[0] == '/') &&
        (path[1] == '\\' || path[1] == '/')) return 1;
    /* Drive path: "C:\" or "C:/" (a drive-relative "C:foo" is not absolute). */
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/')) return 1;
    return 0;
}

char* lisa_path_join(const char* dir, const char* name) {
    if (dir == NULL || name == NULL) return NULL;
    size_t dl = strlen(dir), nl = strlen(name);
    char* p = (char*)malloc(dl + 1 + nl + 1);
    if (p == NULL) return NULL;
    memcpy(p, dir, dl);
    p[dl] = '\\';
    memcpy(p + dl + 1, name, nl);
    p[dl + 1 + nl] = '\0';
    return p;
}

static DWORD attrs_of(const char* path) {
    wchar_t* w = to_wide(path);
    if (w == NULL) return INVALID_FILE_ATTRIBUTES;
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a;
}

int lisa_path_is_dir(const char* path) {
    if (path == NULL) return 0;
    DWORD a = attrs_of(path);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

int lisa_path_exists(const char* path) {
    if (path == NULL) return 0;
    return attrs_of(path) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
}

char* lisa_path_absolute(const char* path) {
    if (path == NULL) return NULL;
    if (!lisa_path_exists(path)) return NULL;
    wchar_t* w = to_wide(path);
    if (w == NULL) return NULL;
    /* GetFullPathNameW canonicalises; it does not require existence, which
     * we checked above to match the POSIX realpath contract. */
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    if (n == 0) {
        free(w);
        return NULL;
    }
    wchar_t* full = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (full == NULL) {
        free(w);
        return NULL;
    }
    DWORD got = GetFullPathNameW(w, n, full, NULL);
    free(w);
    char* out = (got > 0 && got < n) ? to_utf8(full) : NULL;
    free(full);
    return out;
}

/* ---- directories and files ------------------------------------------ */

static int64_t filetime_to_ns(FILETIME ft) {
    /* FILETIME: 100-ns ticks since 1601-01-01. Convert to ns since Unix epoch. */
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    const ULONGLONG EPOCH_DIFF = 116444736000000000ULL; /* 1601->1970 in 100ns */
    if (t < EPOCH_DIFF) return 0;
    return (int64_t)((t - EPOCH_DIFF) * 100ULL);
}

int lisa_file_info(const char* path, lisa_file_info_t* out) {
    if (path == NULL || out == NULL) return LISA_PLAT_EINVAL;
    memset(out, 0, sizeof(*out));
    wchar_t* w = to_wide(path);
    if (w == NULL) return LISA_PLAT_ENOMEM;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &fad)) {
        int rc = from_win32(GetLastError());
        free(w);
        return rc;
    }
    DWORD a = fad.dwFileAttributes;
    out->is_symlink = (a & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
    out->is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    out->is_file = (!(a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) ? 1 : 0;
    out->size = ((int64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    out->mtime_ns = filetime_to_ns(fad.ftLastWriteTime);
    free(w);
    return LISA_PLAT_OK;
}

static int cmp_names(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* Sorted entry names of dir (UTF-8, without "." and ".."); caller frees. */
static int read_names(const char* dir, int include_hidden, char*** out, size_t* out_n) {
    *out = NULL;
    *out_n = 0;

    size_t dl = strlen(dir);
    char* pattern = (char*)malloc(dl + 3);
    if (pattern == NULL) return LISA_PLAT_ENOMEM;
    memcpy(pattern, dir, dl);
    pattern[dl] = '\\';
    pattern[dl + 1] = '*';
    pattern[dl + 2] = '\0';
    wchar_t* wpat = to_wide(pattern);
    free(pattern);
    if (wpat == NULL) return LISA_PLAT_ENOMEM;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    free(wpat);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return LISA_PLAT_OK; /* empty dir */
        return from_win32(e);
    }

    char** names = NULL;
    size_t n = 0, cap = 0;
    int rc = LISA_PLAT_OK;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char* name = to_utf8(fd.cFileName);
        if (name == NULL) { rc = LISA_PLAT_ENOMEM; break; }
        if (!include_hidden && name[0] == '.') { free(name); continue; }
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            char** g = (char**)realloc(names, cap * sizeof(char*));
            if (g == NULL) { free(name); rc = LISA_PLAT_ENOMEM; break; }
            names = g;
        }
        names[n++] = name;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

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
    if (depth > 256) return LISA_PLAT_OK;
    char** names = NULL;
    size_t n = 0;
    int rc = read_names(dir, include_hidden, &names, &n);
    if (rc != LISA_PLAT_OK) return rc;

    for (size_t i = 0; rc == LISA_PLAT_OK && i < n; i++) {
        char* path = lisa_path_join(dir, names[i]);
        if (path == NULL) { rc = LISA_PLAT_ENOMEM; break; }
        lisa_file_info_t info;
        if (lisa_file_info(path, &info) == LISA_PLAT_OK) {
            if (info.is_dir && !info.is_symlink) {
                rc = walk(path, include_hidden, fn, user, depth + 1);
                if (rc < 0 && rc != LISA_PLAT_ENOMEM) rc = LISA_PLAT_OK; /* unreadable subdir: skip */
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
        if (path == NULL) { rc = LISA_PLAT_ENOMEM; break; }
        int stop = fn(user, names[i], lisa_path_is_dir(path));
        free(path);
        if (stop != 0) rc = stop;
    }
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
    return rc;
}

int lisa_mkdir(const char* path) {
    if (path == NULL || path[0] == '\0') return LISA_PLAT_EINVAL;
    wchar_t* w = to_wide(path);
    if (w == NULL) return LISA_PLAT_ENOMEM;
    int rc = CreateDirectoryW(w, NULL) ? LISA_PLAT_OK : from_win32(GetLastError());
    free(w);
    return rc;
}

int lisa_mkdirs(const char* path) {
    if (path == NULL || path[0] == '\0') return LISA_PLAT_EINVAL;
    char* p = _strdup(path);
    if (p == NULL) return LISA_PLAT_ENOMEM;
    /* Normalise '/' to '\\' so both separators work. */
    for (char* c = p; *c; c++) if (*c == '/') *c = '\\';
    int rc = LISA_PLAT_OK;
    /* Skip a leading drive ("C:\") or UNC prefix. */
    char* start = p;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') start = p + 2;
    for (char* c = start + 1; rc == LISA_PLAT_OK; c++) {
        if (*c != '\\' && *c != '\0') continue;
        char saved = *c;
        *c = '\0';
        if (p[0] != '\0') {
            wchar_t* w = to_wide(p);
            if (w == NULL) { rc = LISA_PLAT_ENOMEM; }
            else {
                if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                    rc = from_win32(GetLastError());
                free(w);
            }
        }
        *c = saved;
        if (saved == '\0') break;
    }
    free(p);
    if (rc == LISA_PLAT_OK && !lisa_path_is_dir(path)) rc = LISA_PLAT_EEXIST;
    return rc;
}

int64_t lisa_file_size(const char* path) {
    if (path == NULL) return LISA_PLAT_EINVAL;
    lisa_file_info_t info;
    int rc = lisa_file_info(path, &info);
    if (rc != LISA_PLAT_OK) return rc;
    if (!info.is_file) return LISA_PLAT_EINVAL;
    return info.size;
}

int lisa_file_sync(FILE* f) {
    if (f == NULL) return LISA_PLAT_EINVAL;
    if (fflush(f) != 0) return LISA_PLAT_EIO;
    int fd = _fileno(f);
    if (fd < 0) return LISA_PLAT_EIO;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return LISA_PLAT_EIO;
    if (!FlushFileBuffers(h)) return from_win32(GetLastError());
    return LISA_PLAT_OK;
}

int lisa_file_seek(FILE* f, int64_t offset) {
    if (f == NULL || offset < 0) return LISA_PLAT_EINVAL;
    if (_fseeki64(f, (__int64)offset, SEEK_SET) != 0) return LISA_PLAT_EIO;
    return LISA_PLAT_OK;
}

int lisa_rename_replace(const char* from, const char* to) {
    if (from == NULL || to == NULL) return LISA_PLAT_EINVAL;
    wchar_t* wf = to_wide(from);
    wchar_t* wt = to_wide(to);
    int rc = LISA_PLAT_OK;
    if (wf == NULL || wt == NULL) rc = LISA_PLAT_ENOMEM;
    /* MOVEFILE_REPLACE_EXISTING replaces atomically on the same volume;
     * WRITE_THROUGH flushes so a crash leaves the old or new file. */
    else if (!MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        rc = from_win32(GetLastError());
    free(wf);
    free(wt);
    return rc;
}

int lisa_remove_file(const char* path) {
    if (path == NULL) return LISA_PLAT_EINVAL;
    wchar_t* w = to_wide(path);
    if (w == NULL) return LISA_PLAT_ENOMEM;
    int rc = DeleteFileW(w) ? LISA_PLAT_OK : from_win32(GetLastError());
    free(w);
    return rc;
}

/* ---- memory mapping ------------------------------------------------- */

struct lisa_map {
    HANDLE file;
    HANDLE mapping;
    void*  data;
    int64_t size;
    int    writable;
};

int lisa_map_file(const char* path, int writable, lisa_map_t** out) {
    if (path == NULL || out == NULL) return LISA_PLAT_EINVAL;
    *out = NULL;
    wchar_t* w = to_wide(path);
    if (w == NULL) return LISA_PLAT_ENOMEM;

    DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0);
    /* FILE_SHARE_DELETE lets compaction rename/delete a vector file while a
     * reader still has it mapped, matching POSIX unlink-open semantics; the
     * reader keeps working and moves to the new file on refresh. */
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE file = CreateFileW(w, access, share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (file == INVALID_HANDLE_VALUE) return from_win32(GetLastError());

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(file, &sz)) {
        int rc = from_win32(GetLastError());
        CloseHandle(file);
        return rc;
    }

    lisa_map_t* m = (lisa_map_t*)calloc(1, sizeof(*m));
    if (m == NULL) { CloseHandle(file); return LISA_PLAT_ENOMEM; }
    m->file = file;
    m->size = (int64_t)sz.QuadPart;
    m->writable = writable;

    if (m->size > 0) {
        DWORD prot = writable ? PAGE_READWRITE : PAGE_READONLY;
        HANDLE mapping = CreateFileMappingW(file, NULL, prot, 0, 0, NULL);
        if (mapping == NULL) {
            int rc = from_win32(GetLastError());
            CloseHandle(file);
            free(m);
            return rc;
        }
        DWORD view = writable ? FILE_MAP_WRITE : FILE_MAP_READ;
        void* p = MapViewOfFile(mapping, view, 0, 0, 0);
        if (p == NULL) {
            int rc = from_win32(GetLastError());
            CloseHandle(mapping);
            CloseHandle(file);
            free(m);
            return rc;
        }
        m->mapping = mapping;
        m->data = p;
    }
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
    if (!FlushViewOfFile(map->data, 0)) return from_win32(GetLastError());
    if (map->file != INVALID_HANDLE_VALUE && !FlushFileBuffers(map->file))
        return from_win32(GetLastError());
    return LISA_PLAT_OK;
}

void lisa_unmap(lisa_map_t* map) {
    if (map == NULL) return;
    if (map->data) UnmapViewOfFile(map->data);
    if (map->mapping) CloseHandle(map->mapping);
    if (map->file != INVALID_HANDLE_VALUE) CloseHandle(map->file);
    free(map);
}

/* ---- locking -------------------------------------------------------- */

struct lisa_lock {
    HANDLE handle;
};

int lisa_lock_acquire(const char* path, int wait, lisa_lock_t** out) {
    if (path == NULL || out == NULL) return LISA_PLAT_EINVAL;
    *out = NULL;
    wchar_t* w = to_wide(path);
    if (w == NULL) return LISA_PLAT_ENOMEM;

    /* Opened without FILE_SHARE_WRITE, so a second acquirer fails with a
     * sharing violation — an exclusive advisory lock across processes. */
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (!wait && (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION))
            return LISA_PLAT_EBUSY;
        return from_win32(e);
    }

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
    if (!LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        if (!wait && e == ERROR_LOCK_VIOLATION) return LISA_PLAT_EBUSY;
        return from_win32(e);
    }

    lisa_lock_t* l = (lisa_lock_t*)malloc(sizeof(*l));
    if (l == NULL) { CloseHandle(h); return LISA_PLAT_ENOMEM; }
    l->handle = h;
    *out = l;
    return LISA_PLAT_OK;
}

void lisa_lock_release(lisa_lock_t* lock) {
    if (lock == NULL) return;
    CloseHandle(lock->handle); /* closing releases the lock */
    free(lock);
}

/* ---- threads -------------------------------------------------------- */

struct lisa_thread {
    HANDLE handle;
    void (*fn)(void*);
    void*  arg;
};

static DWORD WINAPI thread_main(LPVOID p) {
    lisa_thread_t* t = (lisa_thread_t*)p;
    t->fn(t->arg);
    return 0;
}

int lisa_thread_start(void (*fn)(void* arg), void* arg, lisa_thread_t** out) {
    if (fn == NULL || out == NULL) return LISA_PLAT_EINVAL;
    *out = NULL;
    lisa_thread_t* t = (lisa_thread_t*)calloc(1, sizeof(*t));
    if (t == NULL) return LISA_PLAT_ENOMEM;
    t->fn = fn;
    t->arg = arg;
    t->handle = CreateThread(NULL, 0, thread_main, t, 0, NULL);
    if (t->handle == NULL) {
        free(t);
        return LISA_PLAT_EIO;
    }
    *out = t;
    return LISA_PLAT_OK;
}

void lisa_thread_join(lisa_thread_t* t) {
    if (t == NULL) return;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
}

struct lisa_mutex {
    CRITICAL_SECTION cs;
};

int lisa_mutex_create(lisa_mutex_t** out) {
    if (out == NULL) return LISA_PLAT_EINVAL;
    *out = NULL;
    lisa_mutex_t* m = (lisa_mutex_t*)calloc(1, sizeof(*m));
    if (m == NULL) return LISA_PLAT_ENOMEM;
    InitializeCriticalSection(&m->cs);
    *out = m;
    return LISA_PLAT_OK;
}

void lisa_mutex_lock(lisa_mutex_t* m)   { EnterCriticalSection(&m->cs); }
void lisa_mutex_unlock(lisa_mutex_t* m) { LeaveCriticalSection(&m->cs); }

void lisa_mutex_destroy(lisa_mutex_t* m) {
    if (m == NULL) return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

/* ---- process and environment --------------------------------------- */

char* lisa_default_data_dir(void) {
    /* %LOCALAPPDATA%\LISA */
    PWSTR local = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local) != S_OK) {
        if (local) CoTaskMemFree(local);
        return NULL;
    }
    char* base = to_utf8(local);
    CoTaskMemFree(local);
    if (base == NULL) return NULL;
    char* r = lisa_path_join(base, "LISA");
    free(base);
    return r;
}

char* lisa_executable_path(void) {
    DWORD cap = 512;
    for (;;) {
        wchar_t* buf = (wchar_t*)malloc(cap * sizeof(wchar_t));
        if (buf == NULL) return NULL;
        DWORD n = GetModuleFileNameW(NULL, buf, cap);
        if (n == 0) { free(buf); return NULL; }
        if (n < cap) {
            char* out = to_utf8(buf);
            free(buf);
            return out;
        }
        free(buf);            /* truncated: grow and retry */
        cap *= 2;
        if (cap > (1 << 20)) return NULL;
    }
}

int64_t lisa_process_id(void) {
    return (int64_t)GetCurrentProcessId();
}

int lisa_process_alive(int64_t pid) {
    if (pid <= 0) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (h == NULL) return 0;
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

int lisa_random_bytes(void* buf, size_t n) {
    if (buf == NULL && n > 0) return LISA_PLAT_EINVAL;
    if (n == 0) return LISA_PLAT_OK;
    NTSTATUS s = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                                 BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return s == 0 ? LISA_PLAT_OK : LISA_PLAT_EIO;
}

static volatile LONG g_stop;

static BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        InterlockedExchange(&g_stop, 1);
        return TRUE;
    }
    return FALSE;
}

int lisa_stop_signals_install(void) {
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) return LISA_PLAT_EIO;
    return LISA_PLAT_OK;
}

int lisa_stop_requested(void) {
    return InterlockedCompareExchange(&g_stop, 0, 0) != 0;
}

void lisa_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    Sleep((DWORD)ms);
}

int lisa_open_url(const char* url) {
    if (url == NULL || strncmp(url, "http", 4) != 0) return LISA_PLAT_EINVAL;
    wchar_t* w = to_wide(url);
    if (w == NULL) return LISA_PLAT_ENOMEM;
    HINSTANCE r = ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    return ((INT_PTR)r > 32) ? LISA_PLAT_OK : LISA_PLAT_EIO;
}

/* ---- desktop (no native GUI on Windows yet) ------------------------- */

/* Native "choose a folder" dialog via the Shell IFileOpenDialog, used by the
 * `lisa gui` window. Returns a malloc'd UTF-8 path, or NULL if the user
 * cancelled or the dialog could not be shown. Runs on the UI thread, where
 * the webview has already put COM into a single-threaded apartment. */
char* lisa_choose_folder(void) {
    char* result = NULL;
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int weInited = SUCCEEDED(hrInit) && hrInit != S_FALSE;

    IFileOpenDialog* dlg = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileOpenDialog, (void**)&dlg);
    if (SUCCEEDED(hr) && dlg != NULL) {
        DWORD opts = 0;
        dlg->lpVtbl->GetOptions(dlg, &opts);
        dlg->lpVtbl->SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dlg->lpVtbl->Show(dlg, NULL))) {
            IShellItem* item = NULL;
            if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) && item != NULL) {
                PWSTR wpath = NULL;
                if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &wpath)) &&
                    wpath != NULL) {
                    result = to_utf8(wpath);
                    CoTaskMemFree(wpath);
                }
                item->lpVtbl->Release(item);
            }
        }
        dlg->lpVtbl->Release(dlg);
    }

    if (weInited) CoUninitialize();
    return result;
}

int lisa_file_drops_install(void* native_view, lisa_drop_fn fn, void* user) {
    /* Drag-and-drop onto the native window is not wired up on Windows yet;
     * the GUI's folder picker (above) is the supported way to add a folder. */
    (void)native_view;
    (void)fn;
    (void)user;
    return LISA_PLAT_EINVAL;
}

/* OCR: no system recogniser wired up on Windows yet (platform_ocr_none.c
 * provides lisa_ocr_available/lisa_ocr_image). */

/* ---- time ----------------------------------------------------------- */

int64_t lisa_time_monotonic_ns(void) {
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    /* ticks -> ns without overflow: seconds part + fractional part. */
    int64_t sec = now.QuadPart / freq.QuadPart;
    int64_t rem = now.QuadPart % freq.QuadPart;
    return sec * 1000000000LL + (rem * 1000000000LL) / freq.QuadPart;
}
