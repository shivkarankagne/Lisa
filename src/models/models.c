/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * LISA models — llama.cpp implementation of models.h.
 */

#include "models.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llama.h"
#include "hash/sha256/sha256.h"
#include "../platform/platform.h"

#define DEFAULT_CONTEXT   4096
/*
 * Passages embedded in one llama_decode call, and the context each one
 * gets. llama.cpp splits the context between sequences when the context
 * is created, so the slot size is fixed then: a passage that exceeds it
 * cannot be embedded at all, not even on its own ("find_slot: n_tokens =
 * 671 > size = 512"), and its file is recorded as failed. The slot is
 * therefore sized above the chunker's hard limit of 1,500 characters,
 * which is about 2,000 tokens in the densest scripts, and the context is
 * the slot times the number of sequences.
 *
 * More sequences cost key/value cache: this model spends about 112 KB
 * per token of context, so every extra sequence is another 230 MB.
 */
#define EMBED_SLOT_TOKENS 2048
#define EMBED_MAX_SEQ     1

#define PROMPT_BATCH      512
#define PENALTY_LAST_N    64

/* ==== Known models ==================================================== */

/*
 * Known models. The order matters: when no model is configured, the
 * program takes the first entry of each kind that is present on disk,
 * so the preferred default comes first.
 */
static const lm_profile_t k_profiles[] = {
    {
        "qwen3-4b-q4_k_m", "Qwen3-4B-Q4_K_M.gguf", 2497280256LL,
        "7485fe6f11af29433bc51cab58009521f205840f5b4ae3a32fa7f92e8534fdf5",
        "Apache-2.0",
        "https://huggingface.co/Qwen/Qwen3-4B-GGUF (rev bc640142c66e1fdd12af0bd68f40445458f3869b)",
        0,
        /* Non-thinking mode: an empty think block, as the Qwen3 template
         * emits for enable_thinking=False. Sampling per the model card. */
        "<think>\n\n</think>\n\n", 0.7f, 0.8f, 20, 1.5f,
        NULL, NULL, 0, 0,
    },
    {
        "e5-small-v2-q8_0", "e5-small-v2-q8_0.gguf", 36685088LL,
        "afdfb5c342d2efc2a051c426dd1d00913495d5f2bbceaea100d2f3892aa31cbc",
        "MIT",
        "https://huggingface.co/ggml-org/e5-small-v2-Q8_0-GGUF",
        1,
        NULL, 0, 0, 0, 0,
        /* E5 was trained with these exact prefixes; without them the
         * vectors are noticeably worse. Mean pooling, so no EOS. */
        "query: ", "passage: ", 0, 0,
    },
    {
        "qwen3-embedding-0.6b-q8_0", "Qwen3-Embedding-0.6B-Q8_0.gguf", 639150592LL,
        "06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439",
        "Apache-2.0",
        "https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF (rev 370f27d7550e0def9b39c1f16d3fbaa13aa67728)",
        1,
        NULL, 0, 0, 0, 0,
        /* Query instruction per the model card; documents are embedded as-is. */
        "Instruct: Given a question, retrieve passages that answer the question\nQuery:",
        "", 1, 1,
    },
    { NULL, NULL, 0, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL, NULL, 0, 0 },
};

static const lm_profile_t k_generic_gen = {
    "generic", NULL, 0, NULL, NULL, NULL, 0, "", 0.7f, 0.9f, 40, 0.0f, NULL, NULL, 0, 0,
};
static const lm_profile_t k_generic_embed = {
    "generic", NULL, 0, NULL, NULL, NULL, 1, NULL, 0, 0, 0, 0, "", "", 0, 0,
};

const lm_profile_t* lm_known_models(void) {
    return k_profiles;
}

/* ==== Runtime setup =================================================== */

static void quiet_log(enum ggml_log_level level, const char* text, void* user) {
    (void)user;
    static int verbose = -1;
    if (verbose < 0) verbose = getenv("LISA_LLAMA_LOG") != NULL;
    if (verbose || level == GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
}

static atomic_int g_backend_ready = 0;

static void backend_init_once(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_backend_ready, &expected, 1)) {
        llama_log_set(quiet_log, NULL);
        llama_backend_init();
        atomic_store(&g_backend_ready, 2);
    }
    while (atomic_load(&g_backend_ready) != 2) { /* another thread is initialising */ }
}

/* ==== Model =========================================================== */

