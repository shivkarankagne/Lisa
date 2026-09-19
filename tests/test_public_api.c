/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * test_public_api — the public API (include/lisa.h), used exactly as a
 * program would: only lisa.h is included.
 *
 * Usage: test_public_api <scratch_dir>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unity.h"
#include "lisa.h"

#define DIM 6
#define MODEL "api-test-model"

static const char* g_scratch;
static char g_path[512];
static int g_seq;

void setUp(void) {
    snprintf(g_path, sizeof(g_path), "%s/api_%d_%d", g_scratch, (int)getpid(), g_seq++);
}
void tearDown(void) {}

static void vec_for(uint64_t id, float* out) {
    for (int j = 0; j < DIM; j++) out[j] = (float)((id * 7 + (uint64_t)j) % 23);
}

static lisa_chunk_t chunk(const char* doc, int64_t i) {
    lisa_chunk_t c;
    memset(&c, 0, sizeof(c));
    c.doc_id = doc;
    c.chunk_index = i;
    c.source_path = "/data/report.pdf";
    c.offset = i * 10;
    c.length = 10;
    c.text = "chunk text";
    c.content_hash = "h1";
    return c;
}

/* Add n chunks whose vectors correspond to IDs first..first+n-1. */
static void add_n(lisa_collection_t* c, const char* doc, int64_t n, uint64_t first) {
    float v[64 * DIM];
    lisa_chunk_t ch[64];
    TEST_ASSERT_TRUE(n <= 64);
    for (int64_t i = 0; i < n; i++) {
        vec_for(first + (uint64_t)i, v + i * DIM);
        ch[i] = chunk(doc, i);
    }
    uint64_t ids[64];
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_add(c, n, v, ch, ids));
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQUAL_UINT64(first + (uint64_t)i, ids[i]);
}

/* ---- version and status ----------------------------------------------- */

static void test_version(void) {
    int a = -1, b = -1, c = -1;
    TEST_ASSERT_EQUAL_STRING(LISA_VERSION_STRING, lisa_version(&a, &b, &c));
    TEST_ASSERT_EQUAL_INT(LISA_VERSION_MAJOR, a);
    TEST_ASSERT_EQUAL_INT(LISA_VERSION_MINOR, b);
    TEST_ASSERT_EQUAL_INT(LISA_VERSION_PATCH, c);
    TEST_ASSERT_NOT_NULL(lisa_version(NULL, NULL, NULL));
}

static void test_status_strings(void) {
    for (int s = LISA_OK; s >= LISA_E_WRONG_MODEL_KIND; s--) {
        TEST_ASSERT_NOT_NULL(lisa_status_string(s));
        TEST_ASSERT_TRUE(strcmp(lisa_status_string(s), "unknown status") != 0);
    }
    TEST_ASSERT_EQUAL_STRING("unknown status", lisa_status_string(-999));
    TEST_ASSERT_EQUAL_STRING("unknown status", lisa_status_string(5));
}

/* ---- context and allocator -------------------------------------------- */

typedef struct {
    long allocs, frees;
} counts_t;

static void* c_alloc(void* u, size_t n) {
    ((counts_t*)u)->allocs++;
    return malloc(n);
}
static void* c_realloc(void* u, void* p, size_t n) {
    if (p == NULL) ((counts_t*)u)->allocs++;
    return realloc(p, n);
}
static void c_free(void* u, void* p) {
    if (p) ((counts_t*)u)->frees++;
    free(p);
}

static void test_context_config_validation(void) {
    lisa_context_t* ctx = (lisa_context_t*)1;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_context_create(NULL, NULL));

    lisa_context_config_t cfg = LISA_CONTEXT_CONFIG_INIT;
    cfg.struct_size = 0;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_context_create(&cfg, &ctx));
    TEST_ASSERT_NULL(ctx);

    lisa_allocator_t bad = { c_alloc, NULL, c_free, NULL };
    lisa_context_config_t cfg2 = LISA_CONTEXT_CONFIG_INIT;
    cfg2.allocator = &bad;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_context_create(&cfg2, &ctx));

    lisa_audit_sink_t no_cb = { sizeof(lisa_audit_sink_t), NULL, NULL };
    lisa_context_config_t cfg3 = LISA_CONTEXT_CONFIG_INIT;
    cfg3.audit = &no_cb;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_context_create(&cfg3, &ctx));

    /* A caller built against an older, shorter config struct: later fields ignored. */
    lisa_context_config_t old = LISA_CONTEXT_CONFIG_INIT;
    old.audit = &no_cb;  /* would be invalid, but lies beyond the old struct_size */
    old.struct_size = offsetof(lisa_context_config_t, auth);
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_context_create(&old, &ctx));
    lisa_context_destroy(ctx);

    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_context_create(NULL, &ctx));
    TEST_ASSERT_NULL(lisa_context_auth(ctx));
    TEST_ASSERT_NULL(lisa_context_http_routes(ctx));
    lisa_context_destroy(ctx);
    lisa_context_destroy(NULL);
}

