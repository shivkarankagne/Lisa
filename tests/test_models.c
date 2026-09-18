/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_models — public model API (W4).
 *
 * Usage: test_models <scratch_dir> <models_dir>
 *
 * Tests that need no model file always run. Tests that need the default
 * models (Qwen3-4B-Q4_K_M.gguf, Qwen3-Embedding-0.6B-Q8_0.gguf in
 * <models_dir>) are ignored with a message when the files are absent;
 * CI provides them.
 *
 * The greedy-decoding reference check drives llama.cpp directly (not
 * through LISA) and requires identical output.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "lisa.h"
#include "llama.h"

static const char* g_scratch;
static char g_gen_path[1024];
static char g_emb_path[1024];
static int g_have_gen, g_have_emb;
static lisa_context_t* g_ctx;

void setUp(void) {}
void tearDown(void) {}

static int file_exists(const char* p) {
    FILE* f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

#define NEED_GEN() do { if (!g_have_gen) TEST_IGNORE_MESSAGE("generation model not present"); } while (0)
#define NEED_EMB() do { if (!g_have_emb) TEST_IGNORE_MESSAGE("embedding model not present"); } while (0)

/* ---- no model needed -------------------------------------------------- */

static void test_known_models_table(void) {
    TEST_ASSERT_EQUAL_INT64(2, lisa_known_model_count());
    int gen = 0, emb = 0;
    for (int64_t i = 0; i < lisa_known_model_count(); i++) {
        lisa_known_model_t k;
        TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_known_model(i, &k));
        TEST_ASSERT_NOT_NULL(k.id);
        TEST_ASSERT_EQUAL_size_t(64, strlen(k.sha256));
        TEST_ASSERT_EQUAL_STRING("Apache-2.0", k.license);
        TEST_ASSERT_TRUE(k.file_size > 0);
        if (k.is_embedding) emb++; else gen++;
    }
    TEST_ASSERT_EQUAL_INT(1, gen);
    TEST_ASSERT_EQUAL_INT(1, emb);
    lisa_known_model_t k;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_known_model(2, &k));
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_known_model(-1, &k));
}

static void test_load_errors(void) {
    char path[1200];
    lisa_model_t* m = (lisa_model_t*)1;
    snprintf(path, sizeof(path), "%s/no-such-model.gguf", g_scratch);
    TEST_ASSERT_EQUAL_INT(LISA_E_NOT_FOUND, lisa_model_load(g_ctx, path, NULL, &m));
    TEST_ASSERT_NULL(m);

    snprintf(path, sizeof(path), "%s/not-a-model.gguf", g_scratch);
    FILE* f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs("this is not a GGUF file", f);
    fclose(f);
    TEST_ASSERT_EQUAL_INT(LISA_E_FORMAT, lisa_model_load(g_ctx, path, NULL, &m));
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_model_load(NULL, path, NULL, &m));
    lisa_model_free(NULL);
}

static void test_verify_hash(void) {
    char path[1200], hex[65];
    snprintf(path, sizeof(path), "%s/abc.bin", g_scratch);
    FILE* f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs("abc", f);
    fclose(f);
    /* FIPS 180-2 test vector for "abc". */
    TEST_ASSERT_EQUAL_INT(LISA_E_UNSUPPORTED, lisa_model_verify(path, NULL, hex));
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex);

    snprintf(path, sizeof(path), "%s/missing.bin", g_scratch);
    TEST_ASSERT_EQUAL_INT(LISA_E_NOT_FOUND, lisa_model_verify(path, NULL, NULL));
}

/* ---- generation model ------------------------------------------------- */

static void test_verify_downloaded_models(void) {
    if (!g_have_gen && !g_have_emb) TEST_IGNORE_MESSAGE("no model files present");
    lisa_known_model_t k;
    if (g_have_gen) {
        TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_model_verify(g_gen_path, &k, NULL));
        TEST_ASSERT_EQUAL_STRING("qwen3-4b-q4_k_m", k.id);
    }
    if (g_have_emb) {
        TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_model_verify(g_emb_path, &k, NULL));
        TEST_ASSERT_EQUAL_STRING("qwen3-embedding-0.6b-q8_0", k.id);
    }
}