struct lm_model {
    struct llama_model*        model;
    const struct llama_vocab*  vocab;
    struct llama_context*      gen_ctx;
    struct llama_context*      emb_ctx;
    const lm_profile_t*        profile;
    int64_t                    context_tokens;
    int32_t                    threads;
    int32_t                    gpu_layers;
    int64_t                    file_size;
    char                       name[128];
    char                       arch[64];
};

typedef struct {
    lm_progress_fn fn;
    void*          user;
    int            cancelled;
} progress_t;

static bool on_progress(float fraction, void* user) {
    progress_t* p = (progress_t*)user;
    if (p->fn && !p->fn(p->user, fraction)) {
        p->cancelled = 1;
        return false;
    }
    return true;
}

static int meta_str(const struct llama_model* m, const char* key, char* buf, size_t n) {
    buf[0] = '\0';
    return llama_model_meta_val_str(m, key, buf, n) >= 0;
}

static const lm_profile_t* pick_profile(const char* arch, int is_embedding) {
    for (const lm_profile_t* p = k_profiles; p->id; p++) {
        if (p->is_embedding != is_embedding) continue;
        /* Profiles are keyed by architecture family: "qwen3-...". */
        size_t al = strlen(arch);
        if (al > 0 && strncmp(p->id, arch, al) == 0 && p->id[al] == '-') return p;
    }
    return is_embedding ? &k_generic_embed : &k_generic_gen;
}

int lm_load(const char* path, const lm_load_params_t* params, lm_model_t** out) {
    if (path == NULL || params == NULL || out == NULL) return LM_EINVAL;
    *out = NULL;
    int64_t size = lisa_file_size(path);
    if (size == LISA_PLAT_ENOENT) return LM_ENOTFOUND;
    if (size < 0) return LM_EIO;
    if (params->context_tokens < 0 || params->threads < 0) return LM_EINVAL;

    backend_init_once();

    progress_t prog = { params->progress, params->progress_user, 0 };
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = params->gpu_layers < 0 ? 999 : params->gpu_layers;
    mp.progress_callback = on_progress;
    mp.progress_callback_user_data = &prog;

    struct llama_model* model = llama_model_load_from_file(path, mp);
    if (model == NULL) return prog.cancelled ? LM_ECANCELLED : LM_EFORMAT;

    lm_model_t* m = (lm_model_t*)calloc(1, sizeof(*m));
    if (m == NULL) {
        llama_model_free(model);
        return LM_ENOMEM;
    }
    m->model = model;
    m->vocab = llama_model_get_vocab(model);
    m->file_size = size;
    m->gpu_layers = params->gpu_layers;
    m->threads = params->threads;
    m->context_tokens = params->context_tokens ? params->context_tokens : DEFAULT_CONTEXT;
    if (m->context_tokens > llama_model_n_ctx_train(model))
        m->context_tokens = llama_model_n_ctx_train(model);

    meta_str(model, "general.name", m->name, sizeof(m->name));
    meta_str(model, "general.architecture", m->arch, sizeof(m->arch));

    /* Embedding models declare a pooling type in their metadata. */
    char key[96], val[32];
    snprintf(key, sizeof(key), "%s.pooling_type", m->arch);
    int is_embedding = meta_str(model, key, val, sizeof(val)) && strcmp(val, "0") != 0;
    m->profile = pick_profile(m->arch, is_embedding);

    *out = m;
    return LM_OK;
}

void lm_free(lm_model_t* m) {
    if (m == NULL) return;
    if (m->gen_ctx) llama_free(m->gen_ctx);
    if (m->emb_ctx) llama_free(m->emb_ctx);
    llama_model_free(m->model);
    free(m);
}

void lm_get_info(const lm_model_t* m, lm_info_t* out) {
    memset(out, 0, sizeof(*out));
    out->name = m->name;
    out->architecture = m->arch;
    out->profile_id = m->profile->id;
    out->file_size = m->file_size;
    out->context_train = llama_model_n_ctx_train(m->model);
    out->context_tokens = m->context_tokens;
    out->is_embedding = m->profile->is_embedding;
    out->embedding_dim = out->is_embedding ? llama_model_n_embd_out(m->model) : 0;
    out->gpu_offloaded = m->gpu_layers != 0;
    out->supports_truncation = m->profile->supports_truncation;
}

/* ==== Verification ==================================================== */