/* ---- collections through the public API -------------------------------- */

static void test_collection_lifecycle_and_errors(void) {
    counts_t counts = { 0, 0 };
    lisa_allocator_t a = { c_alloc, c_realloc, c_free, &counts };
    lisa_context_config_t cfg = LISA_CONTEXT_CONFIG_INIT;
    cfg.allocator = &a;
    lisa_context_t* ctx = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_context_create(&cfg, &ctx));

    lisa_collection_t *w = NULL, *r = NULL, *x = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_E_NOT_FOUND, lisa_collection_open(ctx, g_path, LISA_OPEN_READ, NULL, &x));
    TEST_ASSERT_NULL(x);
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_create(ctx, g_path, MODEL, 0));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create(ctx, g_path, MODEL, DIM));
    TEST_ASSERT_EQUAL_INT(LISA_E_EXISTS, lisa_collection_create(ctx, g_path, MODEL, DIM));
    TEST_ASSERT_EQUAL_INT(LISA_E_MODEL_MISMATCH,
                          lisa_collection_open(ctx, g_path, LISA_OPEN_READ, "other", &x));

    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(ctx, g_path, LISA_OPEN_WRITE, MODEL, &w));
    TEST_ASSERT_EQUAL_INT(LISA_E_BUSY, lisa_collection_open(ctx, g_path, LISA_OPEN_WRITE, NULL, &x));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(ctx, g_path, LISA_OPEN_READ, NULL, &r));

    add_n(w, "docA", 10, 0);
    add_n(w, "docB", 5, 10);

    lisa_collection_info_t info = LISA_COLLECTION_INFO_INIT;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_info(w, &info));
    TEST_ASSERT_EQUAL_STRING(MODEL, info.embedding_model);
    TEST_ASSERT_EQUAL_INT64(DIM, info.dim);
    TEST_ASSERT_EQUAL_INT64(15, info.chunk_count);

    /* Read-only handle refuses writes; sees data after refresh. */
    float v[DIM];
    lisa_chunk_t ch = chunk("x", 0);
    vec_for(0, v);
    TEST_ASSERT_EQUAL_INT(LISA_E_READ_ONLY, lisa_collection_add(r, 1, v, &ch, NULL));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_refresh(r));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_info(r, &info));
    TEST_ASSERT_EQUAL_INT64(15, info.chunk_count);

    /* get_chunk */
    lisa_chunk_t* got = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_get_chunk(w, 12, &got));
    TEST_ASSERT_EQUAL_UINT64(12, got->id);
    TEST_ASSERT_EQUAL_STRING("docB", got->doc_id);
    TEST_ASSERT_EQUAL_INT64(2, got->chunk_index);
    TEST_ASSERT_EQUAL_STRING("/data/report.pdf", got->source_path);
    TEST_ASSERT_EQUAL_STRING("chunk text", got->text);
    lisa_chunk_free(got);
    lisa_chunk_free(NULL);
    TEST_ASSERT_EQUAL_INT(LISA_E_NOT_FOUND, lisa_collection_get_chunk(w, 999, &got));
    TEST_ASSERT_NULL(got);

    /* search: exact match first */
    lisa_hit_t hits[5];
    int64_t n = -1;
    vec_for(7, v);
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(w, v, NULL, hits, 5, &n));
    TEST_ASSERT_EQUAL_INT64(5, n);
    TEST_ASSERT_EQUAL_UINT64(7, hits[0].id);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, hits[0].distance);
    for (int i = 1; i < n; i++) TEST_ASSERT_TRUE(hits[i].distance >= hits[i - 1].distance);

    /* remove / remove_document */
    uint64_t seven = 7, missing[2] = { 1, 999 };
    TEST_ASSERT_EQUAL_INT(LISA_E_NOT_FOUND, lisa_collection_remove(w, 2, missing));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_remove(w, 1, &seven));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(w, v, NULL, hits, 5, &n));
    for (int i = 0; i < n; i++) TEST_ASSERT_NOT_EQUAL(7, hits[i].id);
    int64_t removed = -1;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_remove_document(w, "docB", &removed));
    TEST_ASSERT_EQUAL_INT64(5, removed);

    TEST_ASSERT_EQUAL_INT(LISA_E_READ_ONLY, lisa_collection_compact(r));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_compact(w));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_info(w, &info));
    TEST_ASSERT_EQUAL_INT64(9, info.chunk_count);

    lisa_collection_close(r);
    lisa_collection_close(w);
    lisa_collection_close(NULL);
    lisa_context_destroy(ctx);

    /* Every allocation LISA made through the injected allocator was freed. */
    TEST_ASSERT_TRUE(counts.allocs > 0);
    TEST_ASSERT_EQUAL(counts.allocs, counts.frees);
}

