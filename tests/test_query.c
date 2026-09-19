/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * test_query — hybrid search (W7): RRF fusion unit tests, then the public
 * query API (include/lisa.h) on a small hand-built collection. The
 * query_text tests need the default embedding model in <models_dir>; they
 * are ignored without it.
 *
 * Usage: test_query <scratch_dir> <models_dir>
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unity.h"
#include "lisa.h"
#include "../src/retrieval/fusion.h"

static const char* g_scratch;
static char g_model_path[1024];
static int g_have_model;
static lisa_context_t* g_ctx;      /* audit sink, no filter */
static lisa_context_t* g_fctx;     /* retrieval filter hiding g_hidden */
static lisa_model_t* g_model;
static char g_coll[800];
static char g_dir[800];
static int g_seq;
static uint64_t g_ids[5];
static uint64_t g_hidden;
static int g_searches;

static void on_audit(void* user, const lisa_audit_event_t* ev) {
    (void)user;
    if (ev->action == LISA_AUDIT_SEARCH) g_searches++;
}

static int hide_one(void* user, const lisa_principal_t* principal,
                    const lisa_filter_input_t* in, uint8_t* allowed) {
    (void)user; (void)principal;
    for (int64_t i = 0; i < in->n_slots; i++) {
        if (allowed[i] && in->chunk_ids[i] == g_hidden) allowed[i] = 0;
    }
    return LISA_OK;
}

/*
 * Five chunks, dim 4:
 *   0 /docs/a.txt      pump bearing, vibration     {1,0,0,0}
 *   1 /docs/b.txt      lubrication, conveyor       {0,1,0,0}
 *   2 /docs/sub/c.txt  pump inspection, vibration  {0.9,0.1,0,0}
 *   3 /other/d.txt     Hindi: maintenance report, pump vibration  {0,0,1,0}
 *   4 /docs-old/e.txt  pump archive                {0,0,0,1}
 */
static void make_collection(void) {
    static const char* paths[5] = { "/docs/a.txt", "/docs/b.txt", "/docs/sub/c.txt",
                                    "/other/d.txt", "/docs-old/e.txt" };
    static const char* texts[5] = {
        "The pump bearing failed after strong vibration.",
        "Lubrication schedule for the conveyor belts.",
        "Pump inspection passed with no vibration.",
        "\xe0\xa4\xb0\xe0\xa4\x96\xe0\xa4\xb0\xe0\xa4\x96\xe0\xa4\xbe\xe0\xa4\xb5 "
        "\xe0\xa4\xb0\xe0\xa4\xbf\xe0\xa4\xaa\xe0\xa5\x8b\xe0\xa4\xb0\xe0\xa5\x8d\xe0\xa4\x9f "
        "\xe0\xa4\xaa\xe0\xa4\x82\xe0\xa4\xaa "
        "\xe0\xa4\x95\xe0\xa4\x82\xe0\xa4\xaa\xe0\xa4\xa8",
        "Pump archive from last year.",
    };
    const float v[5 * 4] = { 1, 0, 0, 0,  0, 1, 0, 0,  0.9f, 0.1f, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    lisa_chunk_t ch[5];
    memset(ch, 0, sizeof(ch));
    for (int i = 0; i < 5; i++) {
        ch[i].doc_id = paths[i];
        ch[i].source_path = paths[i];
        ch[i].text = texts[i];
        ch[i].length = (int64_t)strlen(texts[i]);
        ch[i].content_hash = "h";
    }
    snprintf(g_coll, sizeof(g_coll), "%s/query_%d_%d.coll", g_scratch, (int)getpid(), g_seq++);
    lisa_collection_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create(g_ctx, g_coll, "m", 4));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, g_coll, LISA_OPEN_WRITE, NULL, &c));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_add(c, 5, v, ch, g_ids));
    lisa_collection_close(c);
}

static lisa_collection_t* open_read(lisa_context_t* ctx) {
    lisa_collection_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(ctx, g_coll, LISA_OPEN_READ, NULL, &c));
    return c;
}

static int has(const lisa_scored_hit_t* h, int64_t n, uint64_t id) {
    for (int64_t i = 0; i < n; i++) {
        if (h[i].id == id) return 1;
    }
    return 0;
}