int lm_verify_file(const char* path, const lm_profile_t** out, char* hex_out) {
    if (out) *out = NULL;
    if (path == NULL) return LM_EINVAL;
    if (!lisa_path_exists(path)) return LM_ENOTFOUND;
    FILE* f = fopen(path, "rb");
    if (f == NULL) return LM_EIO;

    enum { CHUNK = 1 << 20 };
    unsigned char* buf = (unsigned char*)malloc(CHUNK);
    if (buf == NULL) {
        fclose(f);
        return LM_ENOMEM;
    }
    sha256_t ctx;
    sha256_init(&ctx);
    size_t n;
    int64_t total = 0;
    while ((n = fread(buf, 1, CHUNK, f)) > 0) {
        sha256_update(&ctx, buf, n);
        total += (int64_t)n;
    }
    int read_error = ferror(f);
    fclose(f);
    free(buf);
    if (read_error) return LM_EIO;

    unsigned char digest[SHA256_DIGEST_SIZE];
    sha256_final(&ctx, digest);
    char hex[65];
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    if (hex_out) memcpy(hex_out, hex, sizeof(hex));

    for (const lm_profile_t* p = k_profiles; p->id; p++) {
        if (p->file_size == total && strcmp(p->sha256, hex) == 0) {
            if (out) *out = p;
            return LM_OK;
        }
    }
    return LM_EFORMAT;
}

/* ==== Contexts ======================================================== */

static int ensure_gen_ctx(lm_model_t* m) {
    if (m->gen_ctx) return LM_OK;
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t)m->context_tokens;
    cp.n_batch = PROMPT_BATCH;
    cp.n_ubatch = PROMPT_BATCH;
    cp.n_seq_max = 1;
    cp.no_perf = true;
    if (m->threads > 0) {
        cp.n_threads = m->threads;
        cp.n_threads_batch = m->threads;
    }
    m->gen_ctx = llama_init_from_model(m->model, cp);
    return m->gen_ctx ? LM_OK : LM_ENOMEM;
}

static int ensure_emb_ctx(lm_model_t* m) {
    if (m->emb_ctx) return LM_OK;
    struct llama_context_params cp = llama_context_default_params();
    /*
     * The slot must also fit the model: a BERT embedder has an absolute
     * position table of 512 entries, and sending it more than that walks
     * off the end of the table inside ggml. context_tokens is already
     * clamped to what the model was trained for.
     */
    int64_t slot = m->context_tokens < EMBED_SLOT_TOKENS ? m->context_tokens : EMBED_SLOT_TOKENS;
    int64_t n = slot * EMBED_MAX_SEQ;
    cp.n_ctx = (uint32_t)n;
    cp.n_batch = (uint32_t)n;   /* pooled embeddings need the whole sequence in one batch */
    cp.n_ubatch = (uint32_t)n;
    cp.n_seq_max = EMBED_MAX_SEQ;
    cp.embeddings = true;
    cp.no_perf = true;
    if (m->threads > 0) {
        cp.n_threads = m->threads;
        cp.n_threads_batch = m->threads;
    }
    m->emb_ctx = llama_init_from_model(m->model, cp);
    return m->emb_ctx ? LM_OK : LM_ENOMEM;
}

/* Tokenize into a malloc'd array. */
static int tokenize(const lm_model_t* m, const char* text, int add_special, int parse_special,
                    llama_token** out, int32_t* n_out) {
    int32_t len = (int32_t)strlen(text);
    int32_t cap = len + 16;
    llama_token* t = (llama_token*)malloc((size_t)cap * sizeof(llama_token));
    if (t == NULL) return LM_ENOMEM;
    int32_t n = llama_tokenize(m->vocab, text, len, t, cap, add_special, parse_special);
    if (n < 0) {
        cap = -n;
        llama_token* g = (llama_token*)realloc(t, (size_t)cap * sizeof(llama_token));
        if (g == NULL) {
            free(t);
            return LM_ENOMEM;
        }
        t = g;
        n = llama_tokenize(m->vocab, text, len, t, cap, add_special, parse_special);
    }
    if (n < 0) {
        free(t);
        return LM_ERUNTIME;
    }
    *out = t;
    *n_out = n;
    return LM_OK;
}

/* ==== Chat formatting ================================================= */

