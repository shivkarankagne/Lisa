/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Implementation of the public API (include/lisa.h).
 *
 * This layer owns: argument validation, the context and its allocator,
 * extension points, audit events, and translation of internal error codes
 * to lisa_status. Storage and search are delegated to src/storage and
 * src/retrieval.
 */

#include "lisa.h"

#include <stdlib.h>
#include <string.h>

#include "../retrieval/retrieval.h"
#include "../storage/store.h"

#define MAX_TOP_K 10000

/* A struct field is present if the caller's struct_size covers it. */
#define HAS_FIELD(ptr, type, field) \
    ((ptr)->struct_size >= offsetof(type, field) + sizeof((ptr)->field))

/* ==== Version and status ============================================== */

const char* lisa_version(int* major, int* minor, int* patch) {
    if (major) *major = LISA_VERSION_MAJOR;
    if (minor) *minor = LISA_VERSION_MINOR;
    if (patch) *patch = LISA_VERSION_PATCH;
    return LISA_VERSION_STRING;
}

const char* lisa_status_string(int status) {
    switch (status) {
    case LISA_OK:                 return "ok";
    case LISA_E_INVALID_ARGUMENT: return "invalid argument";
    case LISA_E_IO:               return "input/output error";
    case LISA_E_FORMAT:           return "unsupported or corrupt data format";
    case LISA_E_MODEL_MISMATCH:   return "collection was built with a different embedding model";
    case LISA_E_BUSY:             return "collection is open for writing elsewhere";
    case LISA_E_NOT_FOUND:        return "not found";
    case LISA_E_NO_MEMORY:        return "out of memory";
    case LISA_E_READ_ONLY:        return "collection is open read-only";
    case LISA_E_EXISTS:           return "already exists";
    case LISA_E_DENIED:           return "access denied";
    case LISA_E_UNSUPPORTED:      return "not supported in this build";
    case LISA_E_INTERNAL:         return "internal error";
    default:                      return "unknown status";
    }
}

static int from_store(int rc) {
    switch (rc) {
    case LISA_STORE_OK:        return LISA_OK;
    case LISA_STORE_EINVAL:    return LISA_E_INVALID_ARGUMENT;
    case LISA_STORE_EIO:       return LISA_E_IO;
    case LISA_STORE_EFORMAT:   return LISA_E_FORMAT;
    case LISA_STORE_EMODEL:    return LISA_E_MODEL_MISMATCH;
    case LISA_STORE_EBUSY:     return LISA_E_BUSY;
    case LISA_STORE_ENOTFOUND: return LISA_E_NOT_FOUND;
    case LISA_STORE_ENOMEM:    return LISA_E_NO_MEMORY;
    case LISA_STORE_EREADONLY: return LISA_E_READ_ONLY;
    case LISA_STORE_EEXIST:    return LISA_E_EXISTS;
    default:                   return LISA_E_INTERNAL;
    }
}

/* ==== Context ========================================================= */

struct lisa_context {
    lisa_allocator_t        alloc;
    int                     has_auth, has_filter, has_audit, has_crypto, has_routes;
    lisa_auth_provider_t    auth;
    lisa_retrieval_filter_t filter;
    lisa_audit_sink_t       audit;
    lisa_storage_crypto_t   crypto;
    lisa_http_routes_t      routes;
};

static void* sys_alloc(void* user, size_t size) {
    (void)user;
    return malloc(size);
}

static void* sys_realloc(void* user, void* p, size_t size) {
    (void)user;
    return realloc(p, size);
}

static void sys_free(void* user, void* p) {
    (void)user;
    free(p);
}

static void* ctx_alloc(const lisa_context_t* ctx, size_t size) {
    return ctx->alloc.alloc(ctx->alloc.user, size ? size : 1);
}

static void ctx_free(const lisa_context_t* ctx, void* p) {
    if (p) ctx->alloc.free(ctx->alloc.user, p);
}

static char* ctx_strdup(const lisa_context_t* ctx, const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)ctx_alloc(ctx, n);
    if (d) memcpy(d, s, n);
    return d;
}

/*
 * Copy a caller's extension struct into dst (sizeof dst_size), honouring
 * the caller's struct_size: fields the caller's version does not have
 * stay zero.
 */
