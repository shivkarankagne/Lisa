/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * test_store — storage v2 contract (src/storage/store.h).
 *
 * Usage: test_store <scratch_dir>
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "../src/platform/platform.h"
#include "../src/retrieval/retrieval.h"
#include "../src/storage/storage.h"
#include "../src/storage/store.h"
#include "sqlite3.h"

#define DIM 13
#define MODEL "test-embed-v1"

static const char* g_scratch;
static char g_dir[512];
static int g_seq = 0;

/* Deterministic vector for an ID, so content can be checked after any operation. */
static void vec_for(uint64_t id, float* out) {
    for (int j = 0; j < DIM; j++) out[j] = (float)((id * 31 + (uint64_t)j * 7) % 101) / 101.0f;
}

static lisa_store_chunk_t chunk_for(const char* doc, int64_t i) {
    lisa_store_chunk_t c;
    c.doc_id = (char*)doc;
    c.chunk_index = i;
    c.source_path = (char*)"/docs/file.txt";
    c.offset = i * 100;
    c.length = 100;
    c.text = (char*)"some chunk text";
    c.content_hash = (char*)"abc123";
    c.page = 0;
    return c;
}

/* Insert n chunks for doc; vectors derived from the IDs they will get. */
static void insert_n(lisa_store_t* s, const char* doc, int64_t n, uint64_t first_expected_id,
                     uint64_t* ids) {
    float* v = malloc((size_t)(n * DIM) * sizeof(float));
    lisa_store_chunk_t* c = malloc((size_t)n * sizeof(lisa_store_chunk_t));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_NOT_NULL(c);
    for (int64_t i = 0; i < n; i++) {
        vec_for(first_expected_id + (uint64_t)i, v + i * DIM);
        c[i] = chunk_for(doc, i);
    }
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_insert(s, n, v, c, ids));
    free(v);
    free(c);
}

static void check_vector(lisa_store_t* s, uint64_t id) {
    float got[DIM], want[DIM];
    vec_for(id, want);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_get_vector(s, id, got));
    TEST_ASSERT_EQUAL_MEMORY(want, got, sizeof(want));
}

void setUp(void) {
    snprintf(g_dir, sizeof(g_dir), "%s/store_%lld_%d", g_scratch,
             (long long)lisa_time_monotonic_ns(), g_seq++);
}

void tearDown(void) {}

/* ---- create / open ---------------------------------------------------- */

static void test_create_validates_and_refuses_existing(void) {
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EINVAL, lisa_store_create(NULL, MODEL, DIM));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EINVAL, lisa_store_create(g_dir, "", DIM));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EINVAL, lisa_store_create(g_dir, MODEL, 0));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EINVAL, lisa_store_create(g_dir, MODEL, 65537));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_dir, MODEL, DIM));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EEXIST, lisa_store_create(g_dir, MODEL, DIM));

    char* db = lisa_path_join(g_dir, "meta.sqlite");
    char* vf = lisa_path_join(g_dir, "vectors.0.lisa");
    TEST_ASSERT_TRUE(lisa_path_exists(db));
    TEST_ASSERT_EQUAL_INT64(64, lisa_file_size(vf));
    free(db);
    free(vf);
}

static void test_open_errors(void) {
    lisa_store_t* s = (lisa_store_t*)1;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_ENOTFOUND, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &s));
    TEST_ASSERT_NULL(s);

    /* A directory that is not a v2 collection (e.g. v1). */
    TEST_ASSERT_EQUAL_INT(LISA_PLAT_OK, lisa_mkdir(g_dir));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EFORMAT, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &s));

    char other[600];
    snprintf(other, sizeof(other), "%s_m", g_dir);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(other, MODEL, DIM));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EMODEL,
                          lisa_store_open(other, LISA_STORE_READ, "other-model", &s));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EINVAL, lisa_store_open(other, 7, NULL, &s));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(other, LISA_STORE_READ, MODEL, &s));
    TEST_ASSERT_EQUAL_STRING(MODEL, lisa_store_model(s));
    TEST_ASSERT_EQUAL_INT64(DIM, lisa_store_dim(s));
    TEST_ASSERT_EQUAL_INT64(0, lisa_store_count(s));
    lisa_store_view_t view;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_view(s, &view));
    TEST_ASSERT_EQUAL_INT64(0, view.n_slots);
    lisa_store_close(s);
    lisa_store_close(NULL);
}

