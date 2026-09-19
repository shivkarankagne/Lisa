/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Public model API (include/lisa.h, "Models") over src/models/models.h.
 */

#include "api_internal.h"

#include <stdlib.h>
#include <string.h>

#include "../models/models.h"

#define MAX_GENERATE_TOKENS 32768

struct lisa_model {
    lisa_context_t* ctx;
    lm_model_t*     lm;
};

static int from_lm(int rc) {
    switch (rc) {
    case LM_OK:         return LISA_OK;
    case LM_EINVAL:     return LISA_E_INVALID_ARGUMENT;
    case LM_EIO:        return LISA_E_IO;
    case LM_EFORMAT:    return LISA_E_FORMAT;
    case LM_ENOMEM:     return LISA_E_NO_MEMORY;
    case LM_ECANCELLED: return LISA_E_CANCELLED;
    case LM_EWRONGKIND: return LISA_E_WRONG_MODEL_KIND;
    case LM_ECONTEXT:   return LISA_E_TOO_LONG;
    case LM_ENOTFOUND:  return LISA_E_NOT_FOUND;
    default:            return LISA_E_INTERNAL;
    }
}

/* ---- load / info ------------------------------------------------------ */

int lisa_model_load(lisa_context_t* ctx, const char* path,
                    const lisa_model_options_t* options, lisa_model_t** out) {
    if (out == NULL) return LISA_E_INVALID_ARGUMENT;
    *out = NULL;
    if (ctx == NULL || path == NULL) return LISA_E_INVALID_ARGUMENT;

    lm_load_params_t p = { -1, 0, 0, NULL, NULL };
    if (options != NULL) {
        if (options->struct_size < sizeof(size_t)) return LISA_E_INVALID_ARGUMENT;
        if (HAS_FIELD(options, lisa_model_options_t, gpu_layers)) p.gpu_layers = options->gpu_layers;
        if (HAS_FIELD(options, lisa_model_options_t, context_tokens)) p.context_tokens = options->context_tokens;
        if (HAS_FIELD(options, lisa_model_options_t, threads)) p.threads = options->threads;
        if (HAS_FIELD(options, lisa_model_options_t, progress)) p.progress = (lm_progress_fn)options->progress;
        if (HAS_FIELD(options, lisa_model_options_t, progress_user)) p.progress_user = options->progress_user;
    }

    lisa_model_t* m = (lisa_model_t*)ctx_alloc(ctx, sizeof(*m));
    if (m == NULL) return LISA_E_NO_MEMORY;
    m->ctx = ctx;
    int rc = from_lm(lm_load(path, &p, &m->lm));
    if (rc != LISA_OK) {
        ctx_free(ctx, m);
        return rc;
    }
    *out = m;
    return LISA_OK;
}

void lisa_model_free(lisa_model_t* m) {
    if (m == NULL) return;
    lm_free(m->lm);
    ctx_free(m->ctx, m);
}

int lisa_model_info(const lisa_model_t* m, lisa_model_info_t* info) {
    if (m == NULL || info == NULL || info->struct_size < sizeof(size_t))
        return LISA_E_INVALID_ARGUMENT;
    lm_info_t li;
    lm_get_info(m->lm, &li);
    if (HAS_FIELD(info, lisa_model_info_t, name)) info->name = li.name;
    if (HAS_FIELD(info, lisa_model_info_t, architecture)) info->architecture = li.architecture;
    if (HAS_FIELD(info, lisa_model_info_t, profile)) info->profile = li.profile_id;
    if (HAS_FIELD(info, lisa_model_info_t, file_size)) info->file_size = li.file_size;
    if (HAS_FIELD(info, lisa_model_info_t, context_train)) info->context_train = li.context_train;
    if (HAS_FIELD(info, lisa_model_info_t, context_tokens)) info->context_tokens = li.context_tokens;
    if (HAS_FIELD(info, lisa_model_info_t, embedding_dim)) info->embedding_dim = li.embedding_dim;
    if (HAS_FIELD(info, lisa_model_info_t, is_embedding)) info->is_embedding = li.is_embedding;
    if (HAS_FIELD(info, lisa_model_info_t, gpu)) info->gpu = li.gpu_offloaded;
    return LISA_OK;
}

/* ---- known models ----------------------------------------------------- */

static void to_known(const lm_profile_t* p, lisa_known_model_t* out) {
    out->id = p->id;
    out->file_name = p->file_name;
    out->file_size = p->file_size;
    out->sha256 = p->sha256;
    out->license = p->license;
    out->source = p->source;
    out->is_embedding = p->is_embedding;
}

int64_t lisa_known_model_count(void) {
    int64_t n = 0;
    for (const lm_profile_t* p = lm_known_models(); p->id; p++) n++;
    return n;
}

