/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_platform — platform layer contract.
 *
 * Usage: test_platform <scratch_dir>
 * Creates a fresh subdirectory under <scratch_dir> (ctest passes one in
 * the build tree) and works only inside it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/platform/platform.h"

static int g_fail = 0;

static void check(int cond, const char* what) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail = 1;
}

static int write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n && lisa_file_sync(f) == LISA_PLAT_OK;
    fclose(f);
    return ok ? 0 : -1;
}

static int read_equals(const char* path, const char* text) {
    char buf[256] = { 0 };
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return n == strlen(text) && memcmp(buf, text, n) == 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch_dir>\n", argv[0]);
        return 2;
    }
    printf("Platform tests\n");

    char name[64];
    snprintf(name, sizeof(name), "platform_%lld", (long long)lisa_time_monotonic_ns());
    char* dir = lisa_path_join(argv[1], name);
    char* a = lisa_path_join(dir, "a.txt");
    char* b = lisa_path_join(dir, "b.txt");
    char* e = lisa_path_join(dir, "empty.bin");
    char* lk = lisa_path_join(dir, "lock");
    char* missing = lisa_path_join(dir, "missing");
    char* nested = lisa_path_join(missing, "child");
    if (!dir || !a || !b || !e || !lk || !missing || !nested) {
        printf("FAIL: allocation\n");
        return 1;
    }

    /* paths */
    check(lisa_path_sep() == '/', "path separator");
    char* j = lisa_path_join("x", "y");
    check(j && strcmp(j, "x/y") == 0, "path_join");
    free(j);
    check(lisa_path_join(NULL, "y") == NULL, "path_join(NULL) is NULL");

    /* directories */
    check(lisa_mkdir(argv[1]) == LISA_PLAT_OK || lisa_path_is_dir(argv[1]), "scratch dir exists");
    check(lisa_mkdir(dir) == LISA_PLAT_OK, "mkdir new");
    check(lisa_mkdir(dir) == LISA_PLAT_EEXIST, "mkdir existing -> EEXIST");
    check(lisa_mkdir(nested) == LISA_PLAT_ENOENT, "mkdir without parent -> ENOENT");
    check(lisa_mkdir("") == LISA_PLAT_EINVAL, "mkdir empty -> EINVAL");
    check(lisa_path_is_dir(dir) == 1, "is_dir on dir");
    check(lisa_path_exists(missing) == 0, "exists on missing");

    /* files */
    check(write_file(a, "hello") == 0, "write + file_sync");
    check(lisa_path_is_dir(a) == 0 && lisa_path_exists(a) == 1, "is_dir/exists on file");
    check(lisa_file_size(a) == 5, "file_size");
    check(lisa_file_size(missing) == LISA_PLAT_ENOENT, "file_size missing -> ENOENT");
    check(lisa_file_size(dir) == LISA_PLAT_EINVAL, "file_size dir -> EINVAL");
    check(lisa_file_sync(NULL) == LISA_PLAT_EINVAL, "file_sync(NULL) -> EINVAL");

    /* 64-bit seek */
    FILE* sf = fopen(a, "r+b");
    char c = 0;
    check(sf && lisa_file_seek(sf, 4) == LISA_PLAT_OK && fread(&c, 1, 1, sf) == 1 && c == 'o',
          "file_seek to offset");
    check(lisa_file_seek(sf, -1) == LISA_PLAT_EINVAL, "file_seek negative -> EINVAL");
    if (sf) fclose(sf);

    /* rename_replace */
    check(write_file(b, "old contents") == 0, "write second file");
    check(lisa_rename_replace(a, b) == LISA_PLAT_OK, "rename_replace over existing");
    check(read_equals(b, "hello") && !lisa_path_exists(a), "target replaced, source gone");
    check(lisa_rename_replace(a, b) == LISA_PLAT_ENOENT, "rename missing -> ENOENT");

    /* mapping */
    lisa_map_t* m = NULL;
    check(lisa_map_file(b, 0, &m) == LISA_PLAT_OK && lisa_map_size(m) == 5 &&
          memcmp(lisa_map_data(m), "hello", 5) == 0, "map read-only");
    lisa_unmap(m);

    m = NULL;
    int rc = lisa_map_file(b, 1, &m);
    check(rc == LISA_PLAT_OK, "map writable");
    if (rc == LISA_PLAT_OK) {
        memcpy(lisa_map_data(m), "HE", 2);
        check(lisa_map_sync(m) == LISA_PLAT_OK, "map_sync");
        lisa_unmap(m);
        check(read_equals(b, "HEllo"), "writes through mapping reach the file");
    }

    check(write_file(e, "") == 0, "write empty file");
    m = NULL;
    check(lisa_map_file(e, 0, &m) == LISA_PLAT_OK && lisa_map_size(m) == 0 &&
          lisa_map_data(m) == NULL, "map empty file");
    lisa_unmap(m);
    m = (lisa_map_t*)1;
    check(lisa_map_file(missing, 0, &m) == LISA_PLAT_ENOENT && m == NULL,
          "map missing -> ENOENT, out cleared");
    check(lisa_map_file(dir, 0, &m) == LISA_PLAT_EINVAL, "map dir -> EINVAL");
    lisa_unmap(NULL);

    /* remove */
    check(lisa_remove_file(b) == LISA_PLAT_OK && !lisa_path_exists(b), "remove_file");
    check(lisa_remove_file(b) == LISA_PLAT_ENOENT, "remove missing -> ENOENT");

    /* locking */
    lisa_lock_t* l1 = NULL;
    lisa_lock_t* l2 = NULL;
    check(lisa_lock_acquire(lk, 0, &l1) == LISA_PLAT_OK && l1 != NULL, "lock acquire");
    check(lisa_lock_acquire(lk, 0, &l2) == LISA_PLAT_EBUSY && l2 == NULL,
          "second non-blocking acquire -> EBUSY");
    lisa_lock_release(l1);
    check(lisa_lock_acquire(lk, 0, &l2) == LISA_PLAT_OK, "acquire after release");
    lisa_lock_release(l2);
    lisa_lock_release(NULL);

    /* time */
    int64_t t0 = lisa_time_monotonic_ns();
    volatile double x = 0;
    for (int i = 0; i < 100000; i++) x += i;
    int64_t t1 = lisa_time_monotonic_ns();
    check(t0 > 0 && t1 > t0, "monotonic clock advances");

    free(dir); free(a); free(b); free(e); free(lk); free(missing); free(nested);

    if (g_fail) {
        printf("FAIL: platform tests\n");
        return 1;
    }
    printf("PASS: platform tests\n");
    return 0;
}