static void test_single_writer(void) {
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_dir, MODEL, DIM));
    lisa_store_t *w1 = NULL, *w2 = NULL, *r = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_WRITE, NULL, &w1));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EBUSY, lisa_store_open(g_dir, LISA_STORE_WRITE, NULL, &w2));
    TEST_ASSERT_NULL(w2);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &r));

    float v[DIM];
    lisa_store_chunk_t c = chunk_for("d", 0);
    vec_for(0, v);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EREADONLY, lisa_store_insert(r, 1, v, &c, NULL));
    uint64_t id = 0;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EREADONLY, lisa_store_delete(r, 1, &id));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EREADONLY, lisa_store_compact(r));

    lisa_store_close(w1);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_WRITE, NULL, &w2));
    lisa_store_close(w2);
    lisa_store_close(r);
}

/* ---- insert / get / delete -------------------------------------------- */

static void test_insert_get_and_stable_ids(void) {
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_dir, MODEL, DIM));
    lisa_store_t* s = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_WRITE, MODEL, &s));

    uint64_t ids[5];
    insert_n(s, "docA", 5, 0, ids);
    for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_UINT64((uint64_t)i, ids[i]);
    TEST_ASSERT_EQUAL_INT64(5, lisa_store_count(s));

    lisa_store_chunk_t got;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_get(s, 3, &got));
    TEST_ASSERT_EQUAL_STRING("docA", got.doc_id);
    TEST_ASSERT_EQUAL_INT64(3, got.chunk_index);
    TEST_ASSERT_EQUAL_STRING("/docs/file.txt", got.source_path);
    TEST_ASSERT_EQUAL_INT64(300, got.offset);
    TEST_ASSERT_EQUAL_INT64(100, got.length);
    TEST_ASSERT_EQUAL_STRING("some chunk text", got.text);
    TEST_ASSERT_EQUAL_STRING("abc123", got.content_hash);
    lisa_store_chunk_free(&got);
    for (uint64_t i = 0; i < 5; i++) check_vector(s, i);

    /* Delete two; the others keep their IDs and vectors. */
    uint64_t del[2] = { 1, 3 };
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_delete(s, 2, del));
    TEST_ASSERT_EQUAL_INT64(3, lisa_store_count(s));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_ENOTFOUND, lisa_store_get(s, 1, &got));
    float tmp[DIM];
    TEST_ASSERT_EQUAL_INT(LISA_STORE_ENOTFOUND, lisa_store_get_vector(s, 3, tmp));
    check_vector(s, 0);
    check_vector(s, 2);
    check_vector(s, 4);

    /* IDs are never reused. */
    insert_n(s, "docB", 2, 5, ids);
    TEST_ASSERT_EQUAL_UINT64(5, ids[0]);
    TEST_ASSERT_EQUAL_UINT64(6, ids[1]);
    check_vector(s, 6);
    lisa_store_close(s);
}

static void test_insert_and_delete_are_all_or_nothing(void) {
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_dir, MODEL, DIM));
    lisa_store_t* s = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_WRITE, NULL, &s));
    insert_n(s, "d", 3, 0, NULL);

    /* A NULL string in the second record rejects the whole batch. */
    float v[2 * DIM] = { 0 };
    lisa_store_chunk_t c[2] = { chunk_for("x", 0), chunk_for("x", 1) };
    c[1].text = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EINVAL, lisa_store_insert(s, 2, v, c, NULL));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EINVAL, lisa_store_insert(s, 0, v, c, NULL));
    TEST_ASSERT_EQUAL_INT64(3, lisa_store_count(s));

    /* One unknown ID: nothing deleted. */
    uint64_t ids[2] = { 0, 99 };
    TEST_ASSERT_EQUAL_INT(LISA_STORE_ENOTFOUND, lisa_store_delete(s, 2, ids));
    TEST_ASSERT_EQUAL_INT64(3, lisa_store_count(s));
    check_vector(s, 0);

    /* delete_doc */
    insert_n(s, "other", 2, 3, NULL);
    int64_t n = -1;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_delete_doc(s, "d", &n));
    TEST_ASSERT_EQUAL_INT64(3, n);
    TEST_ASSERT_EQUAL_INT64(2, lisa_store_count(s));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_delete_doc(s, "missing", &n));
    TEST_ASSERT_EQUAL_INT64(0, n);
    check_vector(s, 3);
    check_vector(s, 4);
    lisa_store_close(s);
}