static int cancel_at_half(void* user, float f) {
    (void)user;
    return f < 0.5f;
}

static void test_load_can_be_cancelled(void) {
    NEED_GEN();
    lisa_model_options_t o = LISA_MODEL_OPTIONS_INIT;
    o.progress = cancel_at_half;
    lisa_model_t* m = (lisa_model_t*)1;
    TEST_ASSERT_EQUAL_INT(LISA_E_CANCELLED, lisa_model_load(g_ctx, g_gen_path, &o, &m));
    TEST_ASSERT_NULL(m);
}

/*
 * Reference greedy decoding written directly against llama.cpp, with the
 * same model and context settings LISA uses.
 */
static char* reference_greedy(const char* path, const char* prompt, int n_new) {
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 999;
    struct llama_model* model = llama_model_load_from_file(path, mp);
    TEST_ASSERT_NOT_NULL(model);
    const struct llama_vocab* vocab = llama_model_get_vocab(model);
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4096;
    cp.n_batch = 512;
    cp.n_ubatch = 512;
    cp.n_seq_max = 1;
    struct llama_context* ctx = llama_init_from_model(model, cp);
    TEST_ASSERT_NOT_NULL(ctx);

    llama_token toks[512];
    int n = llama_tokenize(vocab, prompt, (int)strlen(prompt), toks, 512, true, true);
    TEST_ASSERT_TRUE(n > 0 && n < 512);
    TEST_ASSERT_EQUAL_INT(0, llama_decode(ctx, llama_batch_get_one(toks, n)));

    struct llama_sampler* s = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(s, llama_sampler_init_greedy());
    char* out = calloc(1, 64 * 1024);
    size_t len = 0;
    for (int i = 0; i < n_new; i++) {
        llama_token t = llama_sampler_sample(s, ctx, -1);
        if (llama_vocab_is_eog(vocab, t)) break;
        char piece[256];
        int k = llama_token_to_piece(vocab, t, piece, sizeof(piece), 0, false);
        memcpy(out + len, piece, (size_t)k);
        len += (size_t)k;
        TEST_ASSERT_EQUAL_INT(0, llama_decode(ctx, llama_batch_get_one(&t, 1)));
    }
    llama_sampler_free(s);
    llama_free(ctx);
    llama_model_free(model);
    return out;
}

typedef struct {
    char   text[64 * 1024];
    size_t len;
    int    calls;
    int    stop_after;
} stream_t;

static int on_token(void* user, const char* text, int64_t len) {
    stream_t* s = (stream_t*)user;
    memcpy(s->text + s->len, text, (size_t)len);
    s->len += (size_t)len;
    s->text[s->len] = '\0';
    s->calls++;
    return s->stop_after > 0 && s->calls >= s->stop_after;
}

