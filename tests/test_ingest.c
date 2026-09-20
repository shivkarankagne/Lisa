/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * test_ingest — folder sync (W6).
 *
 * Usage: test_ingest <scratch_dir> <fixtures_dir> <models_dir>
 *
 * Uses a deterministic fake embedder (fast, no model), which also counts
 * embed calls so tests can prove unchanged files are not re-embedded. One
 * test uses the real embedding model when it is present in <models_dir>.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "unity.h"
#include "../src/ingest/ingest.h"
#include "../src/models/models.h"
#include "../src/platform/platform.h"
#include "../src/retrieval/retrieval.h"

#define DIM 8

static const char* g_scratch;
static const char* g_fixtures;
static const char* g_models;
static char g_root[800];
static char g_coll[800];
static int g_seq;

/* ---- fake embedder ------------------------------------------------------ */

typedef struct {
    int64_t calls;
    int64_t texts;
} fake_t;

static void fake_vec(const char* text, float* out, int64_t dim) {
    uint64_t h = 1469598103934665603ULL;  /* FNV-1a */
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) h = (h ^ *p) * 1099511628211ULL;
    double norm = 0;
    for (int64_t j = 0; j < dim; j++) {
        h = h * 6364136223846793005ULL + 1442695040888963407ULL;
        out[j] = (float)((double)(h >> 11) / 9007199254740992.0 - 0.5);
        norm += (double)out[j] * out[j];
    }
    for (int64_t j = 0; j < dim; j++) out[j] = (float)(out[j] / sqrt(norm));
}

static int fake_embed(void* user, const char* const* texts, int64_t n, float* out, int64_t dim) {
    fake_t* f = (fake_t*)user;
    f->calls++;
    for (int64_t i = 0; i < n; i++) {
        if (strstr(texts[i], "FAILEMBED")) return 1;
        fake_vec(texts[i], out + i * dim, dim);
        f->texts++;
    }
    return 0;
}

/* ---- helpers ------------------------------------------------------------ */

static void write_text(const char* rel, const char* text) {
    char path[1000];
    snprintf(path, sizeof(path), "%s/%s", g_root, rel);
    FILE* f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
}

static void set_mtime(const char* rel, long seconds) {
    char path[1000];
    snprintf(path, sizeof(path), "%s/%s", g_root, rel);
    struct timeval tv[2] = { { seconds, 0 }, { seconds, 0 } };
    TEST_ASSERT_EQUAL_INT(0, utimes(path, tv));
}

static void remove_rel(const char* rel) {
    char path[1000];
    snprintf(path, sizeof(path), "%s/%s", g_root, rel);
    TEST_ASSERT_EQUAL_INT(LISA_PLAT_OK, lisa_remove_file(path));
}

static lisa_store_t* open_store(void) {
    lisa_store_t* s = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(g_coll, LISA_STORE_WRITE, NULL, &s));
    return s;
}

static int run(lisa_store_t* s, fake_t* f, const char* path, ingest_progress_t* out) {
    ingest_params_t p;
    memset(&p, 0, sizeof(p));
    p.store = s;
    p.embed = fake_embed;
    p.embed_user = f;
    doc_chunk_params_t cp = { 200, 300, 40 };
    p.chunk = cp;
    const char* paths[1] = { path };
    return ingest_run(&p, paths, 1, out, NULL);
}

static char* abs_of(const char* rel) {
    char path[1000];
    snprintf(path, sizeof(path), "%s/%s", g_root, rel);
    return lisa_path_absolute(path);
}

static const char* status_of(lisa_store_t* s, const char* rel, lisa_store_doc_t* rec) {
    char* a = abs_of(rel);
    int rc = a ? lisa_store_doc_get(s, a, rec) : LISA_STORE_ENOTFOUND;
    free(a);
    return rc == LISA_STORE_OK ? rec->status : NULL;
}