void setUp(void) { make_collection(); }
void tearDown(void) {}

/* ---- fusion ------------------------------------------------------------ */

static void test_rrf_basic(void) {
    const uint64_t a[] = { 1, 2, 3 };
    const uint64_t b[] = { 3, 4 };
    lisa_fused_t f[8];
    int64_t n = lisa_rrf_fuse(a, 3, 1.0, b, 2, 1.0, f, 8);
    TEST_ASSERT_EQUAL_INT64(4, n);
    TEST_ASSERT_EQUAL_UINT64(3, f[0].id);          /* in both lists */
    TEST_ASSERT_EQUAL_INT32(3, f[0].rank_a);
    TEST_ASSERT_EQUAL_INT32(1, f[0].rank_b);
    TEST_ASSERT_TRUE(fabs(f[0].score - (1.0 / 63 + 1.0 / 61)) < 1e-12);
    TEST_ASSERT_EQUAL_UINT64(1, f[1].id);
    TEST_ASSERT_EQUAL_UINT64(2, f[2].id);          /* ties keep list order */
    TEST_ASSERT_EQUAL_UINT64(4, f[3].id);
    TEST_ASSERT_EQUAL_INT32(0, f[3].rank_a);
    TEST_ASSERT_EQUAL_INT32(2, f[3].rank_b);
}

static void test_rrf_weights_capacity_duplicates(void) {
    const uint64_t a[] = { 1, 1, 2 };
    const uint64_t b[] = { 2 };
    lisa_fused_t f[8];
    int64_t n = lisa_rrf_fuse(a, 3, 1.0, b, 1, 3.0, f, 8);
    TEST_ASSERT_EQUAL_INT64(2, n);                 /* duplicate 1 counted once */
    TEST_ASSERT_EQUAL_UINT64(2, f[0].id);          /* keyword weight lifts it */
    TEST_ASSERT_EQUAL_INT32(3, f[0].rank_a);       /* rank = position in its list */

    n = lisa_rrf_fuse(a, 3, 1.0, b, 1, 0.0, f, 1);
    TEST_ASSERT_EQUAL_INT64(1, n);
    TEST_ASSERT_EQUAL_UINT64(1, f[0].id);

    TEST_ASSERT_EQUAL_INT64(0, lisa_rrf_fuse(NULL, 0, 1.0, NULL, 0, 1.0, f, 8));
    TEST_ASSERT_EQUAL_INT64(0, lisa_rrf_fuse(a, 3, 1.0, b, 1, 1.0, f, 0));
}

/* ---- public query API -------------------------------------------------- */

static void test_keyword_only(void) {
    lisa_collection_t* c = open_read(g_ctx);
    lisa_query_t q = LISA_QUERY_INIT;
    q.text = "pump";
    q.mode = LISA_SEARCH_KEYWORD;
    q.top_k = 10;
    lisa_scored_hit_t h[10];
    int64_t n = -1;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 10, &n));
    TEST_ASSERT_EQUAL_INT64(3, n);
    TEST_ASSERT_TRUE(has(h, n, g_ids[0]) && has(h, n, g_ids[2]) && has(h, n, g_ids[4]));
    for (int64_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT32(0, h[i].vector_rank);
        TEST_ASSERT_EQUAL_FLOAT(-1.0f, h[i].distance);
        TEST_ASSERT_EQUAL_INT32((int32_t)(i + 1), h[i].keyword_rank);
        TEST_ASSERT_TRUE(h[i].keyword_score > 0);
        if (i > 0) TEST_ASSERT_TRUE(h[i - 1].score >= h[i].score);
    }

    /* Punctuation and FTS syntax in user text are plain words, not errors. */
    q.text = "pump\" OR (NEAR* -";
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 10, &n));
    TEST_ASSERT_TRUE(has(h, n, g_ids[0]));
    q.text = "   ";
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 10, &n));
    TEST_ASSERT_EQUAL_INT64(0, n);
    q.text = "nosuchwordanywhere";
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 10, &n));
    TEST_ASSERT_EQUAL_INT64(0, n);
    lisa_collection_close(c);
}

