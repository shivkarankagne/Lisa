/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * test_ask — lisa_ask (W8) used as a program would: only lisa.h. Needs
 * both default models in <models_dir>; model tests are ignored without
 * them. One collection is ingested once and shared by the tests.
 *
 * Usage: test_ask <scratch_dir> <fixtures_dir> <models_dir>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unity.h"
#include "lisa.h"

static const char* g_scratch;
static const char* g_fixtures;
static int g_have_models;
static lisa_context_t* g_ctx;
static lisa_model_t* g_embed;
static lisa_model_t* g_chat;
static lisa_collection_t* g_coll;
static char g_dir[800];

void setUp(void) {}
void tearDown(void) {}

#define NEED_MODELS() do { if (!g_have_models) TEST_IGNORE_MESSAGE("models not present"); } while (0)

static void write_file(const char* name, const char* text) {
    char p[1000];
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE* f = fopen(p, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
}

typedef struct {
    char    text[4096];
    size_t  len;
    int     pieces;
    int     stop_after;   /* 0: never */
} stream_t;

static int on_token(void* user, const char* s, int64_t n) {
    stream_t* st = (stream_t*)user;
    if (st->len + (size_t)n < sizeof(st->text)) {
        memcpy(st->text + st->len, s, (size_t)n);
        st->len += (size_t)n;
        st->text[st->len] = '\0';
    }
    st->pieces++;
    return st->stop_after && st->pieces >= st->stop_after;
}

static int ask(const char* q, lisa_ask_options_t* o, lisa_answer_t** a) {
    lisa_message_t m = { "user", q };
    int rc = lisa_ask(g_coll, g_embed, g_chat, &m, 1, o, a);
    if (rc == LISA_OK) {
        printf("  Q: %s\n  A: %s\n     found=%d cites=%lld used=%lld/%lld prompt=%lld tokens=%lld "
               "first=%.2fs total=%.2fs\n", q, (*a)->text, (*a)->found,
               (long long)(*a)->citation_count, (long long)(*a)->passages_used,
               (long long)(*a)->passages_retrieved, (long long)(*a)->prompt_tokens,
               (long long)(*a)->answer_tokens, (*a)->first_token_seconds, (*a)->total_seconds);
    }
    return rc;
}

static int cites_file(const lisa_answer_t* a, const char* name) {
    for (int64_t i = 0; i < a->citation_count; i++) {
        if (strstr(a->citations[i].source_path, name)) return 1;
    }
    return 0;
}

static void test_answer_with_citation_and_stream(void) {
    NEED_MODELS();
    stream_t s;
    memset(&s, 0, sizeof(s));
    lisa_ask_options_t o = LISA_ASK_OPTIONS_INIT;
    o.on_token = on_token;
    o.on_token_user = &s;
    o.max_tokens = 96;
    lisa_answer_t* a = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, ask("Why did the pump fail?", &o, &a));
    TEST_ASSERT_TRUE(a->found);
    TEST_ASSERT_TRUE(a->complete);
    TEST_ASSERT_NOT_NULL(strstr(a->text, "bearing"));
    TEST_ASSERT_TRUE(a->citation_count >= 1);
    TEST_ASSERT_TRUE(cites_file(a, "pump.txt"));
    const lisa_citation_t* c = &a->citations[0];
    TEST_ASSERT_TRUE(c->number >= 1 && c->number <= a->passages_used);
    TEST_ASSERT_NOT_NULL(strstr(c->quote, "bearing"));
    TEST_ASSERT_EQUAL_INT64(0, c->page);
    TEST_ASSERT_TRUE(c->length > 0);
    TEST_ASSERT_TRUE(c->similarity >= 0.40f);
    TEST_ASSERT_TRUE(strlen(c->content_hash) > 0);
    TEST_ASSERT_TRUE(a->prompt_tokens > 0 && a->prompt_tokens <= 1100);
    TEST_ASSERT_TRUE(a->first_token_seconds > 0 && a->first_token_seconds <= a->total_seconds);
    /* The stream is exactly the answer. */
    TEST_ASSERT_EQUAL_STRING(a->text, s.text);
    TEST_ASSERT_TRUE(s.pieces > 1);
    lisa_answer_free(a);
}