int lm_format_chat(lm_model_t* m, const lm_message_t* msgs, int64_t n, int add_assistant,
                   char** out) {
    if (m == NULL || msgs == NULL || n <= 0 || out == NULL) return LM_EINVAL;
    *out = NULL;
    if (m->profile->is_embedding) return LM_EWRONGKIND;

    struct llama_chat_message* cm =
        (struct llama_chat_message*)malloc((size_t)n * sizeof(*cm));
    if (cm == NULL) return LM_ENOMEM;
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        if (msgs[i].role == NULL || msgs[i].content == NULL) {
            free(cm);
            return LM_EINVAL;
        }
        cm[i].role = msgs[i].role;
        cm[i].content = msgs[i].content;
        total += strlen(msgs[i].content) + strlen(msgs[i].role);
    }

    const char* tmpl = llama_model_chat_template(m->model, NULL);
    if (tmpl == NULL) tmpl = "chatml";
    const char* prefix = add_assistant && m->profile->assistant_prefix
                             ? m->profile->assistant_prefix : "";

    int32_t cap = (int32_t)(2 * total + 256);
    char* buf = NULL;
    int32_t len = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        free(buf);
        buf = (char*)malloc((size_t)cap + strlen(prefix) + 1);
        if (buf == NULL) {
            free(cm);
            return LM_ENOMEM;
        }
        len = llama_chat_apply_template(tmpl, cm, (size_t)n, add_assistant != 0, buf, cap);
        if (len < 0) {
            free(buf);
            free(cm);
            return LM_EFORMAT;  /* template not supported */
        }
        if (len <= cap) break;
        cap = len;
    }
    free(cm);
    memcpy(buf + len, prefix, strlen(prefix));
    buf[len + (int32_t)strlen(prefix)] = '\0';
    *out = buf;
    return LM_OK;
}

/* ==== Generation ====================================================== */

typedef struct {
    char*   data;
    size_t  len, cap;
    size_t  emitted;  /* bytes already passed to the token callback */
} text_buf_t;

static int buf_append(text_buf_t* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char* d = (char*)realloc(b->data, cap);
        if (d == NULL) return LM_ENOMEM;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return LM_OK;
}

/* Length of the longest prefix of s[0..n) that ends on a UTF-8 boundary. */
static size_t utf8_complete(const char* s, size_t n) {
    size_t i = n;
    int back = 0;
    while (i > 0 && back < 4) {
        unsigned char c = (unsigned char)s[i - 1];
        if ((c & 0xC0) != 0x80) {
            size_t need = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            return (n - (i - 1) >= need) ? n : i - 1;
        }
        i--;
        back++;
    }
    return n;
}

static struct llama_sampler* make_sampler(const lm_model_t* m, const lm_gen_params_t* p) {
    struct llama_sampler* s = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (s == NULL) return NULL;
    float temp = p->temperature < 0 ? m->profile->temperature : p->temperature;
    if (temp == 0.0f) {
        llama_sampler_chain_add(s, llama_sampler_init_greedy());
        return s;
    }
    if (m->profile->presence_penalty != 0.0f)
        llama_sampler_chain_add(s, llama_sampler_init_penalties(
            llama_vocab_n_tokens(m->vocab), PENALTY_LAST_N, 1.0f, 0.0f,
            m->profile->presence_penalty));
    if (m->profile->top_k > 0) llama_sampler_chain_add(s, llama_sampler_init_top_k(m->profile->top_k));
    if (m->profile->top_p > 0) llama_sampler_chain_add(s, llama_sampler_init_top_p(m->profile->top_p, 1));
    llama_sampler_chain_add(s, llama_sampler_init_temp(temp));
    llama_sampler_chain_add(s, llama_sampler_init_dist((uint32_t)(p->seed ? p->seed : 1)));
    return s;
}

int lm_count_tokens(const lm_model_t* m, const char* text, int parse_special, int64_t* out) {
    if (out) *out = 0;
    if (m == NULL || text == NULL || out == NULL) return LM_EINVAL;
    if (text[0] == '\0') return LM_OK;
    llama_token* toks = NULL;
    int32_t n = 0;
    int rc = tokenize(m, text, 0, parse_special ? 1 : 0, &toks, &n);
    if (rc != LM_OK) return rc;
    free(toks);
    *out = n;
    return LM_OK;
}