/* ---- persistence, refresh, search ------------------------------------- */

static void test_persistence_and_refresh(void) {
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_dir, MODEL, DIM));
    lisa_store_t *w = NULL, *r = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_WRITE, NULL, &w));
    insert_n(w, "d", 10, 0, NULL);

    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &r));
    TEST_ASSERT_EQUAL_INT64(10, lisa_store_count(r));

    insert_n(w, "d", 5, 10, NULL);
    uint64_t gone = 2;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_delete(w, 1, &gone));

    /* The reader sees the new state only after refresh. */
    TEST_ASSERT_EQUAL_INT64(10, lisa_store_count(r));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_refresh(r));
    TEST_ASSERT_EQUAL_INT64(14, lisa_store_count(r));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_refresh(r)); /* no change: cheap no-op */
    check_vector(r, 14);
    lisa_store_close(r);
    lisa_store_close(w);

    /* Everything survives close and reopen. */
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &r));
    TEST_ASSERT_EQUAL_INT64(14, lisa_store_count(r));
    for (uint64_t id = 0; id < 15; id++) {
        if (id == 2) continue;
        check_vector(r, id);
    }
    lisa_store_close(r);
}

static void test_search_through_view_skips_deleted(void) {
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_dir, MODEL, DIM));
    lisa_store_t* s = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_WRITE, NULL, &s));
    insert_n(s, "d", 50, 0, NULL);

    /* Query equal to vector 7: it must be the top hit until deleted. */
    float q[DIM];
    vec_for(7, q);
    int idx[3];
    float dist[3];
    lisa_result_t r = { idx, dist, 3, 0 };
    lisa_store_view_t v;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_view(s, &v));
    TEST_ASSERT_EQUAL_INT(0, lisa_search_masked(q, v.vectors, v.n_slots, v.dim, 3, v.live, &r));
    TEST_ASSERT_EQUAL_UINT64(7, v.slot_ids[idx[0]]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, dist[0]);

    uint64_t seven = 7;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_delete(s, 1, &seven));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_view(s, &v));
    TEST_ASSERT_EQUAL_INT(0, lisa_search_masked(q, v.vectors, v.n_slots, v.dim, 3, v.live, &r));
    for (int i = 0; i < r.n_returned; i++) {
        TEST_ASSERT_TRUE(v.live[idx[i]]);
        TEST_ASSERT_NOT_EQUAL(7, v.slot_ids[idx[i]]);
    }
    lisa_store_close(s);
}

/* ---- compaction ------------------------------------------------------- */

static void test_compaction_keeps_ids_and_vectors(void) {
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_dir, MODEL, DIM));
    lisa_store_t *w = NULL, *r = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_WRITE, NULL, &w));
    insert_n(w, "d", 20, 0, NULL);
    uint64_t del[5] = { 0, 3, 4, 10, 19 };
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_delete(w, 5, del));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &r));

    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_compact(w));
    lisa_store_view_t v;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_view(w, &v));
    TEST_ASSERT_EQUAL_INT64(15, v.n_slots);
    for (int64_t i = 0; i < v.n_slots; i++) TEST_ASSERT_TRUE(v.live[i]);

    char* old_file = lisa_path_join(g_dir, "vectors.0.lisa");
    char* new_file = lisa_path_join(g_dir, "vectors.1.lisa");
    TEST_ASSERT_FALSE(lisa_path_exists(old_file));
    TEST_ASSERT_EQUAL_INT64(64 + 15 * DIM * 4, lisa_file_size(new_file));
    free(old_file);
    free(new_file);

    for (uint64_t id = 0; id < 20; id++) {
        int deleted = id == 0 || id == 3 || id == 4 || id == 10 || id == 19;
        if (!deleted) check_vector(w, id);
    }

    /* The reader opened before compaction still works and moves over on refresh. */
    check_vector(r, 5);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_refresh(r));
    TEST_ASSERT_EQUAL_INT64(15, lisa_store_count(r));
    check_vector(r, 18);

    /* Inserts continue after compaction with fresh IDs. */
    uint64_t id;
    insert_n(w, "d", 1, 20, &id);
    TEST_ASSERT_EQUAL_UINT64(20, id);
    check_vector(w, 20);
    lisa_store_close(r);
    lisa_store_close(w);

    /* Reopen after compaction. */
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &r));
    TEST_ASSERT_EQUAL_INT64(16, lisa_store_count(r));
    check_vector(r, 20);
    check_vector(r, 1);
    lisa_store_close(r);
}