static void test_generation(void) {
    NEED_GEN();
    lisa_model_t* m = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_model_load(g_ctx, g_gen_path, NULL, &m));

    lisa_model_info_t info = LISA_MODEL_INFO_INIT;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_model_info(m, &info));
    TEST_ASSERT_EQUAL_STRING("qwen3", info.architecture);
    TEST_ASSERT_EQUAL_STRING("qwen3-4b-q4_k_m", info.profile);
    TEST_ASSERT_EQUAL_INT(0, info.is_embedding);
    TEST_ASSERT_EQUAL_INT64(4096, info.context_tokens);

    /* Chat, deterministic. */
    lisa_message_t msgs[2] = {
        { "system", "You are a concise assistant." },
        { "user", "What is the capital of France? Answer with one word." },
    };
    lisa_generate_options_t o = LISA_GENERATE_OPTIONS_INIT;
    o.temperature = 0.0f;
    o.max_tokens = 16;
    char *a = NULL, *b = NULL;
    int64_t na = 0, nb = 0;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_chat(m, msgs, 2, &o, &a, &na));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_chat(m, msgs, 2, &o, &b, &nb));
    TEST_ASSERT_NOT_NULL(strstr(a, "Paris"));
    TEST_ASSERT_NULL(strstr(a, "<think>"));
    TEST_ASSERT_EQUAL_STRING(a, b);
    TEST_ASSERT_EQUAL_INT64(na, nb);
    TEST_ASSERT_TRUE(na > 0 && na <= 16);
    lisa_free(g_ctx, a);
    lisa_free(g_ctx, b);

    /* Greedy output identical to driving llama.cpp directly. */
    const char* prompt = "The three primary colours are";
    o.max_tokens = 24;
    char* mine = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_generate(m, prompt, &o, &mine, NULL));
    char* ref = reference_greedy(g_gen_path, prompt, 24);
    TEST_ASSERT_EQUAL_STRING(ref, mine);
    free(ref);
    lisa_free(g_ctx, mine);

    /* Streaming: pieces concatenate to the full text; early stop works. */
    static stream_t st;
    memset(&st, 0, sizeof(st));
    o.on_token = on_token;
    o.on_token_user = &st;
    char* full = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_generate(m, prompt, &o, &full, NULL));
    TEST_ASSERT_EQUAL_STRING(full, st.text);
    TEST_ASSERT_TRUE(st.calls > 1);
    lisa_free(g_ctx, full);

    memset(&st, 0, sizeof(st));
    st.stop_after = 2;
    int64_t produced = 0;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_generate(m, prompt, &o, NULL, &produced));
    TEST_ASSERT_EQUAL_INT(2, st.calls);
    TEST_ASSERT_TRUE(produced < 24);

    /* Sampled output with a seed is reproducible. */
    lisa_generate_options_t s = LISA_GENERATE_OPTIONS_INIT;
    s.max_tokens = 12;
    s.seed = 1234;
    char *s1 = NULL, *s2 = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_chat(m, msgs, 2, &s, &s1, NULL));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_chat(m, msgs, 2, &s, &s2, NULL));
    TEST_ASSERT_EQUAL_STRING(s1, s2);
    lisa_free(g_ctx, s1);
    lisa_free(g_ctx, s2);

    /* Wrong kind and bad options. */
    const char* t[1] = { "x" };
    float v[4];
    TEST_ASSERT_EQUAL_INT(LISA_E_WRONG_MODEL_KIND, lisa_embed(m, LISA_EMBED_QUERY, t, 1, v, 0));
    o.max_tokens = 0;
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_generate(m, prompt, &o, NULL, NULL));
    lisa_model_free(m);
}

static void test_context_limit_and_cpu_path(void) {
    NEED_GEN();
    lisa_model_options_t o = LISA_MODEL_OPTIONS_INIT;
    o.gpu_layers = 0;          /* CPU only */
    o.context_tokens = 256;
    lisa_model_t* m = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_model_load(g_ctx, g_gen_path, &o, &m));
    lisa_model_info_t info = LISA_MODEL_INFO_INIT;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_model_info(m, &info));
    TEST_ASSERT_EQUAL_INT(0, info.gpu);
    TEST_ASSERT_EQUAL_INT64(256, info.context_tokens);

    static char big[8000];
    memset(big, 0, sizeof(big));
    for (int i = 0; i < 700; i++) strcat(big, "word ");
    TEST_ASSERT_EQUAL_INT(LISA_E_TOO_LONG, lisa_generate(m, big, NULL, NULL, NULL));

    lisa_message_t msg = { "user", "What is 2 + 2? Answer with a number only." };
    lisa_generate_options_t g = LISA_GENERATE_OPTIONS_INIT;
    g.temperature = 0.0f;
    g.max_tokens = 8;
    char* out = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_chat(m, &msg, 1, &g, &out, NULL));
    TEST_ASSERT_NOT_NULL(strstr(out, "4"));
    lisa_free(g_ctx, out);
    lisa_model_free(m);
}

/* ---- embedding model -------------------------------------------------- */

static float dot(const float* a, const float* b, int64_t n) {
    double s = 0;
    for (int64_t i = 0; i < n; i++) s += (double)a[i] * b[i];
    return (float)s;
}