static void test_search_options(void) {
    lisa_context_t* ctx = NULL;
    lisa_collection_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_context_create(NULL, &ctx));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create(ctx, g_path, MODEL, DIM));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(ctx, g_path, LISA_OPEN_WRITE, NULL, &c));

    float q[DIM];
    vec_for(3, q);
    lisa_hit_t hits[20];
    int64_t n = -1;

    /* Empty collection: no hits. */
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(c, q, NULL, hits, 20, &n));
    TEST_ASSERT_EQUAL_INT64(0, n);

    add_n(c, "d", 20, 0);
    lisa_search_options_t o = LISA_SEARCH_OPTIONS_INIT;
    o.top_k = 12;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(c, q, &o, hits, 20, &n));
    TEST_ASSERT_EQUAL_INT64(12, n);
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(c, q, &o, hits, 4, &n));
    TEST_ASSERT_EQUAL_INT64(4, n);   /* capacity limits */
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(c, q, &o, NULL, 0, &n));
    TEST_ASSERT_EQUAL_INT64(0, n);

    o.top_k = 0;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_search_vector(c, q, &o, hits, 20, &n));
    o.top_k = 10001;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_search_vector(c, q, &o, hits, 20, &n));
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_search_vector(c, q, NULL, NULL, 5, &n));
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_collection_search_vector(c, NULL, NULL, hits, 5, &n));

    /* Older options struct without `principal`. */
    lisa_search_options_t old = LISA_SEARCH_OPTIONS_INIT;
    old.struct_size = offsetof(lisa_search_options_t, principal);
    old.top_k = 2;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(c, q, &old, hits, 20, &n));
    TEST_ASSERT_EQUAL_INT64(2, n);

    lisa_collection_close(c);
    lisa_context_destroy(ctx);
}

/* ---- extension points ------------------------------------------------- */

typedef struct {
    int calls;
    const char* last_user;
    int deny;
    int try_resurrect;
} filter_state_t;

/* Hides odd IDs; optionally tries (illegally) to re-enable every slot. */
static int even_only(void* user, const lisa_principal_t* p, const lisa_filter_input_t* in,
                     uint8_t* allowed) {
    filter_state_t* st = (filter_state_t*)user;
    st->calls++;
    st->last_user = p ? p->user_id : NULL;
    if (st->deny) return LISA_E_DENIED;
    for (int64_t i = 0; i < in->n_slots; i++) {
        if (st->try_resurrect) allowed[i] = 1;
        else if (allowed[i] && (in->chunk_ids[i] % 2) == 1) allowed[i] = 0;
    }
    return LISA_OK;
}