/* ---- format checks ---------------------------------------------------- */

static void test_corrupt_vector_header_is_rejected(void) {
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_dir, MODEL, DIM));
    char* vf = lisa_path_join(g_dir, "vectors.0.lisa");
    FILE* f = fopen(vf, "r+b");
    TEST_ASSERT_NOT_NULL(f);
    fwrite("XXXX", 1, 4, f);
    fclose(f);
    free(vf);
    lisa_store_t* s = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EFORMAT, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &s));
}

static void test_truncated_vector_file_is_rejected(void) {
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_dir, MODEL, DIM));
    lisa_store_t* s = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_WRITE, NULL, &s));
    insert_n(s, "d", 4, 0, NULL);
    lisa_store_close(s);

    /* Replace the vector file with only its header: metadata now points past the end. */
    char* vf = lisa_path_join(g_dir, "vectors.0.lisa");
    FILE* f = fopen(vf, "rb");
    unsigned char hdr[64];
    TEST_ASSERT_EQUAL_size_t(64, fread(hdr, 1, 64, f));
    fclose(f);
    f = fopen(vf, "wb");
    fwrite(hdr, 1, 64, f);
    fclose(f);
    free(vf);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EFORMAT, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &s));
}

/* ---- migration -------------------------------------------------------- */

static void test_migrate_v1_preserves_order_as_ids(void) {
    const int n = 300, dim = 5;
    float* vecs = malloc((size_t)n * dim * sizeof(float));
    TEST_ASSERT_NOT_NULL(vecs);
    for (int i = 0; i < n * dim; i++) vecs[i] = (float)(i % 17) * 0.25f;

    char src[600], dst[600];
    snprintf(src, sizeof(src), "%s_v1", g_dir);
    snprintf(dst, sizeof(dst), "%s_v2", g_dir);
    TEST_ASSERT_EQUAL_INT(0, storage_create(src, n, dim, vecs));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_migrate_v1(src, dst, MODEL));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EEXIST, lisa_store_migrate_v1(src, dst, MODEL));

    lisa_store_t* s = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(dst, LISA_STORE_READ, MODEL, &s));
    TEST_ASSERT_EQUAL_INT64(n, lisa_store_count(s));
    TEST_ASSERT_EQUAL_INT64(dim, lisa_store_dim(s));
    float got[5];
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_get_vector(s, (uint64_t)i, got));
        TEST_ASSERT_EQUAL_MEMORY(vecs + i * dim, got, sizeof(got));
    }

    /* Search results: old indices equal new IDs. */
    int io[5], in[5];
    float dout[5], dnew[5];
    lisa_result_t ro = { io, dout, 5, 0 }, rn = { in, dnew, 5, 0 };
    TEST_ASSERT_EQUAL_INT(0, lisa_search(vecs + 42 * dim, vecs, n, dim, 5, &ro));
    lisa_store_view_t v;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_view(s, &v));
    TEST_ASSERT_EQUAL_INT(0, lisa_search_masked(vecs + 42 * dim, v.vectors, v.n_slots, v.dim,
                                                5, v.live, &rn));
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT64((uint64_t)io[i], v.slot_ids[in[i]]);
        TEST_ASSERT_EQUAL_FLOAT(dout[i], dnew[i]);
    }
    lisa_store_close(s);

    TEST_ASSERT_EQUAL_INT(LISA_STORE_ENOTFOUND, lisa_store_migrate_v1(g_dir, dst, MODEL));
    free(vecs);
}


/* ---- document records (format v2) ------------------------------------ */

static lisa_store_doc_t doc_rec(const char* id, const char* status, int64_t size) {
    lisa_store_doc_t d;
    memset(&d, 0, sizeof(d));
    d.doc_id = (char*)id;
    d.source_path = (char*)id;
    d.content_hash = (char*)"hash-1";
    d.size = size;
    d.mtime_ns = 1000;
    d.title = (char*)"A Title";
    d.status = (char*)status;
    d.message = (char*)"";
    return d;
}

typedef struct {
    char paths[8][128];
    int  n;
} doc_seen_t;