void setUp(void) {
    int n = g_seq++;
    snprintf(g_root, sizeof(g_root), "%s/ingest_%lld_%d", g_scratch, (long long)lisa_time_monotonic_ns(), n);
    snprintf(g_coll, sizeof(g_coll), "%s.coll", g_root);
    TEST_ASSERT_EQUAL_INT(LISA_PLAT_OK, lisa_mkdir(g_root));
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(g_coll, "fake-embed", DIM));
}

void tearDown(void) {}

/* A folder with every kind of file. */
static void make_folder(void) {
    char sub[900];
    snprintf(sub, sizeof(sub), "%s/sub", g_root);
    lisa_mkdir(sub);
    write_text("a.txt", "Pump bearings must be inspected every 500 hours.\n\n"
                        "Vibration above 40 Hz indicates wear.");
    write_text("b.md", "# Leave Policy\n\nEmployees get 20 days of annual leave.");
    write_text("sub/c.txt", "The repo rate was raised by 25 basis points.");
    write_text("notes.zip", "not a document");
    write_text(".hidden.txt", "hidden text");
    write_text("empty.txt", "   \n\n  ");
    write_text("bad.pdf", "this is not really a pdf");
}

/* ---- tests -------------------------------------------------------------- */

static void test_first_run_indexes_supported_files(void) {
    make_folder();
    lisa_store_t* s = open_store();
    fake_t f = { 0, 0 };
    ingest_progress_t r;
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f, g_root, &r));
    TEST_ASSERT_EQUAL_INT64(5, r.files_seen);      /* a.txt, b.md, sub/c.txt, empty.txt, bad.pdf */
    TEST_ASSERT_EQUAL_INT64(3, r.files_added);
    TEST_ASSERT_EQUAL_INT64(1, r.files_no_text);
    TEST_ASSERT_EQUAL_INT64(1, r.files_failed);
    TEST_ASSERT_EQUAL_INT64(1, r.files_skipped);    /* notes.zip; .hidden.txt not even seen */
    TEST_ASSERT_TRUE(r.chunks_added >= 3);
    TEST_ASSERT_EQUAL_INT64(r.chunks_added, lisa_store_count(s));

    lisa_store_doc_t rec;
    TEST_ASSERT_EQUAL_STRING("ok", status_of(s, "b.md", &rec));
    TEST_ASSERT_EQUAL_STRING("Leave Policy", rec.title);
    lisa_store_doc_free(&rec);
    TEST_ASSERT_EQUAL_STRING("no_text", status_of(s, "empty.txt", &rec));
    lisa_store_doc_free(&rec);
    TEST_ASSERT_EQUAL_STRING("error", status_of(s, "bad.pdf", &rec));
    TEST_ASSERT_TRUE(strlen(rec.message) > 0);
    lisa_store_doc_free(&rec);
    TEST_ASSERT_NULL(status_of(s, ".hidden.txt", &rec));
    lisa_store_close(s);
}

static void test_rerun_unchanged_does_nothing(void) {
    make_folder();
    lisa_store_t* s = open_store();
    fake_t f = { 0, 0 };
    ingest_progress_t r;
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f, g_root, &r));
    int64_t count = lisa_store_count(s);

    fake_t f2 = { 0, 0 };
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f2, g_root, &r));
    TEST_ASSERT_EQUAL_INT64(0, f2.calls);             /* nothing embedded */
    /* A file recorded as "error" is read again on every run (the cause is
     * usually temporary), so only the four readable ones count unchanged. */
    TEST_ASSERT_EQUAL_INT64(4, r.files_unchanged);
    TEST_ASSERT_EQUAL_INT64(0, r.files_added + r.files_updated + r.files_removed);
    TEST_ASSERT_EQUAL_INT64(0, r.chunks_added + r.chunks_removed);
    TEST_ASSERT_EQUAL_INT64(count, lisa_store_count(s));

    /* Touched but identical content: record updated, still nothing embedded.
     * The file that failed before is read again, so four count unchanged. */
    set_mtime("a.txt", 1700000000);
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f2, g_root, &r));
    TEST_ASSERT_EQUAL_INT64(0, f2.calls);
    TEST_ASSERT_EQUAL_INT64(4, r.files_unchanged);
    lisa_store_doc_t rec;
    TEST_ASSERT_EQUAL_STRING("ok", status_of(s, "a.txt", &rec));
    TEST_ASSERT_EQUAL_INT64(1700000000LL * 1000000000LL, rec.mtime_ns);
    lisa_store_doc_free(&rec);
    lisa_store_close(s);
}