static void test_pdf_page_and_hindi(void) {
    NEED_MODELS();
    lisa_ask_options_t o = LISA_ASK_OPTIONS_INIT;
    o.max_tokens = 96;
    lisa_answer_t* a = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, ask("How often must pump bearings be inspected?", &o, &a));
    TEST_ASSERT_TRUE(a->found);
    TEST_ASSERT_NOT_NULL(strstr(a->text, "500"));
    TEST_ASSERT_TRUE(cites_file(a, "two_pages.pdf"));
    for (int64_t i = 0; i < a->citation_count; i++) {
        if (strstr(a->citations[i].source_path, "two_pages.pdf")) {
            TEST_ASSERT_EQUAL_INT64(1, a->citations[i].page);
            TEST_ASSERT_EQUAL_STRING("LISA Test Manual", a->citations[i].title);
        }
    }
    lisa_answer_free(a);

    TEST_ASSERT_EQUAL_INT(LISA_OK, ask("\xe0\xa4\xb0\xe0\xa5\x87\xe0\xa4\xaa\xe0\xa5\x8b "
                                       "\xe0\xa4\xa6\xe0\xa4\xb0 \xe0\xa4\x95\xe0\xa4\xbf\xe0\xa4\xa4"
                                       "\xe0\xa4\xa8\xe0\xa5\x80 \xe0\xa4\xb9\xe0\xa5\x88?", &o, &a));
    TEST_ASSERT_TRUE(a->found);
    TEST_ASSERT_NOT_NULL(strstr(a->text, "6.5"));
    TEST_ASSERT_TRUE(cites_file(a, "rbi.txt"));
    lisa_answer_free(a);
}

static void test_not_found(void) {
    NEED_MODELS();
    stream_t s;
    memset(&s, 0, sizeof(s));
    lisa_ask_options_t o = LISA_ASK_OPTIONS_INIT;
    o.on_token = on_token;
    o.on_token_user = &s;
    o.max_tokens = 64;
    lisa_answer_t* a = NULL;

    /* Off-topic with the default floor: whether or not a passage reaches the
     * model, the answer is "not found" with no citations. */
    TEST_ASSERT_EQUAL_INT(LISA_OK, ask("Who won the cricket world cup in 2011?", &o, &a));
    TEST_ASSERT_FALSE(a->found);
    TEST_ASSERT_EQUAL_INT64(0, a->citation_count);
    lisa_answer_free(a);

    /* Off-topic under a strict floor: nothing passes; the model is not run. */
    memset(&s, 0, sizeof(s));
    o.min_similarity = 0.40f;
    TEST_ASSERT_EQUAL_INT(LISA_OK, ask("Who won the cricket world cup in 2011?", &o, &a));
    TEST_ASSERT_FALSE(a->found);
    TEST_ASSERT_EQUAL_STRING(LISA_NOT_FOUND_TEXT, a->text);
    TEST_ASSERT_EQUAL_STRING(LISA_NOT_FOUND_TEXT, s.text);
    TEST_ASSERT_EQUAL_INT64(0, a->citation_count);
    TEST_ASSERT_NULL(a->citations);
    TEST_ASSERT_EQUAL_INT64(0, a->passages_used);
    TEST_ASSERT_EQUAL_INT64(0, a->answer_tokens);
    TEST_ASSERT_TRUE(a->passages_retrieved > 0);
    lisa_answer_free(a);

    o = (lisa_ask_options_t)LISA_ASK_OPTIONS_INIT;
    o.max_tokens = 64;
    /*
     * A fact the model is sure of, with passages that pass the floor: it
     * must still decline rather than answer from its own knowledge and
     * hang a citation on an unrelated passage.
     */
    TEST_ASSERT_EQUAL_INT(LISA_OK, ask("What is the capital of France?", &o, &a));
    TEST_ASSERT_FALSE(a->found);
    TEST_ASSERT_EQUAL_INT64(0, a->citation_count);
    lisa_answer_free(a);

    /* On topic, but the fact is missing: the model declines. */
    TEST_ASSERT_EQUAL_INT(LISA_OK, ask("What is our parental leave policy?", &o, &a));
    TEST_ASSERT_TRUE(a->passages_used > 0);
    TEST_ASSERT_FALSE(a->found);
    TEST_ASSERT_EQUAL_INT64(0, a->citation_count);
    lisa_answer_free(a);
}