static void test_keyword_hindi(void) {
    lisa_collection_t* c = open_read(g_ctx);
    lisa_query_t q = LISA_QUERY_INIT;
    q.text = "\xe0\xa4\x95\xe0\xa4\x82\xe0\xa4\xaa\xe0\xa4\xa8";   /* vibration */
    q.mode = LISA_SEARCH_KEYWORD;
    lisa_scored_hit_t h[5];
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 5, &n));
    TEST_ASSERT_EQUAL_INT64(1, n);
    TEST_ASSERT_EQUAL_UINT64(g_ids[3], h[0].id);
    lisa_collection_close(c);
}

static void test_vector_only(void) {
    lisa_collection_t* c = open_read(g_ctx);
    const float qv[4] = { 1, 0, 0, 0 };
    lisa_query_t q = LISA_QUERY_INIT;
    q.vector = qv;
    q.text = "lubrication";          /* ignored in VECTOR mode */
    q.mode = LISA_SEARCH_VECTOR;
    q.top_k = 2;
    lisa_scored_hit_t h[5];
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 5, &n));
    TEST_ASSERT_EQUAL_INT64(2, n);
    TEST_ASSERT_EQUAL_UINT64(g_ids[0], h[0].id);
    TEST_ASSERT_EQUAL_UINT64(g_ids[2], h[1].id);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, h[0].distance);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.02f, h[1].distance);
    TEST_ASSERT_EQUAL_INT32(0, h[0].keyword_rank);
    TEST_ASSERT_EQUAL_INT32(1, h[0].vector_rank);

    /* capacity below top_k limits the output. */
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 1, &n));
    TEST_ASSERT_EQUAL_INT64(1, n);
    lisa_collection_close(c);
}

static void test_hybrid_fuses_both_lists(void) {
    lisa_collection_t* c = open_read(g_ctx);
    const float qv[4] = { 0, 1, 0, 0 };   /* nearest: 1, then 2 */
    lisa_query_t q = LISA_QUERY_INIT;
    q.vector = qv;
    q.text = "inspection";                /* matches only 2 */
    q.top_k = 5;
    lisa_scored_hit_t h[5];
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 5, &n));
    TEST_ASSERT_EQUAL_INT64(5, n);
    TEST_ASSERT_EQUAL_UINT64(g_ids[2], h[0].id);   /* in both lists */
    TEST_ASSERT_TRUE(h[0].vector_rank > 0 && h[0].keyword_rank > 0);
    TEST_ASSERT_TRUE(h[0].distance >= 0);
    TEST_ASSERT_TRUE(has(h, n, g_ids[1]) && has(h, n, g_ids[0]));

    /* Zero keyword weight: pure vector order. */
    q.keyword_weight = 0;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 5, &n));
    TEST_ASSERT_EQUAL_UINT64(g_ids[1], h[0].id);
    lisa_collection_close(c);
}

static void test_path_prefix(void) {
    lisa_collection_t* c = open_read(g_ctx);
    lisa_query_t q = LISA_QUERY_INIT;
    q.text = "pump";
    q.path_prefix = "/docs/";
    q.top_k = 10;
    lisa_scored_hit_t h[10];
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 10, &n));
    TEST_ASSERT_EQUAL_INT64(2, n);                 /* not /docs-old/, not /other/ */
    TEST_ASSERT_TRUE(has(h, n, g_ids[0]) && has(h, n, g_ids[2]));

    const float qv[4] = { 0, 0, 0.5f, 0.5f };
    q.text = NULL;
    q.vector = qv;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 10, &n));
    TEST_ASSERT_EQUAL_INT64(3, n);
    TEST_ASSERT_FALSE(has(h, n, g_ids[3]) || has(h, n, g_ids[4]));

    q.path_prefix = "/nowhere/";
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 10, &n));
    TEST_ASSERT_EQUAL_INT64(0, n);
    lisa_collection_close(c);
}

static void test_retrieval_filter_applies_to_both_lists(void) {
    g_hidden = g_ids[0];
    lisa_collection_t* c = open_read(g_fctx);
    lisa_query_t q = LISA_QUERY_INIT;
    q.text = "pump";
    q.mode = LISA_SEARCH_KEYWORD;
    q.top_k = 10;
    lisa_scored_hit_t h[10];
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 10, &n));
    TEST_ASSERT_EQUAL_INT64(2, n);
    TEST_ASSERT_FALSE(has(h, n, g_ids[0]));

    const float qv[4] = { 1, 0, 0, 0 };
    q.mode = LISA_SEARCH_HYBRID;
    q.vector = qv;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 10, &n));
    TEST_ASSERT_EQUAL_INT64(4, n);
    TEST_ASSERT_FALSE(has(h, n, g_ids[0]));
    TEST_ASSERT_EQUAL_UINT64(g_ids[2], h[0].id);
    lisa_collection_close(c);
}