int lm_generate(lm_model_t* m, const char* prompt, const lm_gen_params_t* p,
                char** out_text, int64_t* out_tokens) {
    if (out_text) *out_text = NULL;
    if (out_tokens) *out_tokens = 0;
    if (m == NULL || prompt == NULL || p == NULL || p->max_tokens <= 0) return LM_EINVAL;
    if (m->profile->is_embedding) return LM_EWRONGKIND;

    int rc = ensure_gen_ctx(m);
    if (rc != LM_OK) return rc;
    llama_memory_clear(llama_get_memory(m->gen_ctx), true);

    llama_token* toks = NULL;
    int32_t n_prompt = 0;
    rc = tokenize(m, prompt, 1, 1, &toks, &n_prompt);
    if (rc != LM_OK) return rc;
    int64_t n_ctx = llama_n_ctx(m->gen_ctx);
    if (n_prompt == 0 || n_prompt >= n_ctx) {
        free(toks);
        return n_prompt == 0 ? LM_EINVAL : LM_ECONTEXT;
    }
    int64_t budget = n_ctx - n_prompt;
    int64_t max_new = p->max_tokens < budget ? p->max_tokens : budget;

    /* Prompt, in batches. */
    for (int32_t i = 0; i < n_prompt && rc == LM_OK; i += PROMPT_BATCH) {
        int32_t len = n_prompt - i < PROMPT_BATCH ? n_prompt - i : PROMPT_BATCH;
        if (llama_decode(m->gen_ctx, llama_batch_get_one(toks + i, len)) != 0) rc = LM_ERUNTIME;
    }
    free(toks);
    if (rc != LM_OK) return rc;

    struct llama_sampler* smpl = make_sampler(m, p);
    if (smpl == NULL) return LM_ENOMEM;

    text_buf_t out = { NULL, 0, 0, 0 };
    if (buf_append(&out, "", 0) != LM_OK) rc = LM_ENOMEM;
    int64_t produced = 0;
    int stopped = 0;
    while (rc == LM_OK && produced < max_new && !stopped) {
        llama_token tok = llama_sampler_sample(smpl, m->gen_ctx, -1);
        if (llama_vocab_is_eog(m->vocab, tok)) break;
        produced++;

        char piece[256];
        int32_t n = llama_token_to_piece(m->vocab, tok, piece, sizeof(piece), 0, false);
        if (n < 0) {
            rc = LM_ERUNTIME;
            break;
        }
        rc = buf_append(&out, piece, (size_t)n);
        if (rc != LM_OK) break;

        if (p->on_token) {
            size_t ready = utf8_complete(out.data, out.len);
            if (ready > out.emitted) {
                if (p->on_token(p->on_token_user, out.data + out.emitted,
                                (int64_t)(ready - out.emitted)) != 0)
                    stopped = 1;
                out.emitted = ready;
            }
        }
        if (!stopped && llama_decode(m->gen_ctx, llama_batch_get_one(&tok, 1)) != 0)
            rc = LM_ERUNTIME;
    }
    if (rc == LM_OK && p->on_token && !stopped && out.len > out.emitted)
        p->on_token(p->on_token_user, out.data + out.emitted, (int64_t)(out.len - out.emitted));

    llama_sampler_free(smpl);
    /*
     * Nothing generated: the model produced no text at all. This happens
     * when the GPU cannot allocate (Metal reports the failure after
     * llama_decode has already returned success) and sampling then ends
     * the sequence at once. An empty answer is never useful, so report it.
     */
    if (rc == LM_OK && produced == 0) rc = LM_ERUNTIME;
    if (rc != LM_OK) {
        free(out.data);
        return rc;
    }
    if (out_tokens) *out_tokens = produced;
    if (out_text) *out_text = out.data;
    else free(out.data);
    return LM_OK;
}

/* ==== Embeddings ====================================================== */