static int copy_versioned(void* dst, size_t dst_size, const void* src) {
    size_t src_size = *(const size_t*)src;
    if (src_size < sizeof(size_t)) return LISA_E_INVALID_ARGUMENT;
    memset(dst, 0, dst_size);
    memcpy(dst, src, src_size < dst_size ? src_size : dst_size);
    *(size_t*)dst = dst_size;
    return LISA_OK;
}

int lisa_context_create(const lisa_context_config_t* config, lisa_context_t** out) {
    if (out == NULL) return LISA_E_INVALID_ARGUMENT;
    *out = NULL;
    if (config != NULL && config->struct_size < sizeof(size_t)) return LISA_E_INVALID_ARGUMENT;

    lisa_allocator_t alloc = { sys_alloc, sys_realloc, sys_free, NULL };
    if (config && HAS_FIELD(config, lisa_context_config_t, allocator) && config->allocator) {
        const lisa_allocator_t* a = config->allocator;
        if (!a->alloc || !a->realloc || !a->free) return LISA_E_INVALID_ARGUMENT;
        alloc = *a;
    }

    lisa_context_t* ctx = (lisa_context_t*)alloc.alloc(alloc.user, sizeof(*ctx));
    if (ctx == NULL) return LISA_E_NO_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;

    int rc = LISA_OK;
    if (config && HAS_FIELD(config, lisa_context_config_t, auth) && config->auth) {
        rc = copy_versioned(&ctx->auth, sizeof(ctx->auth), config->auth);
        if (rc == LISA_OK && !ctx->auth.authenticate) rc = LISA_E_INVALID_ARGUMENT;
        ctx->has_auth = rc == LISA_OK;
    }
    if (rc == LISA_OK && config && HAS_FIELD(config, lisa_context_config_t, retrieval_filter) &&
        config->retrieval_filter) {
        rc = copy_versioned(&ctx->filter, sizeof(ctx->filter), config->retrieval_filter);
        if (rc == LISA_OK && !ctx->filter.filter) rc = LISA_E_INVALID_ARGUMENT;
        ctx->has_filter = rc == LISA_OK;
    }
    if (rc == LISA_OK && config && HAS_FIELD(config, lisa_context_config_t, audit) && config->audit) {
        rc = copy_versioned(&ctx->audit, sizeof(ctx->audit), config->audit);
        if (rc == LISA_OK && !ctx->audit.record) rc = LISA_E_INVALID_ARGUMENT;
        ctx->has_audit = rc == LISA_OK;
    }
    if (rc == LISA_OK && config && HAS_FIELD(config, lisa_context_config_t, storage_crypto) &&
        config->storage_crypto) {
        rc = copy_versioned(&ctx->crypto, sizeof(ctx->crypto), config->storage_crypto);
        if (rc == LISA_OK && (!ctx->crypto.encrypt || !ctx->crypto.decrypt)) rc = LISA_E_INVALID_ARGUMENT;
        ctx->has_crypto = rc == LISA_OK;
    }
    if (rc == LISA_OK && config && HAS_FIELD(config, lisa_context_config_t, http_routes) &&
        config->http_routes) {
        rc = copy_versioned(&ctx->routes, sizeof(ctx->routes), config->http_routes);
        if (rc == LISA_OK && !ctx->routes.handle) rc = LISA_E_INVALID_ARGUMENT;
        ctx->has_routes = rc == LISA_OK;
    }

    if (rc != LISA_OK) {
        alloc.free(alloc.user, ctx);
        return rc;
    }
    *out = ctx;
    return LISA_OK;
}

void lisa_context_destroy(lisa_context_t* ctx) {
    if (ctx == NULL) return;
    lisa_allocator_t a = ctx->alloc;
    a.free(a.user, ctx);
}

const lisa_auth_provider_t* lisa_context_auth(const lisa_context_t* ctx) {
    return (ctx && ctx->has_auth) ? &ctx->auth : NULL;
}

const lisa_http_routes_t* lisa_context_http_routes(const lisa_context_t* ctx) {
    return (ctx && ctx->has_routes) ? &ctx->routes : NULL;
}

/* ==== Audit =========================================================== */

static void audit(const lisa_context_t* ctx, lisa_audit_action action, int status,
                  const lisa_principal_t* principal, const char* path,
                  int64_t count, const uint64_t* ids) {
    if (!ctx->has_audit) return;
    lisa_audit_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = sizeof(ev);
    ev.action = action;
    ev.status = status;
    ev.principal = principal;
    ev.collection_path = path;
    ev.item_count = count;
    ev.chunk_ids = ids;
    ctx->audit.record(ctx->audit.user, &ev);
}

