/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_sqlite — the vendored SQLite build has the features storage v2
 * relies on: FTS5 with BM25 ranking, WAL mode, thread safety, and no
 * runtime extension loading.
 *
 * Usage: test_sqlite <scratch_dir>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "sqlite3.h"
#include "../src/platform/platform.h"

static char* g_db_path = NULL;
static sqlite3* g_db = NULL;

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(g_db_path, &g_db));
}

void tearDown(void) {
    sqlite3_close(g_db);
    g_db = NULL;
}

static void exec_ok(const char* sql) {
    char* err = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        printf("SQL error: %s\n", err ? err : "?");
    }
    sqlite3_free(err);
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
}

static void test_version_and_options(void) {
    TEST_ASSERT_EQUAL_STRING("3.53.4", sqlite3_libversion());
    TEST_ASSERT_EQUAL_INT(1, sqlite3_threadsafe());
    TEST_ASSERT_TRUE(sqlite3_compileoption_used("ENABLE_FTS5"));
    TEST_ASSERT_TRUE(sqlite3_compileoption_used("OMIT_LOAD_EXTENSION"));
}

static void test_wal_mode(void) {
    sqlite3_stmt* st = NULL;
    TEST_ASSERT_EQUAL_INT(SQLITE_OK,
        sqlite3_prepare_v2(g_db, "PRAGMA journal_mode=WAL;", -1, &st, NULL));
    TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
    TEST_ASSERT_EQUAL_STRING("wal", (const char*)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
}

static void test_fts5_bm25(void) {
    exec_ok("DROP TABLE IF EXISTS docs;"
            "CREATE VIRTUAL TABLE docs USING fts5(body);"
            "INSERT INTO docs(rowid, body) VALUES"
            " (1, 'pump vibration at forty hertz'),"
            " (2, 'annual leave policy for employees'),"
            " (3, 'pump pump maintenance schedule');");

    sqlite3_stmt* st = NULL;
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(g_db,
        "SELECT rowid FROM docs WHERE docs MATCH 'pump' ORDER BY bm25(docs);",
        -1, &st, NULL));
    TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
    TEST_ASSERT_EQUAL_INT(3, sqlite3_column_int(st, 0)); /* two hits ranks first */
    TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
    TEST_ASSERT_EQUAL_INT(1, sqlite3_column_int(st, 0));
    TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(st));
    sqlite3_finalize(st);
}

static void test_double_quoted_strings_rejected(void) {
    exec_ok("DROP TABLE IF EXISTS t; CREATE TABLE t(a TEXT);");
    char* err = NULL;
    int rc = sqlite3_exec(g_db, "INSERT INTO t VALUES (\"not a string\");",
                          NULL, NULL, &err);
    sqlite3_free(err);
    TEST_ASSERT_NOT_EQUAL_INT(SQLITE_OK, rc);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch_dir>\n", argv[0]);
        return 2;
    }
    lisa_mkdir(argv[1]);
    char name[64];
    snprintf(name, sizeof(name), "sqlite_%lld.db", (long long)lisa_time_monotonic_ns());
    g_db_path = lisa_path_join(argv[1], name);
    if (g_db_path == NULL) return 1;

    UNITY_BEGIN();
    RUN_TEST(test_version_and_options);
    RUN_TEST(test_wal_mode);
    RUN_TEST(test_fts5_bm25);
    RUN_TEST(test_double_quoted_strings_rejected);
    int failures = UNITY_END();

    free(g_db_path);
    return failures == 0 ? 0 : 1;
}
