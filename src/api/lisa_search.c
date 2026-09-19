/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Hybrid search (include/lisa.h, "Hybrid search"): vector search through
 * the index seam, keyword search through the collection's FTS5 index,
 * both restricted by the same allowed-slot mask, fused with RRF.
 */

#include "api_internal.h"

#include <stdlib.h>
#include <string.h>

#include "../retrieval/fusion.h"
#include "../retrieval/index.h"

#define MAX_TOP_K      10000
#define MIN_CANDIDATES 50
#define MAX_CANDIDATES 2000

/*
 * Slots a query may return: live in this handle's view, under the path
 * prefix (if any), and allowed by the retrieval filter (if installed).
 * *out is ctx-allocated (n_slots bytes); NULL when n_slots == 0.
 */
static int allowed_mask(lisa_collection_t* c, const lisa_store_view_t* v, const char* prefix,
                        const lisa_principal_t* principal, uint8_t** out) {
    const lisa_context_t* ctx = c->ctx;
    *out = NULL;
    if (v->n_slots == 0) return LISA_OK;
    uint8_t* m = (uint8_t*)ctx_alloc(ctx, (size_t)v->n_slots);
    if (m == NULL) return LISA_E_NO_MEMORY;
    int rc = LISA_OK;
    if (prefix) {
        rc = lisa_api_from_store(lisa_store_prefix_mask(c->store, prefix, m));
    } else {
        memcpy(m, v->live, (size_t)v->n_slots);
    }
    if (rc == LISA_OK && ctx->has_filter) {
        lisa_filter_input_t in;
        memset(&in, 0, sizeof(in));
        in.struct_size = sizeof(in);
        in.collection_path = c->path;
        in.n_slots = v->n_slots;
        in.chunk_ids = v->slot_ids;
        rc = ctx->filter.filter(ctx->filter.user, principal, &in, m);
        if (rc > 0) rc = LISA_E_INTERNAL;
    }
    /* A filter may only remove candidates. */
    for (int64_t i = 0; rc == LISA_OK && i < v->n_slots; i++) {
        if (m[i] && !v->live[i]) m[i] = 0;
    }
    if (rc != LISA_OK) {
        ctx_free(ctx, m);
        return rc;
    }
    *out = m;
    return LISA_OK;
}