int lisa_known_model(int64_t index, lisa_known_model_t* out) {
    if (out == NULL || index < 0 || index >= lisa_known_model_count()) return LISA_E_INVALID_ARGUMENT;
    to_known(&lm_known_models()[index], out);
    return LISA_OK;
}

int lisa_model_verify(const char* path, lisa_known_model_t* match, char* sha256_hex) {
    if (path == NULL) return LISA_E_INVALID_ARGUMENT;
    const lm_profile_t* p = NULL;
    int rc = lm_verify_file(path, &p, sha256_hex);
    if (rc == LM_OK) {
        if (match) to_known(p, match);
        return LISA_OK;
    }
    if (rc == LM_EFORMAT) return LISA_E_UNSUPPORTED;
    return from_lm(rc);
}

/* ---- generation ------------------------------------------------------- */

static int gen_params(const lisa_generate_options_t* o, lm_gen_params_t* p) {
    p->max_tokens = 512;
    p->temperature = -1.0f;
    p->seed = 0;
    p->on_token = NULL;
    p->on_token_user = NULL;
    if (o == NULL) return LISA_OK;
    if (o->struct_size < sizeof(size_t)) return LISA_E_INVALID_ARGUMENT;
    if (HAS_FIELD(o, lisa_generate_options_t, max_tokens)) p->max_tokens = o->max_tokens;
    if (HAS_FIELD(o, lisa_generate_options_t, temperature)) p->temperature = o->temperature;
    if (HAS_FIELD(o, lisa_generate_options_t, seed)) p->seed = o->seed;
    if (HAS_FIELD(o, lisa_generate_options_t, on_token)) p->on_token = (lm_token_fn)o->on_token;
    if (HAS_FIELD(o, lisa_generate_options_t, on_token_user)) p->on_token_user = o->on_token_user;
    if (p->max_tokens < 1 || p->max_tokens > MAX_GENERATE_TOKENS) return LISA_E_INVALID_ARGUMENT;
    return LISA_OK;
}

/* Run lm_generate and copy its result into context-allocated memory. */
static int generate(lisa_model_t* m, const char* prompt, const lm_gen_params_t* p,
                    char** out_text, int64_t* out_tokens) {
    char* text = NULL;
    int64_t tokens = 0;
    int rc = from_lm(lm_generate(m->lm, prompt, p, out_text ? &text : NULL, &tokens));
    if (rc == LISA_OK && out_text) {
        *out_text = ctx_strdup(m->ctx, text);
        if (*out_text == NULL) rc = LISA_E_NO_MEMORY;
    }
    free(text);
    if (rc == LISA_OK && out_tokens) *out_tokens = tokens;
    return rc;
}

int lisa_generate(lisa_model_t* m, const char* prompt, const lisa_generate_options_t* options,
                  char** out_text, int64_t* out_tokens) {
    if (out_text) *out_text = NULL;
    if (out_tokens) *out_tokens = 0;
    if (m == NULL || prompt == NULL) return LISA_E_INVALID_ARGUMENT;
    lm_gen_params_t p;
    int rc = gen_params(options, &p);
    if (rc != LISA_OK) return rc;
    return generate(m, prompt, &p, out_text, out_tokens);
}

int lisa_chat(lisa_model_t* m, const lisa_message_t* messages, int64_t count,
              const lisa_generate_options_t* options, char** out_text, int64_t* out_tokens) {
    if (out_text) *out_text = NULL;
    if (out_tokens) *out_tokens = 0;
    if (m == NULL || messages == NULL || count <= 0) return LISA_E_INVALID_ARGUMENT;
    lm_gen_params_t p;
    int rc = gen_params(options, &p);
    if (rc != LISA_OK) return rc;

    /* lisa_message_t and lm_message_t have the same layout by design. */
    char* prompt = NULL;
    rc = from_lm(lm_format_chat(m->lm, (const lm_message_t*)messages, count, 1, &prompt));
    if (rc == LISA_OK) rc = generate(m, prompt, &p, out_text, out_tokens);
    free(prompt);
    return rc;
}

/* ---- embeddings ------------------------------------------------------- */

int lisa_embed(lisa_model_t* m, lisa_embed_kind kind, const char* const* texts,
               int64_t count, float* out, int64_t dim) {
    if (m == NULL || texts == NULL || count <= 0 || out == NULL || dim < 0)
        return LISA_E_INVALID_ARGUMENT;
    int lk = kind == LISA_EMBED_QUERY ? LM_EMBED_QUERY
           : kind == LISA_EMBED_DOCUMENT ? LM_EMBED_DOCUMENT : -1;
    if (lk < 0) return LISA_E_INVALID_ARGUMENT;
    return from_lm(lm_embed(m->lm, lk, texts, count, out, dim));
}