/* ==== Collections ===================================================== */

struct lisa_collection {
    lisa_context_t* ctx;
    lisa_store_t*   store;
    char*           path;
};

int lisa_collection_create(lisa_context_t* ctx, const char* path,
                           const char* embedding_model, int64_t dim) {
    if (ctx == NULL || path == NULL || embedding_model == NULL) return LISA_E_INVALID_ARGUMENT;
    int rc = ctx->has_crypto ? LISA_E_UNSUPPORTED
                             : from_store(lisa_store_create(path, embedding_model, dim));
    audit(ctx, LISA_AUDIT_COLLECTION_CREATE, rc, NULL, path, 0, NULL);
    return rc;
}

int lisa_collection_open(lisa_context_t* ctx, const char* path, lisa_open_mode mode,
                         const char* expected_model, lisa_collection_t** out) {
    if (out == NULL) return LISA_E_INVALID_ARGUMENT;
    *out = NULL;
    if (ctx == NULL || path == NULL) return LISA_E_INVALID_ARGUMENT;
    if (mode != LISA_OPEN_READ && mode != LISA_OPEN_WRITE) return LISA_E_INVALID_ARGUMENT;

    int rc = LISA_OK;
    lisa_collection_t* c = NULL;
    if (ctx->has_crypto) rc = LISA_E_UNSUPPORTED;
    if (rc == LISA_OK) {
        c = (lisa_collection_t*)ctx_alloc(ctx, sizeof(*c));
        if (c == NULL) rc = LISA_E_NO_MEMORY;
    }
    if (rc == LISA_OK) {
        memset(c, 0, sizeof(*c));
        c->ctx = ctx;
        c->path = ctx_strdup(ctx, path);
        if (c->path == NULL) rc = LISA_E_NO_MEMORY;
    }
    if (rc == LISA_OK) {
        int smode = mode == LISA_OPEN_WRITE ? LISA_STORE_WRITE : LISA_STORE_READ;
        rc = from_store(lisa_store_open(path, smode, expected_model, &c->store));
    }
    audit(ctx, LISA_AUDIT_COLLECTION_OPEN, rc, NULL, path, 0, NULL);
    if (rc != LISA_OK) {
        lisa_collection_close(c);
        return rc;
    }
    *out = c;
    return LISA_OK;
}

void lisa_collection_close(lisa_collection_t* c) {
    if (c == NULL) return;
    lisa_store_close(c->store);
    ctx_free(c->ctx, c->path);
    ctx_free(c->ctx, c);
}

int lisa_collection_info(const lisa_collection_t* c, lisa_collection_info_t* info) {
    if (c == NULL || info == NULL || info->struct_size < sizeof(size_t))
        return LISA_E_INVALID_ARGUMENT;
    if (HAS_FIELD(info, lisa_collection_info_t, embedding_model))
        info->embedding_model = lisa_store_model(c->store);
    if (HAS_FIELD(info, lisa_collection_info_t, dim)) info->dim = lisa_store_dim(c->store);
    if (HAS_FIELD(info, lisa_collection_info_t, chunk_count))
        info->chunk_count = lisa_store_count(c->store);
    return LISA_OK;
}

int lisa_collection_refresh(lisa_collection_t* c) {
    if (c == NULL) return LISA_E_INVALID_ARGUMENT;
    return from_store(lisa_store_refresh(c->store));
}

int lisa_collection_add(lisa_collection_t* c, int64_t count, const float* vectors,
                        const lisa_chunk_t* chunks, uint64_t* out_ids) {
    if (c == NULL || count <= 0 || vectors == NULL || chunks == NULL)
        return LISA_E_INVALID_ARGUMENT;

    lisa_store_chunk_t* sc =
        (lisa_store_chunk_t*)ctx_alloc(c->ctx, (size_t)count * sizeof(*sc));
    uint64_t* ids = out_ids;
    if (ids == NULL && c->ctx->has_audit)
        ids = (uint64_t*)ctx_alloc(c->ctx, (size_t)count * sizeof(uint64_t));
    int rc = (sc == NULL || (c->ctx->has_audit && ids == NULL)) ? LISA_E_NO_MEMORY : LISA_OK;

    for (int64_t i = 0; rc == LISA_OK && i < count; i++) {
        /* The store copies the strings; it never writes through them. */
        sc[i].doc_id = (char*)chunks[i].doc_id;
        sc[i].chunk_index = chunks[i].chunk_index;
        sc[i].source_path = (char*)chunks[i].source_path;
        sc[i].offset = chunks[i].offset;
        sc[i].length = chunks[i].length;
        sc[i].text = (char*)chunks[i].text;
        sc[i].content_hash = (char*)chunks[i].content_hash;
    }
    if (rc == LISA_OK) rc = from_store(lisa_store_insert(c->store, count, vectors, sc, ids));
    audit(c->ctx, LISA_AUDIT_CHUNKS_ADD, rc, NULL, c->path,
          rc == LISA_OK ? count : 0, rc == LISA_OK ? ids : NULL);

    ctx_free(c->ctx, sc);
    if (ids != out_ids) ctx_free(c->ctx, ids);
    return rc;
}