static int collect_doc(void* user, const lisa_store_doc_t* d) {
    doc_seen_t* seen = (doc_seen_t*)user;
    snprintf(seen->paths[seen->n++], 128, "%s", d->source_path);
    return 0;
}

static void test_replace_doc_and_records(void) {
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_dir, MODEL, DIM));
    lisa_store_t *s = NULL, *r = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_WRITE, NULL, &s));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &r));

    /* First version: 3 chunks, with pages. */
    float v[3 * DIM];
    lisa_store_chunk_t c[3];
    for (int i = 0; i < 3; i++) {
        vec_for((uint64_t)i, v + i * DIM);
        c[i] = chunk_for("ignored", i);
        c[i].page = i + 1;
    }
    lisa_store_doc_t d = doc_rec("/docs/a.pdf", "ok", 100);
    uint64_t ids[3];
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_replace_doc(s, &d, 3, v, c, ids));
    TEST_ASSERT_EQUAL_INT64(3, lisa_store_count(s));

    lisa_store_chunk_t got;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_get(s, ids[2], &got));
    TEST_ASSERT_EQUAL_STRING("/docs/a.pdf", got.doc_id);   /* doc_id forced to the record's */
    TEST_ASSERT_EQUAL_INT64(3, got.page);
    lisa_store_chunk_free(&got);

    lisa_store_doc_t rec;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_doc_get(s, "/docs/a.pdf", &rec));
    TEST_ASSERT_EQUAL_INT64(3, rec.chunk_count);
    TEST_ASSERT_EQUAL_STRING("ok", rec.status);
    TEST_ASSERT_EQUAL_STRING("A Title", rec.title);
    TEST_ASSERT_EQUAL_INT64(100, rec.size);
    lisa_store_doc_free(&rec);

    /* Second version replaces the first atomically: old IDs gone, new ones fresh. */
    d.content_hash = (char*)"hash-2";
    uint64_t ids2[2];
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_replace_doc(s, &d, 2, v, c, ids2));
    TEST_ASSERT_EQUAL_INT64(2, lisa_store_count(s));
    TEST_ASSERT_TRUE(ids2[0] > ids[2]);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_ENOTFOUND, lisa_store_get(s, ids[0], &got));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_doc_get(s, "/docs/a.pdf", &rec));
    TEST_ASSERT_EQUAL_STRING("hash-2", rec.content_hash);
    TEST_ASSERT_EQUAL_INT64(2, rec.chunk_count);
    lisa_store_doc_free(&rec);

    /* A document with no text: record kept, no chunks. */
    lisa_store_doc_t e = doc_rec("/docs/scan.pdf", "no_text", 5);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_replace_doc(s, &e, 0, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_doc_get(s, "/docs/scan.pdf", &rec));
    TEST_ASSERT_EQUAL_STRING("no_text", rec.status);
    TEST_ASSERT_EQUAL_INT64(0, rec.chunk_count);
    lisa_store_doc_free(&rec);

    /* touch */
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_doc_touch(s, "/docs/scan.pdf", 6, 2000));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_doc_get(s, "/docs/scan.pdf", &rec));
    TEST_ASSERT_EQUAL_INT64(6, rec.size);
    TEST_ASSERT_EQUAL_INT64(2000, rec.mtime_ns);
    lisa_store_doc_free(&rec);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_ENOTFOUND, lisa_store_doc_touch(s, "/nope", 1, 1));

    /* list by prefix, ordered */
    lisa_store_doc_t other = doc_rec("/other/b.md", "ok", 1);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_replace_doc(s, &other, 1, v, c, NULL));
    doc_seen_t seen;
    memset(&seen, 0, sizeof(seen));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_doc_list(s, "/docs/", collect_doc, &seen));
    TEST_ASSERT_EQUAL_INT(2, seen.n);
    TEST_ASSERT_EQUAL_STRING("/docs/a.pdf", seen.paths[0]);
    TEST_ASSERT_EQUAL_STRING("/docs/scan.pdf", seen.paths[1]);
    memset(&seen, 0, sizeof(seen));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_doc_list(s, NULL, collect_doc, &seen));
    TEST_ASSERT_EQUAL_INT(3, seen.n);

    /* delete_doc removes chunks and record */
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_delete_doc(s, "/docs/a.pdf", &n));
    TEST_ASSERT_EQUAL_INT64(2, n);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_ENOTFOUND, lisa_store_doc_get(s, "/docs/a.pdf", &rec));

    /* readers see it after refresh; read-only handles cannot write */
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_refresh(r));
    TEST_ASSERT_EQUAL_INT64(1, lisa_store_count(r));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EREADONLY, lisa_store_replace_doc(r, &d, 0, NULL, NULL, NULL));
    lisa_store_doc_t bad = doc_rec("/x", "ok", 1);
    bad.status = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EINVAL, lisa_store_replace_doc(s, &bad, 0, NULL, NULL, NULL));
    lisa_store_close(r);
    lisa_store_close(s);
}

