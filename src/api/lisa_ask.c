/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * lisa_ask (include/lisa.h, "Ask"): the answer pipeline
 *
 *     retrieve -> filter -> rank -> dedupe -> budget -> generate
 *
 * filter..budget come from src/context; retrieve and generate need the
 * collection and models, so they live here as stages of the same list.
 */

#include "api_internal.h"

#include <stdlib.h>
#include <string.h>

#include "../context/context.h"
#include "../platform/platform.h"

#define MAX_ASK_TOP_K   100
#define MIN_BUDGET      64
#define STAGE_API_ERROR -100   /* stage failed; status in job->status */

typedef struct {
    int64_t       top_k;
    int64_t       budget;
    float         min_similarity;
    int64_t       max_tokens;
    float         temperature;
    lisa_token_fn on_token;
    void*         on_token_user;
    const char*   path_prefix;
    const lisa_principal_t* principal;
} ask_opts_t;

typedef struct {
    lisa_collection_t*  coll;
    lisa_model_t*       embed;
    lisa_model_t*       chat;
    ask_opts_t          o;
    const char*         question;
    int                 status;       /* LISA status of a failed stage */

    lisa_store_chunk_t* chunks;       /* strings behind the passages */
    char**              titles;
    int64_t             n_loaded;
    ctx_passage_t*      passages;

    char*               text;         /* malloc'd answer */
    int64_t             answer_tokens;
    int                 stopped;
    int64_t             t0_ns, first_ns;
} ask_job_t;

static int read_options(const lisa_ask_options_t* in, ask_opts_t* o) {
    o->top_k = 8;
    o->budget = 1100;
    o->min_similarity = 0.40f;
    o->max_tokens = 512;
    o->temperature = 0.0f;
    o->on_token = NULL;
    o->on_token_user = NULL;
    o->path_prefix = NULL;
    o->principal = NULL;
    if (in == NULL) return LISA_OK;
    if (in->struct_size < sizeof(size_t)) return LISA_E_INVALID_ARGUMENT;
    if (HAS_FIELD(in, lisa_ask_options_t, top_k)) o->top_k = in->top_k;
    if (HAS_FIELD(in, lisa_ask_options_t, prompt_budget)) o->budget = in->prompt_budget;
    if (HAS_FIELD(in, lisa_ask_options_t, min_similarity)) o->min_similarity = in->min_similarity;
    if (HAS_FIELD(in, lisa_ask_options_t, max_tokens)) o->max_tokens = in->max_tokens;
    if (HAS_FIELD(in, lisa_ask_options_t, temperature)) o->temperature = in->temperature;
    if (HAS_FIELD(in, lisa_ask_options_t, on_token)) o->on_token = in->on_token;
    if (HAS_FIELD(in, lisa_ask_options_t, on_token_user)) o->on_token_user = in->on_token_user;
    if (HAS_FIELD(in, lisa_ask_options_t, path_prefix)) o->path_prefix = in->path_prefix;
    if (HAS_FIELD(in, lisa_ask_options_t, principal)) o->principal = in->principal;
    if (o->top_k < 1 || o->top_k > MAX_ASK_TOP_K || o->budget < MIN_BUDGET || o->max_tokens < 1 ||
        !(o->min_similarity >= -1.0f && o->min_similarity <= 1.0f) || !(o->temperature >= -1.0f))
        return LISA_E_INVALID_ARGUMENT;
    return LISA_OK;
}

static int fail(ask_job_t* j, int status) {
    j->status = status;
    return STAGE_API_ERROR;
}

/* ---- retrieve ----------------------------------------------------------- */