static int run_query(lisa_collection_t* c, const lisa_query_t* q, int64_t top_k,
                     lisa_scored_hit_t* hits, int64_t capacity, int64_t* out_count,
                     uint64_t** out_ids) {
    const lisa_context_t* ctx = c->ctx;
    int use_vec = q->vector != NULL && q->mode != LISA_SEARCH_KEYWORD;
    int use_kw = q->text != NULL && q->mode != LISA_SEARCH_VECTOR;
    if (q->mode == LISA_SEARCH_VECTOR && q->vector == NULL) return LISA_E_INVALID_ARGUMENT;
    if (q->mode == LISA_SEARCH_KEYWORD && q->text == NULL) return LISA_E_INVALID_ARGUMENT;
    if (!use_vec && !use_kw) return LISA_E_INVALID_ARGUMENT;

    lisa_store_view_t v;
    int rc = lisa_api_from_store(lisa_store_view(c->store, &v));
    if (rc != LISA_OK) return rc;
    int64_t k = top_k < capacity ? top_k : capacity;
    if (v.n_slots == 0 || k == 0) return LISA_OK;
    if (v.n_slots > INT32_MAX) return LISA_E_UNSUPPORTED;

    int64_t pool = top_k * 4;
    if (pool < MIN_CANDIDATES) pool = MIN_CANDIDATES;
    if (pool > MAX_CANDIDATES) pool = MAX_CANDIDATES;

    uint8_t* mask = NULL;
    rc = allowed_mask(c, &v, q->path_prefix, q->principal, &mask);
    if (rc != LISA_OK) return rc;

    /* Vector candidates. */
    uint64_t* vec_ids = NULL;
    float* vec_dist = NULL;
    int64_t nv = 0;
    if (use_vec) {
        int* idx = (int*)ctx_alloc(ctx, (size_t)pool * sizeof(int));
        vec_dist = (float*)ctx_alloc(ctx, (size_t)pool * sizeof(float));
        vec_ids = (uint64_t*)ctx_alloc(ctx, (size_t)pool * sizeof(uint64_t));
        if (!idx || !vec_dist || !vec_ids) rc = LISA_E_NO_MEMORY;
        if (rc == LISA_OK) {
            lisa_result_t r = { idx, vec_dist, (int)pool, 0 };
            int src = lisa_index_exact()->search(q->vector, v.vectors, v.n_slots, v.dim, pool, mask, &r);
            if (src == -4) rc = LISA_E_NO_MEMORY;
            else if (src != 0) rc = LISA_E_INTERNAL;
            for (int i = 0; rc == LISA_OK && i < r.n_returned; i++) vec_ids[i] = v.slot_ids[idx[i]];
            nv = rc == LISA_OK ? r.n_returned : 0;
        }
        ctx_free(ctx, idx);
    }

    /* Keyword candidates, restricted to the same allowed slots. */
    uint64_t* kw_ids = NULL;
    double* kw_score = NULL;
    int64_t nk = 0;
    if (rc == LISA_OK && use_kw) {
        int64_t want = pool * 2;
        lisa_store_kw_hit_t* raw = (lisa_store_kw_hit_t*)ctx_alloc(ctx, (size_t)want * sizeof(*raw));
        kw_ids = (uint64_t*)ctx_alloc(ctx, (size_t)pool * sizeof(uint64_t));
        kw_score = (double*)ctx_alloc(ctx, (size_t)pool * sizeof(double));
        if (!raw || !kw_ids || !kw_score) rc = LISA_E_NO_MEMORY;
        int64_t nraw = 0;
        if (rc == LISA_OK)
            rc = lisa_api_from_store(lisa_store_keyword_search(c->store, q->text, q->path_prefix,
                                                               want, raw, &nraw));
        for (int64_t i = 0; rc == LISA_OK && i < nraw && nk < pool; i++) {
            int64_t s = raw[i].slot;
            /* Skip chunks this handle's view does not have, or may not return. */
            if (s < 0 || s >= v.n_slots || !mask[s] || v.slot_ids[s] != raw[i].id) continue;
            kw_ids[nk] = raw[i].id;
            kw_score[nk] = raw[i].score;
            nk++;
        }
        ctx_free(ctx, raw);
    }

    /* Fuse and write out. */
    uint64_t* ids = NULL;
    if (rc == LISA_OK) {
        lisa_fused_t* f = (lisa_fused_t*)ctx_alloc(ctx, (size_t)k * sizeof(lisa_fused_t));
        ids = (uint64_t*)ctx_alloc(ctx, (size_t)k * sizeof(uint64_t));
        if (!f || !ids) rc = LISA_E_NO_MEMORY;
        int64_t n = 0;
        if (rc == LISA_OK) {
            double wv = q->vector_weight, wk = q->keyword_weight;
            n = lisa_rrf_fuse(vec_ids, nv, wv, kw_ids, nk, wk, f, k);
            if (n < 0) rc = LISA_E_NO_MEMORY;
        }
        for (int64_t i = 0; rc == LISA_OK && i < n; i++) {
            hits[i].id = f[i].id;
            hits[i].score = f[i].score;
            hits[i].vector_rank = f[i].rank_a;
            hits[i].keyword_rank = f[i].rank_b;
            hits[i].distance = f[i].rank_a ? vec_dist[f[i].rank_a - 1] : -1.0f;
            hits[i].keyword_score = f[i].rank_b ? kw_score[f[i].rank_b - 1] : 0.0;
            ids[i] = f[i].id;
        }
        if (rc == LISA_OK) *out_count = n;
        ctx_free(ctx, f);
    }

    ctx_free(ctx, mask);
    ctx_free(ctx, vec_ids);
    ctx_free(ctx, vec_dist);
    ctx_free(ctx, kw_ids);
    ctx_free(ctx, kw_score);
    if (rc == LISA_OK) *out_ids = ids;
    else ctx_free(ctx, ids);
    return rc;
}