static void test_edit_and_delete(void) {
    make_folder();
    lisa_store_t* s = open_store();
    fake_t f = { 0, 0 };
    ingest_progress_t r;
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f, g_root, &r));
    lisa_store_doc_t rec;
    status_of(s, "b.md", &rec);
    int64_t old_b = rec.chunk_count;
    lisa_store_doc_free(&rec);
    status_of(s, "sub/c.txt", &rec);
    int64_t old_c = rec.chunk_count;
    lisa_store_doc_free(&rec);
    int64_t before = lisa_store_count(s);

    /* Edit b.md, delete sub/c.txt. */
    write_text("b.md", "# Leave Policy v2\n\nEmployees now get 25 days of annual leave.\n\n"
                       "Unused leave carries over for one year.");
    set_mtime("b.md", 1800000000);
    remove_rel("sub/c.txt");

    fake_t f2 = { 0, 0 };
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f2, g_root, &r));
    TEST_ASSERT_EQUAL_INT64(1, r.files_updated);
    TEST_ASSERT_EQUAL_INT64(1, r.files_removed);
    TEST_ASSERT_TRUE(f2.calls >= 1);
    TEST_ASSERT_EQUAL_INT64(old_b + old_c, r.chunks_removed);
    TEST_ASSERT_EQUAL_INT64(before - r.chunks_removed + r.chunks_added, lisa_store_count(s));

    TEST_ASSERT_EQUAL_STRING("ok", status_of(s, "b.md", &rec));
    TEST_ASSERT_EQUAL_STRING("Leave Policy v2", rec.title);
    lisa_store_doc_free(&rec);
    TEST_ASSERT_NULL(status_of(s, "sub/c.txt", &rec));   /* file gone: record gone */

    /* The new text is what is stored. */
    lisa_store_view_t v;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_view(s, &v));
    int found = 0;
    for (int64_t i = 0; i < v.n_slots; i++) {
        if (!v.live[i]) continue;
        lisa_store_chunk_t c;
        TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_get(s, v.slot_ids[i], &c));
        if (strstr(c.text, "25 days")) found = 1;
        TEST_ASSERT_NULL(strstr(c.text, "20 days"));
        lisa_store_chunk_free(&c);
    }
    TEST_ASSERT_TRUE(found);
    lisa_store_close(s);
}

static int cancel_after_first(void* user, const ingest_progress_t* p) {
    (void)p;
    return ++*(int*)user < 1;
}

static void test_cancel_and_missing_path(void) {
    make_folder();
    lisa_store_t* s = open_store();
    fake_t f = { 0, 0 };
    ingest_params_t p;
    memset(&p, 0, sizeof(p));
    p.store = s;
    p.embed = fake_embed;
    p.embed_user = &f;
    doc_chunk_params_t cp = DOC_CHUNK_PARAMS_DEFAULT;
    p.chunk = cp;
    int calls = 0;
    p.progress = cancel_after_first;
    p.progress_user = &calls;
    const char* paths[1] = { g_root };
    ingest_progress_t r;
    TEST_ASSERT_EQUAL_INT(INGEST_ECANCELLED, ingest_run(&p, paths, 1, &r, NULL));
    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_EQUAL_INT64(1, r.files_seen);

    /* Missing path: nothing happens. */
    char missing[900];
    snprintf(missing, sizeof(missing), "%s/does-not-exist", g_root);
    int64_t before = lisa_store_count(s);
    const char* two[2] = { g_root, missing };
    p.progress = NULL;
    TEST_ASSERT_EQUAL_INT(INGEST_ENOTFOUND, ingest_run(&p, two, 2, &r, NULL));
    TEST_ASSERT_EQUAL_INT64(before, lisa_store_count(s));
    TEST_ASSERT_EQUAL_INT64(0, r.files_seen);
    TEST_ASSERT_EQUAL_INT(INGEST_EINVAL, ingest_run(&p, two, 0, &r, NULL));
    lisa_store_close(s);
}