static void test_budget_and_stop(void) {
    NEED_MODELS();
    lisa_ask_options_t o = LISA_ASK_OPTIONS_INIT;
    o.prompt_budget = 64;                    /* system prompt + question do not fit */
    lisa_answer_t* a = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_E_TOO_LONG, ask("Why did the pump fail?", &o, &a));
    TEST_ASSERT_NULL(a);

    /* The on_token callback can stop the answer early. */
    stream_t s;
    memset(&s, 0, sizeof(s));
    s.stop_after = 2;
    o = (lisa_ask_options_t)LISA_ASK_OPTIONS_INIT;
    o.on_token = on_token;
    o.on_token_user = &s;
    TEST_ASSERT_EQUAL_INT(LISA_OK, ask("How many days of annual leave do employees get?", &o, &a));
    TEST_ASSERT_FALSE(a->complete);
    TEST_ASSERT_EQUAL_INT(2, s.pieces);
    lisa_answer_free(a);
}

static void test_invalid_arguments(void) {
    NEED_MODELS();
    lisa_answer_t* a = (lisa_answer_t*)1;
    lisa_message_t two[2] = { { "user", "hello" }, { "user", "Why did the pump fail?" } };
    lisa_message_t asst = { "assistant", "Why?" };
    lisa_message_t empty = { "user", "" };
    TEST_ASSERT_EQUAL_INT(LISA_E_UNSUPPORTED, lisa_ask(g_coll, g_embed, g_chat, two, 2, NULL, &a));
    TEST_ASSERT_NULL(a);
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_ask(g_coll, g_embed, g_chat, &asst, 1, NULL, &a));
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_ask(g_coll, g_embed, g_chat, &empty, 1, NULL, &a));
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_ask(g_coll, g_embed, g_chat, two, 0, NULL, &a));
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_ask(NULL, g_embed, g_chat, &two[1], 1, NULL, &a));
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_ask(g_coll, g_embed, g_chat, &two[1], 1, NULL, NULL));
    lisa_ask_options_t o = LISA_ASK_OPTIONS_INIT;
    o.top_k = 0;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_ask(g_coll, g_embed, g_chat, &two[1], 1, &o, &a));
    o = (lisa_ask_options_t)LISA_ASK_OPTIONS_INIT;
    o.min_similarity = 2.0f;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_ask(g_coll, g_embed, g_chat, &two[1], 1, &o, &a));
    /* The chat model is not the collection's embedding model. */
    TEST_ASSERT_EQUAL_INT(LISA_E_MODEL_MISMATCH, lisa_ask(g_coll, g_chat, g_chat, &two[1], 1, NULL, &a));
    lisa_answer_free(NULL);
}

