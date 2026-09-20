/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_APP_LOG_H
#define LISA_APP_LOG_H
/*
 * The log for the lisa program (plan §6 W11). Lines go to stderr and, if
 * opened, to <data>/lisa.log:
 *
 *     2026-09-20T09:15:04Z  info   serve: listening on 127.0.0.1:8765
 *
 * The log records what LISA did, never document text, question text or
 * answers: a log file must be safe to send with a bug report.
 *
 * Errors and warnings also appear in the terminal; info and debug go to
 * the file only.
 *
 * Levels: error, warn, info (default), debug. Set with LISA_LOG_LEVEL or
 * `--log-level`. The file is capped (LOG_MAX_BYTES); when it is full it
 * is renamed to lisa.log.1 and a new one starts.
 */

#include <stdint.h>

typedef enum { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 } log_level_t;

#define LOG_MAX_BYTES (4 * 1024 * 1024)

/* Level from its name ("error".."debug"), or -1. */
int log_level_from_name(const char* name);

/*
 * Start logging at `level`. dir (may be NULL) is where lisa.log lives;
 * without it, or if the file cannot be opened, lines go to stderr only.
 * Returns the log file's path (owned by the log) or NULL.
 */
const char* log_open(const char* dir, log_level_t level);
void        log_close(void);

void log_write(log_level_t level, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define LOG_ERR(...)  log_write(LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...) log_write(LOG_WARN, __VA_ARGS__)
#define LOG_INFO(...) log_write(LOG_INFO, __VA_ARGS__)
#define LOG_DEBUG(...) log_write(LOG_DEBUG, __VA_ARGS__)

#endif /* LISA_APP_LOG_H */