static void test_retrieval_filter(void) {
    filter_state_t st = { 0, NULL, 0, 0 };
    lisa_retrieval_filter_t f = { sizeof(lisa_retrieval_filter_t), &st, even_only };
    lisa_context_config_t cfg = LISA_CONTEXT_CONFIG_INIT;
    cfg.retrieval_filter = &f;
    lisa_context_t* ctx = NULL;
    lisa_collection_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_context_create(&cfg, &ctx));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create(ctx, g_path, MODEL, DIM));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(ctx, g_path, LISA_OPEN_WRITE, NULL, &c));
    add_n(c, "d", 30, 0);

    const char* groups[] = { "legal" };
    lisa_principal_t who = { "alice", groups, 1 };
    lisa_search_options_t o = LISA_SEARCH_OPTIONS_INIT;
    o.top_k = 10;
    o.principal = &who;
    float q[DIM];
    vec_for(5, q);   /* ID 5 is odd: hidden */
    lisa_hit_t hits[10];
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(c, q, &o, hits, 10, &n));
    TEST_ASSERT_EQUAL_INT(1, st.calls);
    TEST_ASSERT_EQUAL_STRING("alice", st.last_user);
    TEST_ASSERT_EQUAL_INT64(10, n);
    for (int i = 0; i < n; i++) TEST_ASSERT_EQUAL_UINT64(0, hits[i].id % 2);

    /* A filter cannot resurrect removed chunks. */
    uint64_t gone[2] = { 4, 6 };
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_remove(c, 2, gone));
    st.try_resurrect = 1;
    o.top_k = 30;
    lisa_hit_t all[30];
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(c, q, &o, all, 30, &n));
    TEST_ASSERT_EQUAL_INT64(28, n);
    for (int i = 0; i < n; i++) TEST_ASSERT_TRUE(all[i].id != 4 && all[i].id != 6);

    /* A filter error fails the search. */
    st.deny = 1;
    TEST_ASSERT_EQUAL_INT(LISA_E_DENIED, lisa_collection_search_vector(c, q, &o, all, 30, &n));
    TEST_ASSERT_EQUAL_INT64(0, n);

    lisa_collection_close(c);
    lisa_context_destroy(ctx);
}

typedef struct {
    int n;
    lisa_audit_action actions[32];
    int statuses[32];
    int64_t counts[32];
    uint64_t first_id[32];
    char user[32][16];
} audit_log_t;

static void record(void* u, const lisa_audit_event_t* ev) {
    audit_log_t* log = (audit_log_t*)u;
    if (log->n >= 32) return;
    int i = log->n++;
    log->actions[i] = ev->action;
    log->statuses[i] = ev->status;
    log->counts[i] = ev->item_count;
    log->first_id[i] = (ev->chunk_ids && ev->item_count > 0) ? ev->chunk_ids[0] : UINT64_MAX;
    snprintf(log->user[i], sizeof(log->user[i]), "%s",
             ev->principal && ev->principal->user_id ? ev->principal->user_id : "-");
}

static void test_audit_sink(void) {
    static audit_log_t log;
    memset(&log, 0, sizeof(log));
    lisa_audit_sink_t sink = { sizeof(lisa_audit_sink_t), &log, record };
    lisa_context_config_t cfg = LISA_CONTEXT_CONFIG_INIT;
    cfg.audit = &sink;
    lisa_context_t* ctx = NULL;
    lisa_collection_t* c = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_context_create(&cfg, &ctx));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_create(ctx, g_path, MODEL, DIM));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_open(ctx, g_path, LISA_OPEN_WRITE, NULL, &c));
    add_n(c, "d", 3, 0);

    lisa_principal_t who = { "bob", NULL, 0 };
    lisa_search_options_t o = LISA_SEARCH_OPTIONS_INIT;
    o.principal = &who;
    float q[DIM];
    vec_for(2, q);
    lisa_hit_t hits[5];
    int64_t n;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_collection_search_vector(c, q, &o, hits, 5, &n));
    uint64_t missing = 42;
    TEST_ASSERT_EQUAL_INT(LISA_E_NOT_FOUND, lisa_collection_remove(c, 1, &missing));
    lisa_collection_close(c);
    lisa_context_destroy(ctx);

    TEST_ASSERT_EQUAL_INT(5, log.n);
    TEST_ASSERT_EQUAL_INT(LISA_AUDIT_COLLECTION_CREATE, log.actions[0]);
    TEST_ASSERT_EQUAL_INT(LISA_AUDIT_COLLECTION_OPEN, log.actions[1]);
    TEST_ASSERT_EQUAL_INT(LISA_AUDIT_CHUNKS_ADD, log.actions[2]);
    TEST_ASSERT_EQUAL_INT64(3, log.counts[2]);
    TEST_ASSERT_EQUAL_UINT64(0, log.first_id[2]);
    TEST_ASSERT_EQUAL_INT(LISA_AUDIT_SEARCH, log.actions[3]);
    TEST_ASSERT_EQUAL_INT64(3, log.counts[3]);
    TEST_ASSERT_EQUAL_UINT64(2, log.first_id[3]);
    TEST_ASSERT_EQUAL_STRING("bob", log.user[3]);
    TEST_ASSERT_EQUAL_INT(LISA_AUDIT_CHUNKS_REMOVE, log.actions[4]);
    TEST_ASSERT_EQUAL_INT(LISA_E_NOT_FOUND, log.statuses[4]);
    TEST_ASSERT_EQUAL_INT64(0, log.counts[4]);
}

