/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LISA_API_INTERNAL_H
#define LISA_API_INTERNAL_H

/*
 * Shared between the files implementing include/lisa.h. Not public.
 */

#include "lisa.h"

#include <string.h>

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

#endif /* LISA_API_INTERNAL_H */
