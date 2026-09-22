/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * test_ingest_api — public ingest API (include/lisa.h), used as a program
 * would: only lisa.h. Needs the default embedding model in <models_dir>;
 * model-dependent tests are ignored without it.
 *
 * Usage: test_ingest_api <scratch_dir> <fixtures_dir> <models_dir>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/platform/platform.h"

#include "unity.h"
#include "lisa.h"

static const char* g_scratch;
static const char* g_fixtures;
static char g_model_path[1024];
static int g_have_model;
static lisa_context_t* g_ctx;
static lisa_model_t* g_model;
static char g_dir[800];
static char g_coll[800];
static int g_seq;

typedef struct {
    int     events;
    int64_t last_count;
    int     last_status;
} audit_t;
static audit_t g_audit;

static void on_audit(void* user, const lisa_audit_event_t* ev) {
    (void)user;
    if (ev->action == LISA_AUDIT_INGEST) {
        g_audit.events++;
        g_audit.last_count = ev->item_count;
        g_audit.last_status = ev->status;
    }
}

void setUp(void) {
    snprintf(g_dir, sizeof(g_dir), "%s/iapi_%d_%d", g_scratch, (int)getpid(), g_seq++);
    snprintf(g_coll, sizeof(g_coll), "%s.coll", g_dir);
    mkdir(g_dir, 0755);
}
void tearDown(void) {}

#define NEED_MODEL() do { if (!g_have_model) TEST_IGNORE_MESSAGE("embedding model not present"); } while (0)

static void write_file(const char* name, const char* text) {
    char p[1000];
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE* f = fopen(p, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
}

static void make_many(int n) {
    for (int i = 0; i < n; i++) {
        char name[64], text[400];
        snprintf(name, sizeof(name), "note_%03d.txt", i);
        snprintf(text, sizeof(text),
                 "Note %d. Maintenance record for machine %d: bearings replaced, "
                 "vibration checked, lubrication done, inspection passed.", i, i * 7);
        write_file(name, text);
    }
}

/* ---- model-free -------------------------------------------------------- */

static void test_chunk_page_roundtrip(void) {
    lisa_collection_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create(g_ctx, g_coll, "m", 4));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, g_coll, LISA_OPEN_WRITE, NULL, &c));
    float v[4] = { 1, 2, 3, 4 };
    lisa_chunk_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.doc_id = "d"; ch.source_path = "/d.pdf"; ch.text = "t"; ch.content_hash = "h";
    ch.page = 7;
    uint64_t id;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_add(c, 1, v, &ch, &id));
    lisa_chunk_t* got = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_get_chunk(c, id, &got));
    TEST_ASSERT_EQUAL_INT64(7, got->page);
    lisa_chunk_free(got);

    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_documents(c, NULL, NULL, NULL));
    lisa_ingest_job_t* job = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_ingest_start(c, NULL, NULL, 0, NULL, &job));
    TEST_ASSERT_NULL(job);
    lisa_collection_close(c);
    lisa_ingest_free(NULL);
    lisa_ingest_cancel(NULL);
}

/* ---- with the embedding model ------------------------------------------ */

static void test_create_for_model(void) {
    NEED_MODEL();
    char p[900];
    lisa_collection_t* c = NULL;
    snprintf(p, sizeof(p), "%s.a", g_coll);
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create_for_model(g_ctx, p, g_model, 0));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, p, LISA_OPEN_READ, NULL, &c));
    lisa_collection_info_t info = LISA_COLLECTION_INFO_INIT;
    lisa_collection_info(c, &info);
    TEST_ASSERT_EQUAL_INT64(1024, info.dim);
    TEST_ASSERT_EQUAL_STRING("qwen3-embedding-0.6b-q8_0", info.embedding_model);
    lisa_collection_close(c);

    snprintf(p, sizeof(p), "%s.b", g_coll);
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create_for_model(g_ctx, p, g_model, 256));
    snprintf(p, sizeof(p), "%s.c", g_coll);
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_create_for_model(g_ctx, p, g_model, 2048));
}

typedef struct {
    int n, errors;
} docs_t;

static int count_docs(void* user, const lisa_document_t* d) {
    docs_t* x = (docs_t*)user;
    x->n++;
    if (strcmp(d->status, "error") == 0) x->errors++;
    return 0;
}