static int noop_crypt(void* u, const void* in, size_t len, void* out, uint64_t nonce) {
    (void)u; (void)nonce;
    memcpy(out, in, len);
    return LISA_OK;
}

static void test_storage_crypto_refuses_until_supported(void) {
    lisa_storage_crypto_t cr = { sizeof(lisa_storage_crypto_t), NULL, noop_crypt, noop_crypt };
    lisa_context_config_t cfg = LISA_CONTEXT_CONFIG_INIT;
    cfg.storage_crypto = &cr;
    lisa_context_t* ctx = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_context_create(&cfg, &ctx));
    TEST_ASSERT_EQUAL_INT(LISA_E_UNSUPPORTED, lisa_collection_create(ctx, g_path, MODEL, DIM));
    lisa_collection_t* c = (lisa_collection_t*)1;
    TEST_ASSERT_EQUAL_INT(LISA_E_UNSUPPORTED,
                          lisa_collection_open(ctx, g_path, LISA_OPEN_READ, NULL, &c));
    TEST_ASSERT_NULL(c);
    lisa_context_destroy(ctx);
}

static int noop_auth(void* u, const lisa_auth_request_t* r, lisa_principal_t* out) {
    (void)u; (void)r;
    out->user_id = "local";
    out->groups = NULL;
    out->group_count = 0;
    return LISA_OK;
}

static int noop_route(void* u, const lisa_http_request_t* r, lisa_http_response_t* resp) {
    (void)u; (void)r; (void)resp;
    return LISA_E_NOT_FOUND;
}

static void test_auth_and_routes_are_installed(void) {
    lisa_auth_provider_t au = { sizeof(lisa_auth_provider_t), NULL, noop_auth, NULL };
    lisa_http_routes_t rt = { sizeof(lisa_http_routes_t), NULL, noop_route };
    lisa_context_config_t cfg = LISA_CONTEXT_CONFIG_INIT;
    cfg.auth = &au;
    cfg.http_routes = &rt;
    lisa_context_t* ctx = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_context_create(&cfg, &ctx));

    const lisa_auth_provider_t* a = lisa_context_auth(ctx);
    TEST_ASSERT_NOT_NULL(a);
    lisa_principal_t p;
    lisa_auth_request_t req = { sizeof(req), "GET", "/v1/health", NULL, NULL, 0, "127.0.0.1" };
    TEST_ASSERT_EQUAL_INT(LISA_OK, a->authenticate(a->user, &req, &p));
    TEST_ASSERT_EQUAL_STRING("local", p.user_id);

    const lisa_http_routes_t* r = lisa_context_http_routes(ctx);
    TEST_ASSERT_NOT_NULL(r);
    lisa_http_response_t resp = { sizeof(resp), 0, NULL, NULL, 0 };
    lisa_http_request_t hr = { sizeof(hr), "GET", "/x", NULL, 0, NULL };
    TEST_ASSERT_EQUAL_INT(LISA_E_NOT_FOUND, r->handle(r->user, &hr, &resp));
    lisa_context_destroy(ctx);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch_dir>\n", argv[0]);
        return 2;
    }
    g_scratch = argv[1];
    mkdir(g_scratch, 0755);

    UNITY_BEGIN();
    RUN_TEST(test_version);
    RUN_TEST(test_status_strings);
    RUN_TEST(test_context_config_validation);
    RUN_TEST(test_collection_lifecycle_and_errors);
    RUN_TEST(test_search_options);
    RUN_TEST(test_retrieval_filter);
    RUN_TEST(test_audit_sink);
    RUN_TEST(test_storage_crypto_refuses_until_supported);
    RUN_TEST(test_auth_and_routes_are_installed);
    return UNITY_END() == 0 ? 0 : 1;
}
