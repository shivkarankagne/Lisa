/* SPDX-License-Identifier: Apache-2.0 */
/*
 * bench_models — W4 measurements for the default models.
 *
 * Usage: bench_models <gen.gguf> <embed.gguf> [gpu_layers]
 *
 * Reports: model load time, time to first token for a RAG-sized prompt
 * (~1500 tokens of context + question), generation speed, embedding
 * throughput for 512-token chunks, and peak resident memory.
 *
 * Numbers are for the record in a report, not for marketing (plan §10).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "lisa.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double peak_rss_mb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return ru.ru_maxrss / (1024.0 * 1024.0);   /* bytes */
#else
    return ru.ru_maxrss / 1024.0;              /* kilobytes */
#endif
}

typedef struct {
    double start, first;
    int    pieces;
} ttft_t;

static int on_token(void* user, const char* text, int64_t len) {
    (void)text; (void)len;
    ttft_t* t = (ttft_t*)user;
    if (t->pieces++ == 0) t->first = now_s();
    return 0;
}

/* ~6 chars/token: 9000 chars of prose is roughly 1500 tokens. */
static char* make_context(size_t chars) {
    static const char* s =
        "Section 4.2 of the maintenance manual states that pump bearings must be "
        "inspected every 500 operating hours. Vibration above 40 Hz indicates wear; "
        "operators should log the reading, stop the pump, and notify the shift lead. ";
    char* out = malloc(chars + 1);
    size_t n = strlen(s), len = 0;
    while (len + n < chars) {
        memcpy(out + len, s, n);
        len += n;
    }
    out[len] = '\0';
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <gen.gguf> <embed.gguf> [gpu_layers]\n", argv[0]);
        return 2;
    }
    int gpu = argc > 3 ? atoi(argv[3]) : -1;
    lisa_context_t* ctx = NULL;
    lisa_context_create(NULL, &ctx);

    lisa_model_options_t mo = LISA_MODEL_OPTIONS_INIT;
    mo.gpu_layers = gpu;

    /* ---- generation ---- */
    double t0 = now_s();
    lisa_model_t* gen = NULL;
    int rc = lisa_model_load(ctx, argv[1], &mo, &gen);
    double load_gen = now_s() - t0;
    if (rc != LISA_OK) {
        fprintf(stderr, "load %s: %s\n", argv[1], lisa_status_string(rc));
        return 1;
    }

    char* context = make_context(9000);
    size_t ql = strlen(context) + 256;
    char* user = malloc(ql);
    snprintf(user, ql, "Context:\n%s\n\nQuestion: What should an operator do when pump "
             "vibration exceeds 40 Hz? Answer in two sentences.", context);
    lisa_message_t msgs[2] = { { "system", "Answer only from the context." }, { "user", user } };

    lisa_generate_options_t go = LISA_GENERATE_OPTIONS_INIT;
    go.temperature = 0.0f;
    go.max_tokens = 128;
    go.on_token = on_token;
    ttft_t tt = { 0, 0, 0 };
    go.on_token_user = &tt;

    /* Warm-up run (first use of the context allocates buffers). */
    lisa_chat(gen, msgs, 2, &go, NULL, NULL);

    tt.pieces = 0;
    tt.start = now_s();
    int64_t ntok = 0;
    char* answer = NULL;
    rc = lisa_chat(gen, msgs, 2, &go, &answer, &ntok);
    double end = now_s();
    if (rc != LISA_OK) {
        fprintf(stderr, "chat: %s\n", lisa_status_string(rc));
        return 1;
    }
    double ttft = tt.first - tt.start;
    double gen_tps = ntok > 1 ? (ntok - 1) / (end - tt.first) : 0;

    /* ---- embeddings ---- */
    t0 = now_s();
    lisa_model_t* emb = NULL;
    rc = lisa_model_load(ctx, argv[2], &mo, &emb);
    double load_emb = now_s() - t0;
    if (rc != LISA_OK) {
        fprintf(stderr, "load %s: %s\n", argv[2], lisa_status_string(rc));
        return 1;
    }
    enum { N = 32 };
    char* chunk = make_context(3000);   /* ~500 tokens */
    const char* texts[N];
    for (int i = 0; i < N; i++) texts[i] = chunk;
    lisa_model_info_t info = LISA_MODEL_INFO_INIT;
    lisa_model_info(emb, &info);
    float* vecs = malloc((size_t)N * info.embedding_dim * sizeof(float));
    lisa_embed(emb, LISA_EMBED_DOCUMENT, texts, 1, vecs, 0);  /* warm-up */
    t0 = now_s();
    rc = lisa_embed(emb, LISA_EMBED_DOCUMENT, texts, N, vecs, 0);
    double emb_s = now_s() - t0;

    printf("gpu_layers            %d\n", gpu);
    printf("load generation model %.2f s\n", load_gen);
    printf("load embedding model  %.2f s\n", load_emb);
    printf("time to first token   %.2f s  (~1500-token RAG prompt)\n", ttft);
    printf("generation speed      %.1f tokens/s  (%lld tokens)\n", gen_tps, (long long)ntok);
    printf("embedding throughput  %.1f chunks/s  (~500-token chunks, %s)\n",
           rc == LISA_OK ? N / emb_s : 0.0, lisa_status_string(rc));
    printf("peak resident memory  %.0f MB\n", peak_rss_mb());
    printf("answer: %s\n", answer);

    lisa_free(ctx, answer);
    free(vecs); free(chunk); free(context); free(user);
    lisa_model_free(emb);
    lisa_model_free(gen);
    lisa_context_destroy(ctx);
    return 0;
}
