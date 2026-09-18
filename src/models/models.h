/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LISA_MODELS_H
#define LISA_MODELS_H

/*
 * LISA models — internal interface over the model runtime (llama.cpp).
 *
 * Nothing outside src/models/ includes llama.h. The public API
 * (include/lisa.h) is implemented on top of this header, so the runtime
 * can be changed or extended (other backends, other runtimes) without
 * touching callers.
 *
 * A model handle is used by one thread at a time.
 *
 * Return codes: LM_OK (0) or a negative LM_E* code.
 */

#include <stdint.h>

#define LM_OK          0
#define LM_EINVAL     -1
#define LM_EIO        -2   /* file missing or unreadable */
#define LM_EFORMAT    -3   /* not a usable GGUF model */
#define LM_ENOMEM     -4
#define LM_ECANCELLED -5   /* progress callback asked to stop */
#define LM_EWRONGKIND -6   /* e.g. embed() on a generation model */
#define LM_ECONTEXT   -7   /* input does not fit the context window */
#define LM_ERUNTIME   -8   /* the runtime failed during decode */
#define LM_ENOTFOUND  -9   /* model file does not exist */

typedef struct lm_model lm_model_t;

/* Return non-zero to continue, 0 to cancel. fraction in [0, 1]. */
typedef int (*lm_progress_fn)(void* user, float fraction);

typedef struct {
    int32_t        gpu_layers;     /* -1: all on GPU; 0: CPU only */
    int64_t        context_tokens; /* 0: default (4096) */
    int32_t        threads;        /* 0: runtime default */
    lm_progress_fn progress;
    void*          progress_user;
} lm_load_params_t;

/* ---- known model profiles (model catalog seam) ------------------------ */

typedef struct {
    const char* id;              /* stable LISA identifier */
    const char* file_name;
    int64_t     file_size;
    const char* sha256;          /* lowercase hex */
    const char* license;         /* SPDX */
    const char* source;          /* where it came from */
    int         is_embedding;

    /* generation */
    const char* assistant_prefix;  /* appended after the chat template's assistant turn */
    float       temperature;
    float       top_p;
    int32_t     top_k;
    float       presence_penalty;

    /* embedding */
    const char* query_prefix;
    const char* document_prefix;
    int         supports_truncation;  /* Matryoshka: leading dims usable alone */
    int         append_eos;           /* end input with EOS (last-token pooling) */
} lm_profile_t;

/* All known models (terminated by an entry with id == NULL). */
const lm_profile_t* lm_known_models(void);

/*
 * Hash a model file and look it up among known models.
 *   LM_OK: *out = the matching known model.
 *   LM_EFORMAT: file exists but matches no known model (*out = NULL).
 *   LM_EIO: cannot read the file.
 * hex_out (65 bytes, optional) receives the file's SHA-256.
 */
int lm_verify_file(const char* path, const lm_profile_t** out, char* hex_out);

/* ---- loading ---------------------------------------------------------- */

int  lm_load(const char* path, const lm_load_params_t* params, lm_model_t** out);
void lm_free(lm_model_t* m);

typedef struct {
    const char* name;             /* from GGUF metadata; owned by the model */
    const char* architecture;
    const char* profile_id;       /* matching profile, or "generic" */
    int64_t     file_size;
    int64_t     context_train;
    int64_t     context_tokens;   /* context window in use */
    int64_t     embedding_dim;    /* output dim for embedding models, else 0 */
    int         is_embedding;
    int         gpu_offloaded;    /* 1 if layers run on the GPU */
} lm_info_t;

void lm_get_info(const lm_model_t* m, lm_info_t* out);

/* ---- generation ------------------------------------------------------- */

/* Return 0 to continue, non-zero to stop generation early. */
typedef int (*lm_token_fn)(void* user, const char* piece, int64_t len);

typedef struct {
    int64_t     max_tokens;   /* > 0 */
    float       temperature;  /* 0: greedy; < 0: profile default */
    uint64_t    seed;
    lm_token_fn on_token;     /* optional; receives text pieces as generated */
    void*       on_token_user;
} lm_gen_params_t;

typedef struct {
    const char* role;     /* "system", "user", "assistant" */
    const char* content;
} lm_message_t;

/*
 * Format messages with the model's chat template (plus the profile's
 * assistant_prefix when add_assistant is set). *out is malloc'd.
 */
int lm_format_chat(lm_model_t* m, const lm_message_t* msgs, int64_t n,
                   int add_assistant, char** out);

/*
 * Generate a continuation of prompt (raw text; special tokens parsed).
 * Each call starts from an empty context. *out_text is malloc'd; the
 * number of generated tokens goes to *out_tokens (both optional).
 */
int lm_generate(lm_model_t* m, const char* prompt, const lm_gen_params_t* params,
                char** out_text, int64_t* out_tokens);

/* ---- embeddings ------------------------------------------------------- */

#define LM_EMBED_DOCUMENT 0
#define LM_EMBED_QUERY    1

/*
 * Embed n texts. out: n * dim floats. dim: 0 or the model's embedding
 * dimension, or a smaller value if the profile supports truncation.
 * Output vectors are L2-normalised.
 */
int lm_embed(lm_model_t* m, int kind, const char* const* texts, int64_t n,
             float* out, int64_t dim);

#endif /* LISA_MODELS_H */
