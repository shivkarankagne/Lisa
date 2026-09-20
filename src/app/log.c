/* SPDX-License-Identifier: BUSL-1.1 */
/* The program log (log.h). */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../platform/platform.h"

static const char* const k_names[] = { "error", "warn", "info", "debug" };

static FILE*        g_file;
static char*        g_path;
static log_level_t  g_level = LOG_INFO;
static int64_t      g_written;
static lisa_mutex_t* g_mutex;

int log_level_from_name(const char* name) {
    if (name == NULL) return -1;
    for (int i = 0; i < 4; i++) {
        if (strcmp(name, k_names[i]) == 0) return i;
    }
    return -1;
}

const char* log_open(const char* dir, log_level_t level) {
    log_close();
    g_level = level;
    const char* env = getenv("LISA_LOG_LEVEL");
    int from_env = log_level_from_name(env);
    if (from_env >= 0) g_level = (log_level_t)from_env;
    if (lisa_mutex_create(&g_mutex) != LISA_PLAT_OK) return NULL;
    if (dir == NULL) return NULL;
    g_path = lisa_path_join(dir, "lisa.log");
    if (g_path == NULL) return NULL;
    int64_t size = lisa_file_size(g_path);
    g_written = size > 0 ? size : 0;
    g_file = fopen(g_path, "ab");
    if (g_file == NULL) {
        free(g_path);
        g_path = NULL;
        return NULL;
    }
    return g_path;
}

void log_close(void) {
    if (g_file) fclose(g_file);
    g_file = NULL;
    free(g_path);
    g_path = NULL;
    lisa_mutex_destroy(g_mutex);
    g_mutex = NULL;
    g_written = 0;
}

/* Start a new file when the old one is full; keep one previous file. */
static void roll_if_full(void) {
    if (g_file == NULL || g_written < LOG_MAX_BYTES) return;
    size_t n = strlen(g_path) + 3;
    char* old = (char*)malloc(n);
    if (old == NULL) return;
    snprintf(old, n, "%s.1", g_path);
    fclose(g_file);
    lisa_rename_replace(g_path, old);
    free(old);
    g_file = fopen(g_path, "ab");
    g_written = 0;
}

void log_write(log_level_t level, const char* fmt, ...) {
    if (level > g_level) return;
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);

    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    if (g_mutex) lisa_mutex_lock(g_mutex);
    /* The terminal gets what the user must see; the file gets everything. */
    if (level <= LOG_WARN) fprintf(stderr, "lisa: %s: %s\n", k_names[level], line);
    if (g_file) {
        int written = fprintf(g_file, "%s  %-5s  %s\n", stamp, k_names[level], line);
        fflush(g_file);
        if (written > 0) g_written += written;
        roll_if_full();
    }
    if (g_mutex) lisa_mutex_unlock(g_mutex);
}