static void test_removed_chunks_not_returned(void) {
    lisa_collection_t* w = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, g_coll, LISA_OPEN_WRITE, NULL, &w));
    lisa_collection_t* r = open_read(g_ctx);   /* view taken before the removal */
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_remove(w, 1, &g_ids[2]));

    lisa_query_t q = LISA_QUERY_INIT;
    q.text = "pump";
    q.top_k = 10;
    lisa_scored_hit_t h[10];
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(w, &q, h, 10, &n));
    TEST_ASSERT_EQUAL_INT64(2, n);
    TEST_ASSERT_FALSE(has(h, n, g_ids[2]));

    /* The older reader never returns an ID its view and the FTS index disagree on. */
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(r, &q, h, 10, &n));
    TEST_ASSERT_FALSE(has(h, n, g_ids[2]));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_refresh(r));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(r, &q, h, 10, &n));
    TEST_ASSERT_EQUAL_INT64(2, n);
    lisa_collection_close(r);
    lisa_collection_close(w);
}

static void test_invalid_arguments_and_audit(void) {
    lisa_collection_t* c = open_read(g_ctx);
    const float qv[4] = { 1, 0, 0, 0 };
    lisa_scored_hit_t h[5];
    int64_t n = 7;
    lisa_query_t q = LISA_QUERY_INIT;

    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_query(c, &q, h, 5, &n));
    TEST_ASSERT_EQUAL_INT64(0, n);
    q.mode = LISA_SEARCH_VECTOR; q.text = "pump";
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_query(c, &q, h, 5, &n));
    q.mode = LISA_SEARCH_KEYWORD; q.text = NULL; q.vector = qv;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_query(c, &q, h, 5, &n));
    q.mode = (lisa_search_mode)9; q.text = "pump";
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_query(c, &q, h, 5, &n));
    q.mode = LISA_SEARCH_HYBRID;
    q.top_k = 0;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_query(c, &q, h, 5, &n));
    q.top_k = 10001;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_query(c, &q, h, 5, &n));
    q.top_k = 5; q.vector_weight = -1;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_query(c, &q, h, 5, &n));
    q.vector_weight = 1;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_query(NULL, &q, h, 5, &n));
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_query(c, NULL, h, 5, &n));
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_query(c, &q, NULL, 5, &n));
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_query(c, &q, h, 5, NULL));

    /* Old callers: a struct_size ending before `vector` gets defaults for the rest. */
    lisa_query_t old = LISA_QUERY_INIT;
    old.struct_size = offsetof(lisa_query_t, vector);
    old.text = "pump";
    old.vector = qv;                                  /* beyond struct_size: ignored */
    old.mode = LISA_SEARCH_VECTOR;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &old, h, 5, &n));
    TEST_ASSERT_EQUAL_INT64(3, n);
    TEST_ASSERT_EQUAL_INT32(0, h[0].vector_rank);

    /* Every query that reaches the collection is audited. */
    int before = g_searches;
    q.top_k = 5;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query(c, &q, h, 5, &n));
    TEST_ASSERT_EQUAL_INT(before + 1, g_searches);

    /* Wrong model for this collection. */
    if (g_model) {
        TEST_ASSERT_EQUAL_INT(LISA_E_MODEL_MISMATCH,
                              lisa_collection_query_text(c, g_model, "pump", NULL, h, 5, &n));
    }
    lisa_collection_close(c);
}

/* ---- with the embedding model ------------------------------------------ */

#define NEED_MODEL() do { if (!g_have_model) TEST_IGNORE_MESSAGE("embedding model not present"); } while (0)