/* Make a v2 collection look exactly like format v1 on disk. */
static void downgrade_to_v1(const char* dir) {
    char* db = lisa_path_join(dir, "meta.sqlite");
    sqlite3* h = NULL;
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(db, &h));
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(h,
        "DROP TABLE documents;"
        "ALTER TABLE chunks DROP COLUMN page;"
        "UPDATE meta SET value = 1 WHERE key = 'format_version';", NULL, NULL, NULL));
    sqlite3_close(h);
    free(db);
}

static int count_backup(void* user, const char* path, const lisa_file_info_t* info) {
    (void)info;
    const char* base = strrchr(path, '/');
    if (base && strncmp(base + 1, "meta.v1-backup-", 15) == 0) (*(int*)user)++;
    return 0;
}

static int count_backups(const char* dir) {
    int n = 0;
    TEST_ASSERT_EQUAL_INT(LISA_PLAT_OK, lisa_dir_walk(dir, 0, count_backup, &n));
    return n;
}

static void test_migrate_format_v1_to_v2(void) {
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_dir, MODEL, DIM));
    lisa_store_t* s = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_WRITE, NULL, &s));
    insert_n(s, "d", 4, 0, NULL);
    lisa_store_close(s);
    downgrade_to_v1(g_dir);

    /* A read-only open migrates (taking the writer lock briefly). */
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &s));
    TEST_ASSERT_EQUAL_INT64(4, lisa_store_count(s));
    for (uint64_t id = 0; id < 4; id++) check_vector(s, id);
    lisa_store_chunk_t got;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_get(s, 1, &got));
    TEST_ASSERT_EQUAL_INT64(0, got.page);
    lisa_store_chunk_free(&got);
    lisa_store_close(s);
    TEST_ASSERT_EQUAL_INT(1, count_backups(g_dir));

    /* Opening again does not migrate or back up again; v2 features work. */
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_dir, LISA_STORE_WRITE, NULL, &s));
    lisa_store_doc_t d = doc_rec("/docs/new.txt", "ok", 1);
    float v[DIM];
    lisa_store_chunk_t c = chunk_for("x", 0);
    vec_for(9, v);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_replace_doc(s, &d, 1, v, &c, NULL));
    lisa_store_close(s);
    TEST_ASSERT_EQUAL_INT(1, count_backups(g_dir));

    /* A newer format than this build understands is refused. */
    char* db = lisa_path_join(g_dir, "meta.sqlite");
    sqlite3* h = NULL;
    sqlite3_open(db, &h);
    sqlite3_exec(h, "UPDATE meta SET value = 99 WHERE key = 'format_version';", NULL, NULL, NULL);
    sqlite3_close(h);
    free(db);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_EFORMAT, lisa_store_open(g_dir, LISA_STORE_READ, NULL, &s));
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch_dir>\n", argv[0]);
        return 2;
    }
    g_scratch = argv[1];
    lisa_mkdir(g_scratch);

    UNITY_BEGIN();
    RUN_TEST(test_create_validates_and_refuses_existing);
    RUN_TEST(test_open_errors);
    RUN_TEST(test_single_writer);
    RUN_TEST(test_insert_get_and_stable_ids);
    RUN_TEST(test_insert_and_delete_are_all_or_nothing);
    RUN_TEST(test_persistence_and_refresh);
    RUN_TEST(test_search_through_view_skips_deleted);
    RUN_TEST(test_compaction_keeps_ids_and_vectors);
    RUN_TEST(test_corrupt_vector_header_is_rejected);
    RUN_TEST(test_truncated_vector_file_is_rejected);
    RUN_TEST(test_migrate_v1_preserves_order_as_ids);
    RUN_TEST(test_replace_doc_and_records);
    RUN_TEST(test_migrate_format_v1_to_v2);
    return UNITY_END() == 0 ? 0 : 1;
}
