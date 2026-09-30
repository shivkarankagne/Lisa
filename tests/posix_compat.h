/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Test-only shim: the low-level file calls the tests use (access, close,
 * read, write) live in <unistd.h> on POSIX and <io.h>/<direct.h> on
 * Windows. Process id goes through the platform layer (lisa_process_id),
 * not getpid. Tests include this instead of <unistd.h>.
 */
#ifndef LISA_TEST_POSIX_COMPAT_H
#define LISA_TEST_POSIX_COMPAT_H

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

#endif