/* Copy the fields the caller's lisa_query_t version has over the defaults. */
static int read_query(const lisa_query_t* in, lisa_query_t* q) {
    lisa_query_t d = LISA_QUERY_INIT;
    *q = d;
    if (in == NULL) return LISA_OK;
    if (in->struct_size < sizeof(size_t)) return LISA_E_INVALID_ARGUMENT;
    if (HAS_FIELD(in, lisa_query_t, text)) q->text = in->text;
    if (HAS_FIELD(in, lisa_query_t, vector)) q->vector = in->vector;
    if (HAS_FIELD(in, lisa_query_t, mode)) q->mode = in->mode;
    if (HAS_FIELD(in, lisa_query_t, top_k)) q->top_k = in->top_k;
    if (HAS_FIELD(in, lisa_query_t, vector_weight)) q->vector_weight = in->vector_weight;
    if (HAS_FIELD(in, lisa_query_t, keyword_weight)) q->keyword_weight = in->keyword_weight;
    if (HAS_FIELD(in, lisa_query_t, path_prefix)) q->path_prefix = in->path_prefix;
    if (HAS_FIELD(in, lisa_query_t, principal)) q->principal = in->principal;
    if (q->mode != LISA_SEARCH_HYBRID && q->mode != LISA_SEARCH_VECTOR && q->mode != LISA_SEARCH_KEYWORD)
        return LISA_E_INVALID_ARGUMENT;
    if (q->top_k < 1 || q->top_k > MAX_TOP_K) return LISA_E_INVALID_ARGUMENT;
    if (!(q->vector_weight >= 0) || !(q->keyword_weight >= 0)) return LISA_E_INVALID_ARGUMENT;
    return LISA_OK;
}

int lisa_collection_query(lisa_collection_t* c, const lisa_query_t* query,
                          lisa_scored_hit_t* hits, int64_t capacity, int64_t* out_count) {
    if (out_count) *out_count = 0;
    if (c == NULL || query == NULL || out_count == NULL || capacity < 0 ||
        (capacity > 0 && hits == NULL))
        return LISA_E_INVALID_ARGUMENT;
    lisa_query_t q;
    int rc = read_query(query, &q);
    if (rc != LISA_OK) return rc;
    if (API_BUSY(c)) return LISA_E_BUSY;

    uint64_t* ids = NULL;
    rc = run_query(c, &q, q.top_k, hits, capacity, out_count, &ids);
    lisa_api_audit(c->ctx, LISA_AUDIT_SEARCH, rc, q.principal, c->path,
                   rc == LISA_OK ? *out_count : 0, rc == LISA_OK ? ids : NULL);
    ctx_free(c->ctx, ids);
    return rc;
}

int lisa_collection_query_text(lisa_collection_t* c, lisa_model_t* model, const char* text,
                               const lisa_query_t* options, lisa_scored_hit_t* hits,
                               int64_t capacity, int64_t* out_count) {
    if (out_count) *out_count = 0;
    if (c == NULL || model == NULL || text == NULL) return LISA_E_INVALID_ARGUMENT;
    lisa_query_t q;
    int rc = read_query(options, &q);
    if (rc != LISA_OK) return rc;
    if (API_BUSY(c) || API_BUSY(model)) return LISA_E_BUSY;

    char id[256];
    lisa_api_model_id(model, id, sizeof(id));
    int64_t dim = lisa_store_dim(c->store);
    if (strcmp(id, lisa_store_model(c->store)) != 0 || lisa_api_check_dim(model, dim) != LISA_OK)
        return LISA_E_MODEL_MISMATCH;

    float* vec = (float*)ctx_alloc(c->ctx, (size_t)dim * sizeof(float));
    if (vec == NULL) return LISA_E_NO_MEMORY;
    const char* texts[1] = { text };
    rc = lisa_api_from_lm(lm_embed(model->lm, LM_EMBED_QUERY, texts, 1, vec, dim));
    if (rc == LISA_OK) {
        q.text = text;
        q.vector = vec;
        if (q.mode != LISA_SEARCH_KEYWORD && q.mode != LISA_SEARCH_VECTOR) q.mode = LISA_SEARCH_HYBRID;
        rc = lisa_collection_query(c, &q, hits, capacity, out_count);
    }
    ctx_free(c->ctx, vec);
    return rc;
}