static void test_embeddings(void) {
    NEED_EMB();
    lisa_model_t* m = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_model_load(g_ctx, g_emb_path, NULL, &m));
    lisa_model_info_t info = LISA_MODEL_INFO_INIT;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_model_info(m, &info));
    TEST_ASSERT_EQUAL_INT(1, info.is_embedding);
    TEST_ASSERT_EQUAL_STRING("qwen3-embedding-0.6b-q8_0", info.profile);
    int64_t D = info.embedding_dim;
    TEST_ASSERT_EQUAL_INT64(1024, D);

    const char* docs[4] = {
        "The cat is sleeping on the warm windowsill.",
        "Quarterly revenue fell because of weak demand in Europe.",
        "Replace the pump bearing if vibration exceeds 40 Hz.",
        "The Reserve Bank of India raised the repo rate by 25 basis points.",
    };
    float* dv = malloc((size_t)(4 * D) * sizeof(float));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_embed(m, LISA_EMBED_DOCUMENT, docs, 4, dv, 0));
    for (int i = 0; i < 4; i++) TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, dot(dv + i * D, dv + i * D, D));

    /* English and Hindi queries each find the right document. */
    const char* queries[3] = {
        "Where is the cat?",
        "What should I do about pump vibration?",
        "आरबीआई ने ब्याज दर के बारे में क्या फैसला किया?",  /* What did the RBI decide about interest rates? */
    };
    const int expect[3] = { 0, 2, 3 };
    float* qv = malloc((size_t)(3 * D) * sizeof(float));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_embed(m, LISA_EMBED_QUERY, queries, 3, qv, 0));
    for (int q = 0; q < 3; q++) {
        int best = -1;
        float best_s = -2;
        for (int d = 0; d < 4; d++) {
            float s = dot(qv + q * D, dv + d * D, D);
            if (s > best_s) { best_s = s; best = d; }
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(expect[q], best, queries[q]);
    }

    /* Deterministic, and query/document forms differ (instruction prefix). */
    float* again = malloc((size_t)D * sizeof(float));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_embed(m, LISA_EMBED_DOCUMENT, docs, 1, again, 0));
    TEST_ASSERT_EQUAL_MEMORY(dv, again, (size_t)D * sizeof(float));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_embed(m, LISA_EMBED_QUERY, docs, 1, again, 0));
    TEST_ASSERT_TRUE(dot(dv, again, D) < 0.9999f);

    /* Truncated (Matryoshka) embeddings: renormalised, still rank correctly. */
    float d256[4 * 256], q256[256];
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_embed(m, LISA_EMBED_DOCUMENT, docs, 4, d256, 256));
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_embed(m, LISA_EMBED_QUERY, queries, 1, q256, 256));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, dot(d256, d256, 256));
    int best = 0;
    for (int d = 1; d < 4; d++)
        if (dot(q256, d256 + d * 256, 256) > dot(q256, d256 + best * 256, 256)) best = d;
    TEST_ASSERT_EQUAL_INT(0, best);
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, lisa_embed(m, LISA_EMBED_QUERY, queries, 1, qv, D + 1));

    /* Wrong kind. */
    lisa_message_t msg = { "user", "hi" };
    TEST_ASSERT_EQUAL_INT(LISA_E_WRONG_MODEL_KIND, lisa_chat(m, &msg, 1, NULL, NULL, NULL));

    free(dv);
    free(qv);
    free(again);
    lisa_model_free(m);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <scratch_dir> <models_dir>\n", argv[0]);
        return 2;
    }
    g_scratch = argv[1];
    snprintf(g_gen_path, sizeof(g_gen_path), "%s/Qwen3-4B-Q4_K_M.gguf", argv[2]);
    snprintf(g_emb_path, sizeof(g_emb_path), "%s/Qwen3-Embedding-0.6B-Q8_0.gguf", argv[2]);
    g_have_gen = file_exists(g_gen_path);
    g_have_emb = file_exists(g_emb_path);
    if (lisa_context_create(NULL, &g_ctx) != LISA_OK) return 1;

    UNITY_BEGIN();
    RUN_TEST(test_known_models_table);
    RUN_TEST(test_load_errors);
    RUN_TEST(test_verify_hash);
    RUN_TEST(test_verify_downloaded_models);
    RUN_TEST(test_load_can_be_cancelled);
    RUN_TEST(test_generation);
    RUN_TEST(test_context_limit_and_cpu_path);
    RUN_TEST(test_embeddings);
    int failures = UNITY_END();
    lisa_context_destroy(g_ctx);
    return failures == 0 ? 0 : 1;
}