static void test_blocking_ingest_and_search(void) {
    NEED_MODEL();
    write_file("leave.md", "# Leave Policy\n\nEmployees get 20 days of annual leave per year.");
    write_file("pump.txt", "Replace the pump bearing when vibration exceeds 40 Hz.");
    write_file("broken.pdf", "not a pdf");
    char cmd[2000];
    snprintf(cmd, sizeof(cmd), "cp '%s/two_pages.pdf' '%s/manual.pdf'", g_fixtures, g_dir);
    TEST_ASSERT_EQUAL_INT(0, system(cmd));

    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create_for_model(g_ctx, g_coll, g_model, 0));
    lisa_collection_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, g_coll, LISA_OPEN_WRITE, NULL, &c));
    const char* paths[1] = { g_dir };
    lisa_ingest_status_t st = LISA_INGEST_STATUS_INIT;
    memset(&g_audit, 0, sizeof(g_audit));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_ingest(c, g_model, paths, 1, NULL, &st));
    TEST_ASSERT_EQUAL_INT(LISA_JOB_SUCCEEDED, st.state);
    TEST_ASSERT_EQUAL_INT64(3, st.files_added);
    TEST_ASSERT_EQUAL_INT64(1, st.files_failed);
    TEST_ASSERT_TRUE(st.chunks_added >= 3);
    TEST_ASSERT_TRUE(st.elapsed_seconds > 0);
    TEST_ASSERT_EQUAL_INT(1, g_audit.events);
    TEST_ASSERT_EQUAL_INT64(st.chunks_added, g_audit.last_count);

    docs_t d = { 0, 0 };
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_documents(c, NULL, count_docs, &d));
    TEST_ASSERT_EQUAL_INT(4, d.n);
    TEST_ASSERT_EQUAL_INT(1, d.errors);

    /* Search with a real question (handles usable again after the job). */
    const char* q[1] = { "How much annual leave do employees get?" };
    float qv[1024];
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_embed(g_model, LISA_EMBED_QUERY, q, 1, qv, 0));
    lisa_hit_t hit;
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(c, qv, NULL, &hit, 1, &n));
    lisa_chunk_t* ch = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_get_chunk(c, hit.id, &ch));
    TEST_ASSERT_NOT_NULL(strstr(ch->source_path, "leave.md"));
    lisa_chunk_free(ch);

    /* Second run: nothing to do, except the file that failed before (a
     * failure is usually temporary, so it is always read again). */
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_ingest(c, g_model, paths, 1, NULL, &st));
    TEST_ASSERT_EQUAL_INT64(3, st.files_unchanged);
    TEST_ASSERT_EQUAL_INT64(0, st.chunks_added);
    lisa_collection_close(c);
}

static void test_background_job_busy_and_concurrent_search(void) {
    NEED_MODEL();
    make_many(40);
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create_for_model(g_ctx, g_coll, g_model, 0));
    lisa_collection_t *w = NULL, *r = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, g_coll, LISA_OPEN_WRITE, NULL, &w));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, g_coll, LISA_OPEN_READ, NULL, &r));

    const char* paths[1] = { g_dir };
    lisa_ingest_job_t* job = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_ingest_start(w, g_model, paths, 1, NULL, &job));

    /* The job owns the writer handle and the model until it finishes. */
    lisa_collection_info_t info = LISA_COLLECTION_INFO_INIT;
    lisa_ingest_status_t st = LISA_INGEST_STATUS_INIT;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_ingest_status(job, &st));
    if (st.state == LISA_JOB_RUNNING) {
        TEST_ASSERT_EQUAL_INT(LISA_E_BUSY, lisa_collection_info(w, &info));
        const char* t[1] = { "x" };
        float v[1024];
        TEST_ASSERT_EQUAL_INT(LISA_E_BUSY, lisa_embed(g_model, LISA_EMBED_QUERY, t, 1, v, 0));
        lisa_ingest_job_t* second = NULL;
        TEST_ASSERT_EQUAL_INT(LISA_E_BUSY, lisa_ingest_start(w, g_model, paths, 1, NULL, &second));
    }

    /* A separate read handle can search while ingest runs. */
    float zero[1024];
    memset(zero, 0, sizeof(zero));
    zero[0] = 1.0f;
    int searched = 0;
    do {
        lisa_ingest_status(job, &st);
        lisa_hit_t hits[3];
        int64_t n = 0;
        TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_refresh(r));
        TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(r, zero, NULL, hits, 3, &n));
        searched++;
        lisa_sleep_ms(20);
    } while (st.state == LISA_JOB_RUNNING);

    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_ingest_wait(job, &st));
    TEST_ASSERT_EQUAL_INT(LISA_JOB_SUCCEEDED, st.state);
    TEST_ASSERT_EQUAL_INT64(40, st.files_added);
    TEST_ASSERT_TRUE(searched >= 1);
    lisa_ingest_free(job);

    /* Free again: handles usable. */
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_info(w, &info));
    TEST_ASSERT_EQUAL_INT64(st.chunks_added, info.chunk_count);
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_refresh(r));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_info(r, &info));
    TEST_ASSERT_EQUAL_INT64(st.chunks_added, info.chunk_count);
    lisa_collection_close(r);
    lisa_collection_close(w);
}