static float dot(const float* a, const float* b, int64_t n) {
    float s = 0;
    for (int64_t i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

static int stage_retrieve(ctx_state_t* st) {
    ask_job_t* j = (ask_job_t*)st->user;
    const lisa_context_t* ctx = j->coll->ctx;
    float* qvec = NULL;
    int rc = lisa_api_embed_query(j->coll, j->embed, j->question, &qvec);
    if (rc != LISA_OK) return fail(j, rc);

    int64_t dim = lisa_store_dim(j->coll->store);
    lisa_scored_hit_t* hits = (lisa_scored_hit_t*)ctx_alloc(ctx, (size_t)j->o.top_k * sizeof(*hits));
    float* vec = (float*)ctx_alloc(ctx, (size_t)dim * sizeof(float));
    j->chunks = (lisa_store_chunk_t*)calloc((size_t)j->o.top_k, sizeof(lisa_store_chunk_t));
    j->titles = (char**)calloc((size_t)j->o.top_k, sizeof(char*));
    j->passages = (ctx_passage_t*)calloc((size_t)j->o.top_k, sizeof(ctx_passage_t));
    if (!hits || !vec || !j->chunks || !j->titles || !j->passages) rc = LISA_E_NO_MEMORY;

    int64_t n = 0;
    if (rc == LISA_OK) {
        lisa_query_t q = LISA_QUERY_INIT;
        q.text = j->question;
        q.vector = qvec;
        q.top_k = j->o.top_k;
        q.path_prefix = j->o.path_prefix;
        q.principal = j->o.principal;
        rc = lisa_collection_query(j->coll, &q, hits, j->o.top_k, &n);
    }
    for (int64_t i = 0; rc == LISA_OK && i < n; i++) {
        lisa_store_chunk_t* c = &j->chunks[j->n_loaded];
        int src = lisa_store_get(j->coll->store, hits[i].id, c);
        if (src == LISA_STORE_ENOTFOUND) continue;   /* removed since the search */
        rc = lisa_api_from_store(src);
        if (rc != LISA_OK) break;
        j->n_loaded++;

        /* Unit vectors: cosine = 1 - d^2 / 2. Keyword-only hits have no distance. */
        float sim;
        if (hits[i].distance >= 0) {
            sim = 1.0f - hits[i].distance / 2.0f;
        } else if (lisa_store_get_vector(j->coll->store, hits[i].id, vec) == LISA_STORE_OK) {
            sim = dot(qvec, vec, dim);
        } else {
            sim = -1.0f;
        }

        lisa_store_doc_t d;
        memset(&d, 0, sizeof(d));
        if (lisa_store_doc_get(j->coll->store, c->doc_id, &d) == LISA_STORE_OK && d.title) {
            j->titles[j->n_loaded - 1] = d.title;
            d.title = NULL;
        }
        lisa_store_doc_free(&d);

        ctx_passage_t* p = &j->passages[st->n++];
        p->id = hits[i].id;
        p->doc_id = c->doc_id;
        p->source_path = c->source_path;
        p->title = j->titles[j->n_loaded - 1];
        p->page = c->page;
        p->offset = c->offset;
        p->length = c->length;
        p->text = c->text;
        p->score = hits[i].score;
        p->similarity = sim;
    }
    st->passages = j->passages;
    st->considered = st->n;
    ctx_free(ctx, hits);
    ctx_free(ctx, vec);
    ctx_free(ctx, qvec);
    return rc == LISA_OK ? CTX_OK : fail(j, rc);
}

/* ---- generate ----------------------------------------------------------- */

static int on_piece(void* user, const char* s, int64_t n) {
    ask_job_t* j = (ask_job_t*)user;
    if (j->first_ns == 0) j->first_ns = lisa_time_monotonic_ns();
    if (j->o.on_token && j->o.on_token(j->o.on_token_user, s, n) != 0) {
        j->stopped = 1;
        return 1;
    }
    return 0;
}

static int count_prompt(void* user, const char* system, const char* msg, int64_t* out) {
    ask_job_t* j = (ask_job_t*)user;
    lm_message_t m[2] = { { "system", system }, { "user", msg } };
    char* prompt = NULL;
    int rc = lm_format_chat(j->chat->lm, m, 2, 1, &prompt);
    if (rc == LM_OK) rc = lm_count_tokens(j->chat->lm, prompt, 1, out);
    free(prompt);
    if (rc != LM_OK) {
        j->status = lisa_api_from_lm(rc);
        return -1;
    }
    return 0;
}

static int stage_generate(ctx_state_t* st) {
    ask_job_t* j = (ask_job_t*)st->user;
    if (st->n == 0 || st->user_msg == NULL) {
        /* Nothing relevant: say so without running the model. */
        j->text = strdup(CTX_NOT_FOUND_TEXT);
        if (j->text == NULL) return fail(j, LISA_E_NO_MEMORY);
        on_piece(j, j->text, (int64_t)strlen(j->text));
        return CTX_OK;
    }
    lm_message_t m[2] = { { "system", st->system }, { "user", st->user_msg } };
    char* prompt = NULL;
    int rc = lm_format_chat(j->chat->lm, m, 2, 1, &prompt);
    if (rc == LM_OK) {
        lm_gen_params_t p;
        memset(&p, 0, sizeof(p));
        p.max_tokens = j->o.max_tokens;
        p.temperature = j->o.temperature;
        p.on_token = on_piece;
        p.on_token_user = j;
        rc = lm_generate(j->chat->lm, prompt, &p, &j->text, &j->answer_tokens);
    }
    free(prompt);
    return rc == LM_OK ? CTX_OK : fail(j, lisa_api_from_lm(rc));
}

/* ---- answer ------------------------------------------------------------- */

typedef struct {
    lisa_answer_t   pub;   /* first: lisa_answer_t* is this struct */
    lisa_context_t* ctx;
} answer_box_t;

void lisa_answer_free(lisa_answer_t* a) {
    if (a == NULL) return;
    answer_box_t* box = (answer_box_t*)a;
    const lisa_context_t* ctx = box->ctx;
    for (int64_t i = 0; i < a->citation_count; i++) {
        lisa_citation_t* c = &a->citations[i];
        ctx_free(ctx, (void*)c->doc_id);
        ctx_free(ctx, (void*)c->source_path);
        ctx_free(ctx, (void*)c->title);
        ctx_free(ctx, (void*)c->quote);
        ctx_free(ctx, (void*)c->content_hash);
    }
    ctx_free(ctx, a->citations);
    ctx_free(ctx, (void*)a->text);
    ctx_free(ctx, box);
}

static int make_answer(ask_job_t* j, const ctx_state_t* st, lisa_answer_t** out) {
    lisa_context_t* ctx = j->coll->ctx;
    answer_box_t* box = (answer_box_t*)ctx_alloc(ctx, sizeof(*box));
    if (box == NULL) return LISA_E_NO_MEMORY;
    memset(box, 0, sizeof(*box));
    box->ctx = ctx;
    lisa_answer_t* a = &box->pub;

    const char* text = j->text ? j->text : "";
    a->text = ctx_strdup(ctx, text);
    a->found = st->n > 0 && !ctx_is_not_found(text);
    a->complete = !j->stopped;
    a->passages_retrieved = st->considered;
    a->passages_used = st->n;
    a->prompt_tokens = st->prompt_tokens;
    a->answer_tokens = j->answer_tokens;
    int64_t end = lisa_time_monotonic_ns();
    a->first_token_seconds = j->first_ns ? (double)(j->first_ns - j->t0_ns) / 1e9 : 0.0;
    a->total_seconds = (double)(end - j->t0_ns) / 1e9;
    int rc = a->text ? LISA_OK : LISA_E_NO_MEMORY;

    /* Cited passages; an answer that cites nothing relies on all of them. */
    int32_t* nums = NULL;
    int64_t nc = 0;
    if (rc == LISA_OK && a->found) {
        nums = (int32_t*)malloc((size_t)st->n * sizeof(int32_t));
        if (nums == NULL) rc = LISA_E_NO_MEMORY;
        else nc = ctx_parse_citations(text, st->n, nums, st->n);
        if (rc == LISA_OK && nc == 0) {
            for (int64_t i = 0; i < st->n; i++) nums[i] = (int32_t)(i + 1);
            nc = st->n;
        }
    }
    if (rc == LISA_OK && nc > 0) {
        a->citations = (lisa_citation_t*)ctx_alloc(ctx, (size_t)nc * sizeof(lisa_citation_t));
        if (a->citations == NULL) rc = LISA_E_NO_MEMORY;
        else memset(a->citations, 0, (size_t)nc * sizeof(lisa_citation_t));
    }
    for (int64_t i = 0; rc == LISA_OK && i < nc; i++) {
        const ctx_passage_t* p = &st->passages[nums[i] - 1];
        const lisa_store_chunk_t* chunk = NULL;
        for (int64_t k = 0; k < j->n_loaded && chunk == NULL; k++) {
            if (j->chunks[k].text == p->text) chunk = &j->chunks[k];
        }
        lisa_citation_t* c = &a->citations[i];
        a->citation_count = i + 1;
        c->number = nums[i];
        c->chunk_id = p->id;
        c->page = p->page;
        c->offset = p->offset;
        c->length = p->length;
        c->similarity = p->similarity;
        c->doc_id = ctx_strdup(ctx, p->doc_id);
        c->source_path = ctx_strdup(ctx, p->source_path);
        c->title = ctx_strdup(ctx, p->title ? p->title : "");
        c->quote = ctx_strdup(ctx, p->text);
        c->content_hash = ctx_strdup(ctx, chunk ? chunk->content_hash : "");
        if (!c->doc_id || !c->source_path || !c->title || !c->quote || !c->content_hash)
            rc = LISA_E_NO_MEMORY;
    }
    free(nums);
    if (rc != LISA_OK) {
        lisa_answer_free(a);
        return rc;
    }
    *out = a;
    return LISA_OK;
}

static int from_ctx(int rc, const ask_job_t* j) {
    switch (rc) {
    case CTX_OK:          return LISA_OK;
    case STAGE_API_ERROR: return j->status;
    case CTX_ENOMEM:      return LISA_E_NO_MEMORY;
    case CTX_ETOOLONG:    return LISA_E_TOO_LONG;
    case CTX_ECOUNT:      return j->status != LISA_OK ? j->status : LISA_E_INTERNAL;
    default:              return LISA_E_INTERNAL;
    }
}

int lisa_ask(lisa_collection_t* coll, lisa_model_t* embed_model, lisa_model_t* chat_model,
             const lisa_message_t* messages, int64_t count,
             const lisa_ask_options_t* options, lisa_answer_t** out) {
    if (out) *out = NULL;
    if (coll == NULL || embed_model == NULL || chat_model == NULL || messages == NULL ||
        count < 1 || out == NULL)
        return LISA_E_INVALID_ARGUMENT;
    const lisa_message_t* last = &messages[count - 1];
    if (last->role == NULL || strcmp(last->role, "user") != 0 || last->content == NULL ||
        last->content[0] == '\0')
        return LISA_E_INVALID_ARGUMENT;
    if (count > 1) return LISA_E_UNSUPPORTED;   /* follow-up chat: plan §8 L2 */

    ask_job_t j;
    memset(&j, 0, sizeof(j));
    int rc = read_options(options, &j.o);
    if (rc != LISA_OK) return rc;
    if (API_BUSY(coll) || API_BUSY(embed_model) || API_BUSY(chat_model)) return LISA_E_BUSY;
    j.coll = coll;
    j.embed = embed_model;
    j.chat = chat_model;
    j.question = last->content;
    j.t0_ns = lisa_time_monotonic_ns();

    ctx_state_t st;
    memset(&st, 0, sizeof(st));
    st.question = j.question;
    st.params.min_similarity = j.o.min_similarity;
    st.params.budget_tokens = j.o.budget;
    st.params.count = count_prompt;
    st.params.count_user = &j;
    st.user = &j;

    ctx_stage_t stages[6];
    stages[0] = (ctx_stage_t){ "retrieve", stage_retrieve };
    for (int i = 0; i < 4; i++) stages[1 + i] = ctx_default_stages[i];
    stages[5] = (ctx_stage_t){ "generate", stage_generate };

    rc = from_ctx(ctx_run(stages, 6, &st, NULL), &j);
    if (rc == LISA_OK) rc = make_answer(&j, &st, out);

    ctx_state_clear(&st);
    for (int64_t i = 0; i < j.n_loaded; i++) lisa_store_chunk_free(&j.chunks[i]);
    for (int64_t i = 0; j.titles && i < j.o.top_k; i++) free(j.titles[i]);
    free(j.chunks);
    free(j.titles);
    free(j.passages);
    free(j.text);
    return rc;
}