int lm_embed(lm_model_t* m, int kind, const char* const* texts, int64_t n,
             float* out, int64_t dim) {
    if (m == NULL || texts == NULL || n <= 0 || out == NULL) return LM_EINVAL;
    if (kind != LM_EMBED_DOCUMENT && kind != LM_EMBED_QUERY) return LM_EINVAL;
    if (!m->profile->is_embedding) return LM_EWRONGKIND;
    int64_t native = llama_model_n_embd_out(m->model);
    if (dim == 0) dim = native;
    if (dim < 1 || dim > native || (dim < native && !m->profile->supports_truncation))
        return LM_EINVAL;

    int rc = ensure_emb_ctx(m);
    if (rc != LM_OK) return rc;
    int64_t n_ctx = llama_n_ctx(m->emb_ctx);
    const char* prefix = kind == LM_EMBED_QUERY ? m->profile->query_prefix
                                                : m->profile->document_prefix;
    if (prefix == NULL) prefix = "";
    int64_t per_seq = n_ctx / EMBED_MAX_SEQ;

    /*
     * Tokenise every passage first, then embed as many as fit in one call
     * (at most EMBED_MAX_SEQ sequences, and no more tokens than the
     * context holds). Each sequence is pooled separately by llama.cpp, so
     * the vectors are the same as embedding them one at a time.
     */
    llama_token** toks = (llama_token**)calloc((size_t)n, sizeof(llama_token*));
    int32_t* lens = (int32_t*)calloc((size_t)n, sizeof(int32_t));
    if (toks == NULL || lens == NULL) {
        free(toks);
        free(lens);
        return LM_ENOMEM;
    }
    for (int64_t i = 0; i < n && rc == LM_OK; i++) {
        if (texts[i] == NULL) {
            rc = LM_EINVAL;
            break;
        }
        size_t pl = strlen(prefix), tl = strlen(texts[i]);
        char* input = (char*)malloc(pl + tl + 1);
        if (input == NULL) {
            rc = LM_ENOMEM;
            break;
        }
        memcpy(input, prefix, pl);
        memcpy(input + pl, texts[i], tl + 1);
        rc = tokenize(m, input, 1, 0, &toks[i], &lens[i]);
        free(input);
        if (rc != LM_OK) break;

        /* Last-token pooling needs the end-of-text token as the last token. */
        if (m->profile->append_eos && !llama_vocab_get_add_eos(m->vocab)) {
            llama_token* g = (llama_token*)realloc(toks[i], (size_t)(lens[i] + 1) * sizeof(llama_token));
            if (g == NULL) {
                rc = LM_ENOMEM;
                break;
            }
            toks[i] = g;
            toks[i][lens[i]++] = llama_vocab_eos(m->vocab);
        }
        /*
         * A passage longer than one slot cannot be embedded even alone,
         * because the slot size is fixed when the context is created.
         * The tail is dropped for the vector rather than failing the
         * whole file: the text is still stored whole, still found by the
         * keyword index, and still quoted in full in a citation.
         */
        if (lens[i] == 0) rc = LM_EINVAL;
        else if (lens[i] > per_seq) lens[i] = (int32_t)per_seq;
    }

    for (int64_t first = 0; rc == LM_OK && first < n; ) {
        /* How many passages fit in this call. */
        int64_t count = 0, tokens = 0;
        while (first + count < n && count < EMBED_MAX_SEQ) {
            int32_t len = lens[first + count];
            if (count > 0 && (len > per_seq || tokens + len > n_ctx)) break;
            tokens += len;
            count++;
            if (len > per_seq) break;   /* a long passage goes alone */
        }

        llama_memory_clear(llama_get_memory(m->emb_ctx), true);
        struct llama_batch batch = llama_batch_init((int32_t)tokens, 0, (int32_t)count);
        int32_t at = 0;
        for (int64_t k = 0; k < count; k++) {
            for (int32_t t = 0; t < lens[first + k]; t++, at++) {
                batch.token[at] = toks[first + k][t];
                batch.pos[at] = t;
                batch.n_seq_id[at] = 1;
                batch.seq_id[at][0] = (llama_seq_id)k;
                batch.logits[at] = 1;
            }
        }
        batch.n_tokens = at;
        int dec = llama_model_has_encoder(m->model) ? llama_encode(m->emb_ctx, batch)
                                                    : llama_decode(m->emb_ctx, batch);
        llama_batch_free(batch);
        if (dec != 0) {
            rc = LM_ERUNTIME;
            break;
        }

        for (int64_t k = 0; k < count; k++) {
            const float* e = llama_get_embeddings_seq(m->emb_ctx, (llama_seq_id)k);
            if (e == NULL) {
                rc = LM_ERUNTIME;
                break;
            }
            float* dst = out + (first + k) * dim;
            double norm = 0.0;
            for (int64_t j = 0; j < dim; j++) {
                dst[j] = e[j];
                norm += (double)e[j] * e[j];
            }
            float inv = norm > 0 ? (float)(1.0 / sqrt(norm)) : 0.0f;
            for (int64_t j = 0; j < dim; j++) dst[j] *= inv;
        }
        first += count;
    }

    for (int64_t i = 0; i < n; i++) free(toks[i]);
    free(toks);
    free(lens);
    return rc;
}
