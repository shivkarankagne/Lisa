/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_API_INTERNAL_H
#define LISA_API_INTERNAL_H

/*
 * Shared between the files implementing include/lisa.h. Not public.
 */

#include "lisa.h"

#include <stdatomic.h>
#include <string.h>

#include "../models/models.h"
#include "../storage/store.h"

struct lisa_context {
    lisa_allocator_t        alloc;
    int                     has_auth, has_filter, has_audit, has_crypto, has_routes;
    lisa_auth_provider_t    auth;
    lisa_retrieval_filter_t filter;
    lisa_audit_sink_t       audit;
    lisa_storage_crypto_t   crypto;
    lisa_http_routes_t      routes;
};

/* A struct field is present if the caller's struct_size covers it. */
#define HAS_FIELD(ptr, type, field) \
    ((ptr)->struct_size >= offsetof(type, field) + sizeof((ptr)->field))

static inline void* ctx_alloc(const lisa_context_t* ctx, size_t size) {
    return ctx->alloc.alloc(ctx->alloc.user, size ? size : 1);
}

static inline void ctx_free(const lisa_context_t* ctx, void* p) {
    if (p) ctx->alloc.free(ctx->alloc.user, p);
}

static inline char* ctx_strdup(const lisa_context_t* ctx, const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)ctx_alloc(ctx, n);
    if (d) memcpy(d, s, n);
    return d;
}

/*
 * A collection or model owned by a running ingest job is "busy": every
 * other call on it returns LISA_E_BUSY until the job ends.
 */
struct lisa_collection {
    lisa_context_t* ctx;
    lisa_store_t*   store;
    char*           path;
    int             mode;
    atomic_int      busy;
};

struct lisa_model {
    lisa_context_t* ctx;
    lm_model_t*     lm;
    atomic_int      busy;
};

#define API_BUSY(x) (atomic_load(&(x)->busy) != 0)

/* Internal status mapping (defined in lisa_api.c / lisa_models.c). */
int lisa_api_from_store(int rc);
int lisa_api_from_lm(int rc);

/* Emit an audit event if a sink is installed (lisa_api.c). */
void lisa_api_audit(const lisa_context_t* ctx, lisa_audit_action action, int status,
                    const lisa_principal_t* principal, const char* path,
                    int64_t count, const uint64_t* ids);

/* Embedding-model identity recorded in collections, and dim check (lisa_ingest.c). */
void lisa_api_model_id(const lisa_model_t* m, char* buf, size_t n);
int  lisa_api_check_dim(const lisa_model_t* m, int64_t dim);

#endif /* LISA_API_INTERNAL_H */