static void test_embed_failure_marks_document(void) {
    write_text("good.txt", "Normal text about pumps.");
    write_text("poison.txt", "This chunk makes the embedder fail: FAILEMBED.");
    lisa_store_t* s = open_store();
    fake_t f = { 0, 0 };
    ingest_progress_t r;
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f, g_root, &r));
    TEST_ASSERT_EQUAL_INT64(1, r.files_added);
    TEST_ASSERT_EQUAL_INT64(1, r.files_failed);
    lisa_store_doc_t rec;
    TEST_ASSERT_EQUAL_STRING("error", status_of(s, "poison.txt", &rec));
    /* The message says what happened and that LISA will try again: an
     * embedding failure is usually temporary (a busy GPU). */
    TEST_ASSERT_NOT_NULL(strstr(rec.message, "will try again"));
    TEST_ASSERT_EQUAL_INT64(0, rec.chunk_count);
    lisa_store_doc_free(&rec);
    lisa_store_close(s);
}

static void test_sibling_folder_with_same_prefix_untouched(void) {
    /* g_root and g_root + "X" share a prefix: syncing one must not touch the other. */
    char sibling[900];
    snprintf(sibling, sizeof(sibling), "%sX", g_root);
    TEST_ASSERT_EQUAL_INT(LISA_PLAT_OK, lisa_mkdir(sibling));
    write_text("a.txt", "Folder one.");
    char sib_file[1000];
    snprintf(sib_file, sizeof(sib_file), "%s/b.txt", sibling);
    FILE* fh = fopen(sib_file, "wb");
    fputs("Folder two.", fh);
    fclose(fh);

    lisa_store_t* s = open_store();
    fake_t f = { 0, 0 };
    ingest_progress_t r;
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f, g_root, &r));
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f, sibling, &r));
    int64_t both = lisa_store_count(s);
    remove_rel("a.txt");
    write_text("new.txt", "Replacement file.");
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f, g_root, &r));
    TEST_ASSERT_EQUAL_INT64(1, r.files_removed);   /* only a.txt, not ../X/b.txt */
    TEST_ASSERT_EQUAL_INT64(both, lisa_store_count(s));

    /* A single file path also works. */
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f, sib_file, &r));
    TEST_ASSERT_EQUAL_INT64(1, r.files_unchanged);
    lisa_store_close(s);
}

static void test_search_finds_ingested_chunk(void) {
    make_folder();
    lisa_store_t* s = open_store();
    fake_t f = { 0, 0 };
    ingest_progress_t r;
    TEST_ASSERT_EQUAL_INT(INGEST_OK, run(s, &f, g_root, &r));

    /* The passage is embedded on its own, so its own text finds it. */
    float q[DIM];
    fake_vec("Leave Policy\n\nEmployees get 20 days of annual leave.", q, DIM);
    lisa_store_view_t v;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_view(s, &v));
    int idx[1];
    float d[1];
    lisa_result_t res = { idx, d, 1, 0 };
    TEST_ASSERT_EQUAL_INT(0, lisa_search_masked(q, v.vectors, v.n_slots, DIM, 1, v.live, &res));
    lisa_store_chunk_t c;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_get(s, v.slot_ids[idx[0]], &c));
    TEST_ASSERT_NOT_NULL(strstr(c.source_path, "b.md"));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, d[0]);
    lisa_store_chunk_free(&c);
    lisa_store_close(s);
}

/* ---- with the real embedding model ------------------------------------ */

static int real_embed(void* user, const char* const* texts, int64_t n, float* out, int64_t dim) {
    return lm_embed((lm_model_t*)user, LM_EMBED_DOCUMENT, texts, n, out, dim) == LM_OK ? 0 : 1;
}