static int setup_collection(void) {
    snprintf(g_dir, sizeof(g_dir), "%s/ask_%d", g_scratch, (int)getpid());
    mkdir(g_dir, 0755);
    write_file("pump.txt", "Pump P-7 failed on Tuesday: the main bearing seized after weeks of rising "
                           "vibration. A replacement bearing was ordered from the supplier.");
    write_file("leave.md", "# Leave policy\n\nEmployees are entitled to 24 days of paid annual leave "
                           "per calendar year. Up to 10 unused days may be carried forward with "
                           "manager approval.");
    write_file("rbi.txt", "\xe0\xa4\xad\xe0\xa4\xbe\xe0\xa4\xb0\xe0\xa4\xa4\xe0\xa5\x80\xe0\xa4\xaf "
                          "\xe0\xa4\xb0\xe0\xa4\xbf\xe0\xa4\x9c\xe0\xa4\xbc\xe0\xa4\xb0\xe0\xa5\x8d\xe0\xa4\xb5 "
                          "\xe0\xa4\xac\xe0\xa5\x88\xe0\xa4\x82\xe0\xa4\x95 \xe0\xa4\xa8\xe0\xa5\x87 "
                          "\xe0\xa4\xb0\xe0\xa5\x87\xe0\xa4\xaa\xe0\xa5\x8b \xe0\xa4\xa6\xe0\xa4\xb0 "
                          "\xe0\xa4\x95\xe0\xa5\x8b 6.5 \xe0\xa4\xaa\xe0\xa5\x8d\xe0\xa4\xb0\xe0\xa4\xa4"
                          "\xe0\xa4\xbf\xe0\xa4\xb6\xe0\xa4\xa4 \xe0\xa4\x95\xe0\xa4\xb0 \xe0\xa4\xa6"
                          "\xe0\xa4\xbf\xe0\xa4\xaf\xe0\xa4\xbe \xe0\xa4\xb9\xe0\xa5\x88\xe0\xa5\xa4");
    char pdf_src[1000], pdf_dst[1000];
    snprintf(pdf_src, sizeof(pdf_src), "%s/two_pages.pdf", g_fixtures);
    snprintf(pdf_dst, sizeof(pdf_dst), "%s/two_pages.pdf", g_dir);
    FILE* in = fopen(pdf_src, "rb");
    FILE* out = fopen(pdf_dst, "wb");
    if (!in || !out) return 1;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);

    char coll[900];
    snprintf(coll, sizeof(coll), "%s.coll", g_dir);
    if (lisa_collection_create_for_model(g_ctx, coll, g_embed, 0) != LISA_OK) return 1;
    if (lisa_collection_open(g_ctx, coll, LISA_OPEN_WRITE, NULL, &g_coll) != LISA_OK) return 1;
    const char* paths[1] = { g_dir };
    lisa_ingest_status_t st = LISA_INGEST_STATUS_INIT;
    return lisa_ingest(g_coll, g_embed, paths, 1, NULL, &st) != LISA_OK;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <scratch_dir> <fixtures_dir> <models_dir>\n", argv[0]);
        return 2;
    }
    g_scratch = argv[1];
    g_fixtures = argv[2];
    mkdir(g_scratch, 0755);
    char embed_path[1024], chat_path[1024];
    snprintf(embed_path, sizeof(embed_path), "%s/Qwen3-Embedding-0.6B-Q8_0.gguf", argv[3]);
    snprintf(chat_path, sizeof(chat_path), "%s/Qwen3-4B-Q4_K_M.gguf", argv[3]);
    g_have_models = access(embed_path, R_OK) == 0 && access(chat_path, R_OK) == 0;

    if (lisa_context_create(NULL, &g_ctx) != LISA_OK) return 1;
    int fail = 0;
    if (g_have_models) {
        fail = lisa_model_load(g_ctx, embed_path, NULL, &g_embed) != LISA_OK ||
               lisa_model_load(g_ctx, chat_path, NULL, &g_chat) != LISA_OK ||
               setup_collection() != 0;
    }
    int failures = 1;
    if (!fail) {
        UNITY_BEGIN();
        RUN_TEST(test_answer_with_citation_and_stream);
        RUN_TEST(test_pdf_page_and_hindi);
        RUN_TEST(test_not_found);
        RUN_TEST(test_budget_and_stop);
        RUN_TEST(test_invalid_arguments);
        failures = UNITY_END();
    } else {
        fprintf(stderr, "setup failed\n");
    }
    lisa_collection_close(g_coll);
    lisa_model_free(g_chat);    /* llama.cpp aborts at exit if a model is still loaded */
    lisa_model_free(g_embed);
    lisa_context_destroy(g_ctx);
    return failures == 0 ? 0 : 1;
}