int lisa_collection_remove(lisa_collection_t* c, int64_t count, const uint64_t* ids) {
    if (c == NULL || count <= 0 || ids == NULL) return LISA_E_INVALID_ARGUMENT;
    int rc = from_store(lisa_store_delete(c->store, count, ids));
    audit(c->ctx, LISA_AUDIT_CHUNKS_REMOVE, rc, NULL, c->path,
          rc == LISA_OK ? count : 0, rc == LISA_OK ? ids : NULL);
    return rc;
}

int lisa_collection_remove_document(lisa_collection_t* c, const char* doc_id,
                                    int64_t* out_removed) {
    if (out_removed) *out_removed = 0;
    if (c == NULL || doc_id == NULL) return LISA_E_INVALID_ARGUMENT;
    int64_t n = 0;
    int rc = from_store(lisa_store_delete_doc(c->store, doc_id, &n));
    audit(c->ctx, LISA_AUDIT_CHUNKS_REMOVE, rc, NULL, c->path, rc == LISA_OK ? n : 0, NULL);
    if (rc == LISA_OK && out_removed) *out_removed = n;
    return rc;
}

/* A returned chunk: the public struct plus the context that allocated it. */
typedef struct {
    const lisa_context_t* ctx;
    lisa_chunk_t          chunk;
    /* strings follow */
} chunk_block_t;

int lisa_collection_get_chunk(lisa_collection_t* c, uint64_t id, lisa_chunk_t** out) {
    if (out == NULL) return LISA_E_INVALID_ARGUMENT;
    *out = NULL;
    if (c == NULL) return LISA_E_INVALID_ARGUMENT;

    lisa_store_chunk_t sc;
    int rc = from_store(lisa_store_get(c->store, id, &sc));
    if (rc == LISA_OK) {
        size_t l1 = strlen(sc.doc_id) + 1, l2 = strlen(sc.source_path) + 1;
        size_t l3 = strlen(sc.text) + 1, l4 = strlen(sc.content_hash) + 1;
        chunk_block_t* b = (chunk_block_t*)ctx_alloc(c->ctx, sizeof(*b) + l1 + l2 + l3 + l4);
        if (b == NULL) {
            rc = LISA_E_NO_MEMORY;
        } else {
            char* p = (char*)(b + 1);
            b->ctx = c->ctx;
            b->chunk.id = id;
            b->chunk.doc_id = memcpy(p, sc.doc_id, l1);            p += l1;
            b->chunk.source_path = memcpy(p, sc.source_path, l2);  p += l2;
            b->chunk.text = memcpy(p, sc.text, l3);                p += l3;
            b->chunk.content_hash = memcpy(p, sc.content_hash, l4);
            b->chunk.chunk_index = sc.chunk_index;
            b->chunk.offset = sc.offset;
            b->chunk.length = sc.length;
            *out = &b->chunk;
        }
        lisa_store_chunk_free(&sc);
    }
    audit(c->ctx, LISA_AUDIT_CHUNK_READ, rc, NULL, c->path, rc == LISA_OK ? 1 : 0,
          rc == LISA_OK ? &id : NULL);
    return rc;
}

void lisa_chunk_free(lisa_chunk_t* chunk) {
    if (chunk == NULL) return;
    chunk_block_t* b = (chunk_block_t*)((char*)chunk - offsetof(chunk_block_t, chunk));
    ctx_free(b->ctx, b);
}

int lisa_collection_compact(lisa_collection_t* c) {
    if (c == NULL) return LISA_E_INVALID_ARGUMENT;
    int rc = from_store(lisa_store_compact(c->store));
    audit(c->ctx, LISA_AUDIT_COMPACT, rc, NULL, c->path, 0, NULL);
    return rc;
}