static void test_real_model_end_to_end(void) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/Qwen3-Embedding-0.6B-Q8_0.gguf", g_models);
    FILE* fh = fopen(path, "rb");
    if (fh == NULL) TEST_IGNORE_MESSAGE("embedding model not present");
    fclose(fh);

    lm_load_params_t lp = { -1, 0, 0, NULL, NULL };
    lm_model_t* m = NULL;
    TEST_ASSERT_EQUAL_INT(LM_OK, lm_load(path, &lp, &m));
    char coll[900];
    snprintf(coll, sizeof(coll), "%s.real", g_root);
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_create(coll, "qwen3-embedding-0.6b-q8_0", 1024));
    lisa_store_t* s = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_open(coll, LISA_STORE_WRITE, NULL, &s));

    make_folder();
    char pdf[1100];
    snprintf(pdf, sizeof(pdf), "cp '%s/two_pages.pdf' '%s/manual.pdf'", g_fixtures, g_root);
    TEST_ASSERT_EQUAL_INT(0, system(pdf));

    ingest_params_t p;
    memset(&p, 0, sizeof(p));
    p.store = s;
    p.embed = real_embed;
    p.embed_user = m;
    doc_chunk_params_t cp = DOC_CHUNK_PARAMS_DEFAULT;
    p.chunk = cp;
    const char* paths[1] = { g_root };
    ingest_progress_t r;
    TEST_ASSERT_EQUAL_INT(INGEST_OK, ingest_run(&p, paths, 1, &r, NULL));
    TEST_ASSERT_EQUAL_INT64(4, r.files_added);   /* a.txt, b.md, sub/c.txt, manual.pdf */

    /* A real question finds the right document; PDF chunks carry pages. */
    const char* questions[3] = { "How many days of annual leave do employees get?",
                                 "What does vibration above 40 Hz mean?",
                                 "आरबीआई ने रेपो दर के बारे में क्या किया?" };
    const char* expect[3] = { "b.md", "", "c.txt" };
    for (int qi = 0; qi < 3; qi++) {
        float q[1024];
        TEST_ASSERT_EQUAL_INT(LM_OK, lm_embed(m, LM_EMBED_QUERY, &questions[qi], 1, q, 1024));
        lisa_store_view_t v;
        TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_view(s, &v));
        int idx[1];
        float d[1];
        lisa_result_t res = { idx, d, 1, 0 };
        TEST_ASSERT_EQUAL_INT(0, lisa_search_masked(q, v.vectors, v.n_slots, 1024, 1, v.live, &res));
        lisa_store_chunk_t c;
        TEST_ASSERT_EQUAL_INT(LISA_STORE_OK, lisa_store_get(s, v.slot_ids[idx[0]], &c));
        if (qi == 1) {
            /* a.txt or the PDF both state this; the PDF chunk must say page 2. */
            int ok = strstr(c.source_path, "a.txt") != NULL ||
                     (strstr(c.source_path, "manual.pdf") != NULL && c.page >= 1);
            TEST_ASSERT_TRUE_MESSAGE(ok, c.source_path);
        } else {
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c.source_path, expect[qi]), questions[qi]);
        }
        lisa_store_chunk_free(&c);
    }
    lisa_store_close(s);
    lm_free(m);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <scratch_dir> <fixtures_dir> <models_dir>\n", argv[0]);
        return 2;
    }
    g_scratch = argv[1];
    g_fixtures = argv[2];
    g_models = argv[3];
    lisa_mkdir(g_scratch);
    UNITY_BEGIN();
    RUN_TEST(test_first_run_indexes_supported_files);
    RUN_TEST(test_rerun_unchanged_does_nothing);
    RUN_TEST(test_edit_and_delete);
    RUN_TEST(test_cancel_and_missing_path);
    RUN_TEST(test_embed_failure_marks_document);
    RUN_TEST(test_sibling_folder_with_same_prefix_untouched);
    RUN_TEST(test_search_finds_ingested_chunk);
    RUN_TEST(test_real_model_end_to_end);
    return UNITY_END() == 0 ? 0 : 1;
}