static void test_cancel(void) {
    NEED_MODEL();
    make_many(60);
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create_for_model(g_ctx, g_coll, g_model, 0));
    lisa_collection_t* w = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, g_coll, LISA_OPEN_WRITE, NULL, &w));
    const char* paths[1] = { g_dir };
    lisa_ingest_job_t* job = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_ingest_start(w, g_model, paths, 1, NULL, &job));
    lisa_ingest_cancel(job);
    lisa_ingest_status_t st = LISA_INGEST_STATUS_INIT;
    TEST_ASSERT_EQUAL_INT(LISA_E_CANCELLED, lisa_ingest_wait(job, &st));
    TEST_ASSERT_EQUAL_INT(LISA_JOB_CANCELLED, st.state);
    TEST_ASSERT_TRUE(st.files_added < 60);
    lisa_ingest_free(job);

    /* Collection consistent and usable; a new run completes the rest. A
     * file the cancelled run had recorded as failed is read again, so it
     * counts as updated rather than added. */
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_ingest(w, g_model, paths, 1, NULL, &st));
    TEST_ASSERT_EQUAL_INT64(60, st.files_added + st.files_unchanged + st.files_updated);
    lisa_collection_close(w);
}

static void test_start_errors(void) {
    NEED_MODEL();
    write_file("a.txt", "text");
    const char* paths[1] = { g_dir };
    lisa_ingest_job_t* job = NULL;

    /* Collection for a different model. */
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create(g_ctx, g_coll, "some-other-model", 1024));
    lisa_collection_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, g_coll, LISA_OPEN_WRITE, NULL, &c));
    TEST_ASSERT_EQUAL_INT(LISA_E_MODEL_MISMATCH, lisa_ingest_start(c, g_model, paths, 1, NULL, &job));
    TEST_ASSERT_NULL(job);
    lisa_collection_info_t info = LISA_COLLECTION_INFO_INIT;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_info(c, &info));   /* not left busy */
    lisa_collection_close(c);

    /* Read-only handle; missing path; bad options. */
    char p2[900];
    snprintf(p2, sizeof(p2), "%s.ok", g_coll);
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create_for_model(g_ctx, p2, g_model, 0));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, p2, LISA_OPEN_READ, NULL, &c));
    TEST_ASSERT_EQUAL_INT(LISA_E_READ_ONLY, lisa_ingest_start(c, g_model, paths, 1, NULL, &job));
    lisa_collection_close(c);
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, p2, LISA_OPEN_WRITE, NULL, &c));
    char missing[900];
    snprintf(missing, sizeof(missing), "%s/nope", g_dir);
    const char* bad[1] = { missing };
    TEST_ASSERT_EQUAL_INT(LISA_E_NOT_FOUND, lisa_ingest_start(c, g_model, bad, 1, NULL, &job));
    lisa_ingest_options_t o = LISA_INGEST_OPTIONS_INIT;
    o.chunk_overlap_chars = o.chunk_chars;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_ingest_start(c, g_model, paths, 1, &o, &job));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_info(c, &info));
    lisa_collection_close(c);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <scratch_dir> <fixtures_dir> <models_dir>\n", argv[0]);
        return 2;
    }
    g_scratch = argv[1];
    g_fixtures = argv[2];
    mkdir(g_scratch, 0755);
    snprintf(g_model_path, sizeof(g_model_path), "%s/Qwen3-Embedding-0.6B-Q8_0.gguf", argv[3]);
    FILE* f = fopen(g_model_path, "rb");
    g_have_model = f != NULL;
    if (f) fclose(f);

    lisa_audit_sink_t sink = { sizeof(lisa_audit_sink_t), NULL, on_audit };
    lisa_context_config_t cfg = LISA_CONTEXT_CONFIG_INIT;
    cfg.audit = &sink;
    if (lisa_context_create(&cfg, &g_ctx) != LISA_OK) return 1;
    if (g_have_model && lisa_model_load(g_ctx, g_model_path, NULL, &g_model) != LISA_OK) return 1;

    UNITY_BEGIN();
    RUN_TEST(test_chunk_page_roundtrip);
    RUN_TEST(test_create_for_model);
    RUN_TEST(test_blocking_ingest_and_search);
    RUN_TEST(test_background_job_busy_and_concurrent_search);
    RUN_TEST(test_cancel);
    RUN_TEST(test_start_errors);
    int failures = UNITY_END();
    lisa_model_free(g_model);   /* llama.cpp aborts at exit if a model is still loaded */
    lisa_context_destroy(g_ctx);
    return failures == 0 ? 0 : 1;
}