/* ==== Search ========================================================== */

static int search_vector(lisa_collection_t* c, const float* query, int64_t top_k,
                         const lisa_principal_t* principal, lisa_hit_t* hits,
                         int64_t capacity, int64_t* out_count, uint64_t** out_ids) {
    const lisa_context_t* ctx = c->ctx;
    lisa_store_view_t v;
    int rc = from_store(lisa_store_view(c->store, &v));
    if (rc != LISA_OK) return rc;
    int64_t k = top_k < capacity ? top_k : capacity;
    if (v.n_slots == 0 || k == 0) return LISA_OK;
    if (v.n_slots > INT32_MAX) return LISA_E_UNSUPPORTED;

    const uint8_t* mask = v.live;
    uint8_t* allowed = NULL;
    if (ctx->has_filter) {
        allowed = (uint8_t*)ctx_alloc(ctx, (size_t)v.n_slots);
        if (allowed == NULL) return LISA_E_NO_MEMORY;
        memcpy(allowed, v.live, (size_t)v.n_slots);
        lisa_filter_input_t in;
        memset(&in, 0, sizeof(in));
        in.struct_size = sizeof(in);
        in.collection_path = c->path;
        in.n_slots = v.n_slots;
        in.chunk_ids = v.slot_ids;
        rc = ctx->filter.filter(ctx->filter.user, principal, &in, allowed);
        if (rc > 0) rc = LISA_E_INTERNAL;
        /* A filter may only remove candidates. */
        for (int64_t i = 0; rc == LISA_OK && i < v.n_slots; i++) {
            if (allowed[i] && !v.live[i]) allowed[i] = 0;
        }
        mask = allowed;
    }

    int* idx = NULL;
    float* dist = NULL;
    uint64_t* ids = NULL;
    if (rc == LISA_OK) {
        idx = (int*)ctx_alloc(ctx, (size_t)k * sizeof(int));
        dist = (float*)ctx_alloc(ctx, (size_t)k * sizeof(float));
        ids = (uint64_t*)ctx_alloc(ctx, (size_t)k * sizeof(uint64_t));
        if (!idx || !dist || !ids) rc = LISA_E_NO_MEMORY;
    }
    if (rc == LISA_OK) {
        lisa_result_t r = { idx, dist, (int)k, 0 };
        int src = lisa_search_masked(query, v.vectors, v.n_slots, v.dim, k, mask, &r);
        if (src == -4) rc = LISA_E_NO_MEMORY;
        else if (src != 0) rc = LISA_E_INTERNAL;
        for (int i = 0; rc == LISA_OK && i < r.n_returned; i++) {
            hits[i].id = v.slot_ids[idx[i]];
            hits[i].distance = dist[i];
            ids[i] = hits[i].id;
        }
        if (rc == LISA_OK) *out_count = r.n_returned;
    }
    ctx_free(ctx, allowed);
    ctx_free(ctx, idx);
    ctx_free(ctx, dist);
    if (rc == LISA_OK) *out_ids = ids;
    else ctx_free(ctx, ids);
    return rc;
}

int lisa_collection_search_vector(lisa_collection_t* c, const float* query,
                                  const lisa_search_options_t* options,
                                  lisa_hit_t* hits, int64_t capacity, int64_t* out_count) {
    if (out_count) *out_count = 0;
    if (c == NULL || query == NULL || out_count == NULL || capacity < 0 ||
        (capacity > 0 && hits == NULL))
        return LISA_E_INVALID_ARGUMENT;

    int64_t top_k = 5;
    const lisa_principal_t* principal = NULL;
    if (options != NULL) {
        if (options->struct_size < sizeof(size_t)) return LISA_E_INVALID_ARGUMENT;
        if (HAS_FIELD(options, lisa_search_options_t, top_k)) top_k = options->top_k;
        if (HAS_FIELD(options, lisa_search_options_t, principal)) principal = options->principal;
    }
    if (top_k < 1 || top_k > MAX_TOP_K) return LISA_E_INVALID_ARGUMENT;

    uint64_t* ids = NULL;
    int rc = search_vector(c, query, top_k, principal, hits, capacity, out_count, &ids);
    audit(c->ctx, LISA_AUDIT_SEARCH, rc, principal, c->path, rc == LISA_OK ? *out_count : 0,
          rc == LISA_OK ? ids : NULL);
    ctx_free(c->ctx, ids);
    return rc;
}