static void write_file(const char* name, const char* text) {
    char p[1000];
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE* f = fopen(p, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
}

static void test_query_text_real_model(void) {
    NEED_MODEL();
    snprintf(g_dir, sizeof(g_dir), "%s/qdocs_%d_%d", g_scratch, (int)getpid(), g_seq++);
    mkdir(g_dir, 0755);
    write_file("pump.txt", "Pump P-7 failed on Tuesday: the main bearing seized after weeks of "
                           "rising vibration. Replacement bearing ordered from supplier.");
    write_file("canteen.txt", "The canteen menu changes on Mondays. Lunch is served from noon "
                              "until two; vegetarian options are always available.");
    write_file("safety.txt", "Fire drills happen every quarter. Assemble at gate B and wait for "
                             "the floor warden to count everyone.");
    char coll[900];
    snprintf(coll, sizeof(coll), "%s.coll", g_dir);
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create_for_model(g_ctx, coll, g_model, 0));
    lisa_collection_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(g_ctx, coll, LISA_OPEN_WRITE, NULL, &c));
    const char* paths[1] = { g_dir };
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_ingest(c, g_model, paths, 1, NULL, NULL));

    lisa_scored_hit_t h[3];
    int64_t n = 0;
    /* Meaning, no shared keyword with the answer ("machine", "broke"). */
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query_text(c, g_model, "Which machine broke down?",
                                                              NULL, h, 3, &n));
    TEST_ASSERT_EQUAL_INT64(3, n);
    lisa_chunk_t* got = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_get_chunk(c, h[0].id, &got));
    TEST_ASSERT_NOT_NULL(strstr(got->source_path, "pump.txt"));
    lisa_chunk_free(got);

    /* Exact word match: keyword list agrees with the vector list. */
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_query_text(c, g_model, "When are fire drills?",
                                                              NULL, h, 3, &n));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_get_chunk(c, h[0].id, &got));
    TEST_ASSERT_NOT_NULL(strstr(got->source_path, "safety.txt"));
    TEST_ASSERT_TRUE(h[0].keyword_rank == 1 && h[0].vector_rank == 1);
    lisa_chunk_free(got);

    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT,
                          lisa_collection_query_text(c, g_model, NULL, NULL, h, 3, &n));
    lisa_collection_close(c);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <scratch_dir> <models_dir>\n", argv[0]);
        return 2;
    }
    g_scratch = argv[1];
    mkdir(g_scratch, 0755);
    snprintf(g_model_path, sizeof(g_model_path), "%s/Qwen3-Embedding-0.6B-Q8_0.gguf", argv[2]);
    FILE* f = fopen(g_model_path, "rb");
    g_have_model = f != NULL;
    if (f) fclose(f);

    lisa_audit_sink_t sink = { sizeof(lisa_audit_sink_t), NULL, on_audit };
    lisa_context_config_t cfg = LISA_CONTEXT_CONFIG_INIT;
    cfg.audit = &sink;
    if (lisa_context_create(&cfg, &g_ctx) != LISA_OK) return 1;
    lisa_retrieval_filter_t filt = { sizeof(lisa_retrieval_filter_t), NULL, hide_one };
    lisa_context_config_t fcfg = LISA_CONTEXT_CONFIG_INIT;
    fcfg.retrieval_filter = &filt;
    if (lisa_context_create(&fcfg, &g_fctx) != LISA_OK) return 1;
    if (g_have_model && lisa_model_load(g_ctx, g_model_path, NULL, &g_model) != LISA_OK) return 1;

    UNITY_BEGIN();
    RUN_TEST(test_rrf_basic);
    RUN_TEST(test_rrf_weights_capacity_duplicates);
    RUN_TEST(test_keyword_only);
    RUN_TEST(test_keyword_hindi);
    RUN_TEST(test_vector_only);
    RUN_TEST(test_hybrid_fuses_both_lists);
    RUN_TEST(test_path_prefix);
    RUN_TEST(test_retrieval_filter_applies_to_both_lists);
    RUN_TEST(test_removed_chunks_not_returned);
    RUN_TEST(test_invalid_arguments_and_audit);
    RUN_TEST(test_query_text_real_model);
    int failures = UNITY_END();
    lisa_model_free(g_model);   /* llama.cpp aborts at exit if a model is still loaded */
    lisa_context_destroy(g_fctx);
    lisa_context_destroy(g_ctx);
    return failures == 0 ? 0 : 1;
}
