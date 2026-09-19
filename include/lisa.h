/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_H
#define LISA_H

/*
 * LISA public API.
 *
 * This is the only public header. Everything a program (the lisa CLI,
 * HTTP server, GUI, SDKs, or the enterprise edition) does with LISA goes
 * through the functions declared here.
 *
 * Conventions
 * -----------
 * - Functions return an int status: LISA_OK (0) or a negative lisa_status
 *   code. lisa_status_string() describes any code.
 * - Handles are opaque. Every handle returned through an out-parameter is
 *   owned by the caller and must be released with the matching
 *   ..._close / ..._destroy / ..._free function. On error, out-parameters
 *   are set to NULL (or 0) and nothing needs releasing.
 * - Strings passed in are UTF-8, NUL-terminated, and only read during the
 *   call unless stated otherwise. Strings returned by accessors are owned
 *   by the handle and valid until it is closed.
 * - Sizes, counts, offsets, and IDs are 64-bit.
 * - Structs passed in begin with `size_t struct_size`, set to
 *   sizeof(the struct). New fields are only ever appended, so programs
 *   built against an older header keep working (plan §7, upgrade rule 2).
 *   Use the ..._INIT macros to get correct defaults.
 * - Threading: a lisa_context_t may be shared by threads. Every other
 *   handle must be used by one thread at a time; open one handle per
 *   thread instead of sharing.
 *
 * Versioning
 * ----------
 * Semantic versioning. Before 1.0 (major version 0), minor versions may
 * change the API; from 1.0 on, only a major version may.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(LISA_BUILD_SHARED)
#  define LISA_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define LISA_API __attribute__((visibility("default")))
#else
#  define LISA_API
#endif

/* ==== Version ========================================================= */

#define LISA_VERSION_MAJOR 0
#define LISA_VERSION_MINOR 1
#define LISA_VERSION_PATCH 0
#define LISA_VERSION_STRING "0.1.0"

/*
 * Version of the linked library (may differ from the header's macros if
 * a program is linked against another build). Any out pointer may be NULL.
 * Returns the version string, e.g. "0.1.0".
 */
LISA_API const char* lisa_version(int* major, int* minor, int* patch);

/* ==== Status codes ==================================================== */

/* Values are stable: existing codes never change meaning. */
typedef enum lisa_status {
    LISA_OK                  =   0,
    LISA_E_INVALID_ARGUMENT  =  -1,
    LISA_E_IO                =  -2,  /* file, database, or OS error */
    LISA_E_FORMAT            =  -3,  /* data not in a supported format, or corrupt */
    LISA_E_MODEL_MISMATCH    =  -4,  /* collection built with a different model */
    LISA_E_BUSY              =  -5,  /* another writer holds the collection */
    LISA_E_NOT_FOUND         =  -6,
    LISA_E_NO_MEMORY         =  -7,
    LISA_E_READ_ONLY         =  -8,  /* write on a read-only handle */
    LISA_E_EXISTS            =  -9,
    LISA_E_DENIED            = -10,  /* refused by an extension (auth, filter) */
    LISA_E_UNSUPPORTED       = -11,  /* feature not available in this build */
    LISA_E_INTERNAL          = -12,  /* bug: please report */
    LISA_E_CANCELLED         = -13,  /* stopped by a progress callback */
    LISA_E_TOO_LONG          = -14,  /* input does not fit the model's context window */
    LISA_E_WRONG_MODEL_KIND  = -15   /* e.g. embedding with a generation model */
} lisa_status;

/* Human-readable description of a status code. Never NULL. */
LISA_API const char* lisa_status_string(int status);

/* ==== Memory ========================================================== */

/*
 * Allocator used for memory LISA returns to the caller and for its API
 * objects. All three functions are required. `user` is passed back
 * unchanged. The allocator must be thread-safe if the context is shared
 * between threads.
 *
 * Scope: internal subsystems and vendored libraries (SQLite, llama.cpp)
 * currently allocate from the system allocator.
 */
typedef struct lisa_allocator {
    void* (*alloc)(void* user, size_t size);
    void* (*realloc)(void* user, void* ptr, size_t new_size);
    void  (*free)(void* user, void* ptr);
    void* user;
} lisa_allocator_t;

/* ==== Identity ======================================================== */

/*
 * The person or service on whose behalf an operation runs. Produced by
 * the auth extension (or by the program itself); passed to the retrieval
 * filter and audit sink. NULL means "local, unauthenticated".
 */
typedef struct lisa_principal {
    const char*        user_id;
    const char* const* groups;
    int64_t            group_count;
} lisa_principal_t;

/* ==== Extension points ================================================ */
/*
 * Extensions let another build (e.g. the enterprise edition) add
 * behaviour without changing the core. Each is optional; with none
 * installed LISA behaves as a single-user local tool. Callbacks may be
 * called from any thread that uses the context and must be thread-safe.
 */

/* --- Authentication (used by the HTTP server, W9) --- */

typedef struct lisa_auth_request {
    size_t             struct_size;
    const char*        method;        /* e.g. "POST" */
    const char*        path;          /* e.g. "/v1/collections" */
    const char* const* header_names;  /* header_count entries */
    const char* const* header_values;
    int64_t            header_count;
    const char*        remote_addr;
} lisa_auth_request_t;

typedef struct lisa_auth_provider {
    size_t struct_size;
    void*  user;
    /*
     * Decide who is making the request. On LISA_OK, fill *out; strings
     * must stay valid until release() is called for it. Return
     * LISA_E_DENIED to reject the request.
     */
    int  (*authenticate)(void* user, const lisa_auth_request_t* request,
                         lisa_principal_t* out);
    /* Release what authenticate() put in *principal. May be NULL. */
    void (*release)(void* user, lisa_principal_t* principal);
} lisa_auth_provider_t;

/* --- Retrieval filter (called on every search) --- */

typedef struct lisa_filter_input {
    size_t          struct_size;
    const char*     collection_path;
    int64_t         n_slots;
    const uint64_t* chunk_ids;       /* n_slots entries; meaningful where allowed[i] != 0 */
} lisa_filter_input_t;

typedef struct lisa_retrieval_filter {
    size_t struct_size;
    void*  user;
    /*
     * Restrict a search. allowed[i] != 0 on entry means chunk_ids[i] is a
     * candidate; set allowed[i] = 0 to hide it from `principal` (may be
     * NULL). Never set a 0 entry to non-zero. Return LISA_OK, or an error
     * to fail the search.
     */
    int (*filter)(void* user, const lisa_principal_t* principal,
                  const lisa_filter_input_t* input, uint8_t* allowed);
} lisa_retrieval_filter_t;

/* --- Audit (called after every operation on a collection) --- */

typedef enum lisa_audit_action {
    LISA_AUDIT_COLLECTION_CREATE = 1,
    LISA_AUDIT_COLLECTION_OPEN   = 2,
    LISA_AUDIT_CHUNKS_ADD        = 3,
    LISA_AUDIT_CHUNKS_REMOVE     = 4,
    LISA_AUDIT_CHUNK_READ        = 5,
    LISA_AUDIT_SEARCH            = 6,
    LISA_AUDIT_COMPACT           = 7
} lisa_audit_action;

typedef struct lisa_audit_event {
    size_t                  struct_size;
    lisa_audit_action       action;
    int                     status;          /* result of the operation */
    const lisa_principal_t* principal;       /* NULL: local */
    const char*             collection_path;
    int64_t                 item_count;      /* chunks added/removed/read, or hits */
    const uint64_t*         chunk_ids;       /* item_count IDs, or NULL */
} lisa_audit_event_t;

typedef struct lisa_audit_sink {
    size_t struct_size;
    void*  user;
    /* Record one event. Pointers are valid only during the call. The sink
     * adds its own timestamp. Must not call back into LISA. */
    void (*record)(void* user, const lisa_audit_event_t* event);
} lisa_audit_sink_t;

/* --- Storage encryption (reserved; E3) --- */

typedef struct lisa_storage_crypto {
    size_t struct_size;
    void*  user;
    int (*encrypt)(void* user, const void* in, size_t len, void* out, uint64_t nonce);
    int (*decrypt)(void* user, const void* in, size_t len, void* out, uint64_t nonce);
} lisa_storage_crypto_t;
/*
 * NOTE: the storage layer does not yet encrypt. If a context has a
 * storage_crypto installed, lisa_collection_create/open return
 * LISA_E_UNSUPPORTED rather than storing data unencrypted.
 */

/* --- Extra HTTP routes (used by the HTTP server, W9) --- */

typedef struct lisa_http_request {
    size_t                  struct_size;
    const char*             method;
    const char*             path;
    const char*             body;
    int64_t                 body_len;
    const lisa_principal_t* principal;
} lisa_http_request_t;

typedef struct lisa_http_response {
    size_t      struct_size;
    int         status_code;
    const char* content_type;
    char*       body;          /* allocated with the context allocator; LISA frees it */
    int64_t     body_len;
} lisa_http_response_t;

typedef struct lisa_http_routes {
    size_t struct_size;
    void*  user;
    /*
     * Offered every request the core does not handle itself. Return
     * LISA_OK after filling *response, or LISA_E_NOT_FOUND to decline.
     */
    int (*handle)(void* user, const lisa_http_request_t* request,
                  lisa_http_response_t* response);
} lisa_http_routes_t;

/* ==== Context ========================================================= */

typedef struct lisa_context lisa_context_t;

typedef struct lisa_context_config {
    size_t                         struct_size;
    const lisa_allocator_t*        allocator;         /* NULL: system allocator */
    const lisa_auth_provider_t*    auth;              /* NULL: none */
    const lisa_retrieval_filter_t* retrieval_filter;  /* NULL: none */
    const lisa_audit_sink_t*       audit;             /* NULL: none */
    const lisa_storage_crypto_t*   storage_crypto;    /* NULL: none */
    const lisa_http_routes_t*      http_routes;       /* NULL: none */
} lisa_context_config_t;

#define LISA_CONTEXT_CONFIG_INIT \
    { sizeof(lisa_context_config_t), NULL, NULL, NULL, NULL, NULL, NULL }

/*
 * Create a context. config may be NULL for defaults. The structs the
 * config points to are copied; the `user` pointers inside them must stay
 * valid until the context is destroyed.
 */
LISA_API int lisa_context_create(const lisa_context_config_t* config,
                                 lisa_context_t** out);

/*
 * Destroy a context. All handles created from it must be closed first.
 * NULL is ignored.
 */
LISA_API void lisa_context_destroy(lisa_context_t* ctx);

/*
 * Installed extensions (for the HTTP server and tests). Return NULL when
 * none is installed. Valid until the context is destroyed.
 */
LISA_API const lisa_auth_provider_t* lisa_context_auth(const lisa_context_t* ctx);
LISA_API const lisa_http_routes_t*   lisa_context_http_routes(const lisa_context_t* ctx);

/* ==== Collections ===================================================== */

typedef struct lisa_collection lisa_collection_t;

typedef enum lisa_open_mode {
    LISA_OPEN_READ  = 0,
    LISA_OPEN_WRITE = 1   /* one writer per collection at a time */
} lisa_open_mode;

/*
 * A chunk: a piece of a document with its metadata.
 *
 * As input (lisa_collection_add) all strings must be non-NULL (may be
 * empty) and are copied. As output (lisa_collection_get_chunk) the
 * struct and its strings are owned by LISA; release with lisa_chunk_free.
 */
typedef struct lisa_chunk {
    uint64_t    id;            /* output only; ignored on input */
    const char* doc_id;        /* caller-defined document key */
    int64_t     chunk_index;   /* position within the document, >= 0 */
    const char* source_path;   /* file the chunk came from */
    int64_t     offset;        /* byte offset in the source text, >= 0 */
    int64_t     length;        /* byte length, >= 0 */
    const char* text;
    const char* content_hash;  /* hash of the source document content */
} lisa_chunk_t;

typedef struct lisa_collection_info {
    size_t      struct_size;
    const char* embedding_model;  /* owned by the collection handle */
    int64_t     dim;
    int64_t     chunk_count;      /* live chunks, as of the handle's view */
} lisa_collection_info_t;

#define LISA_COLLECTION_INFO_INIT { sizeof(lisa_collection_info_t), NULL, 0, 0 }

/*
 * Create an empty collection directory at path (must not exist; its
 * parent must). The embedding model and dimension are fixed for life.
 */
LISA_API int lisa_collection_create(lisa_context_t* ctx, const char* path,
                                    const char* embedding_model, int64_t dim);

/*
 * Open a collection. If expected_model is non-NULL and differs from the
 * collection's model, fails with LISA_E_MODEL_MISMATCH. Only one
 * LISA_OPEN_WRITE handle may exist per collection (LISA_E_BUSY).
 * Release with lisa_collection_close.
 */
LISA_API int lisa_collection_open(lisa_context_t* ctx, const char* path,
                                  lisa_open_mode mode, const char* expected_model,
                                  lisa_collection_t** out);

/* Close a collection. NULL is ignored. */
LISA_API void lisa_collection_close(lisa_collection_t* coll);

/* Fill *info (set info->struct_size first, e.g. LISA_COLLECTION_INFO_INIT). */
LISA_API int lisa_collection_info(const lisa_collection_t* coll,
                                  lisa_collection_info_t* info);

/* See changes committed by other handles or processes. */
LISA_API int lisa_collection_refresh(lisa_collection_t* coll);

/*
 * Add count chunks atomically. vectors: count * dim floats. If out_ids is
 * non-NULL it receives count new, stable IDs.
 */
LISA_API int lisa_collection_add(lisa_collection_t* coll, int64_t count,
                                 const float* vectors, const lisa_chunk_t* chunks,
                                 uint64_t* out_ids);

/* Remove chunks atomically. Any unknown ID: nothing removed, LISA_E_NOT_FOUND. */
LISA_API int lisa_collection_remove(lisa_collection_t* coll, int64_t count,
                                    const uint64_t* ids);

/* Remove all chunks of a document. *out_removed (may be NULL) gets the count. */
LISA_API int lisa_collection_remove_document(lisa_collection_t* coll,
                                             const char* doc_id,
                                             int64_t* out_removed);

/* Read one chunk. Release *out with lisa_chunk_free. */
LISA_API int lisa_collection_get_chunk(lisa_collection_t* coll, uint64_t id,
                                       lisa_chunk_t** out);

/* Release a chunk returned by lisa_collection_get_chunk. NULL is ignored. */
LISA_API void lisa_chunk_free(lisa_chunk_t* chunk);

/* Reclaim space from removed chunks. IDs do not change. Writer only. */
LISA_API int lisa_collection_compact(lisa_collection_t* coll);

/* ==== Search ========================================================== */

typedef struct lisa_search_options {
    size_t                  struct_size;
    int64_t                 top_k;      /* 1..10000; default 5 */
    const lisa_principal_t* principal;  /* passed to the retrieval filter; NULL: local */
} lisa_search_options_t;

#define LISA_SEARCH_OPTIONS_INIT { sizeof(lisa_search_options_t), 5, NULL }

typedef struct lisa_hit {
    uint64_t id;        /* chunk ID */
    float    distance;  /* squared L2 distance; smaller is closer */
} lisa_hit_t;

/*
 * Exact nearest-neighbour search by vector. query: dim floats. options
 * may be NULL for defaults. Writes up to min(top_k, capacity) hits,
 * nearest first, into the caller's hits array and their number into
 * *out_count. Chunks hidden by the retrieval filter are never returned.
 */
LISA_API int lisa_collection_search_vector(lisa_collection_t* coll,
                                           const float* query,
                                           const lisa_search_options_t* options,
                                           lisa_hit_t* hits, int64_t capacity,
                                           int64_t* out_count);

/* ==== Models ========================================================== */
/*
 * Local models in GGUF format: generation (chat) models and embedding
 * models. A model handle must be used by one thread at a time; loading
 * the same file twice shares nothing.
 */

typedef struct lisa_model lisa_model_t;

/* Progress in [0, 1]. Return non-zero to continue, 0 to cancel. */
typedef int (*lisa_progress_fn)(void* user, float fraction);

typedef struct lisa_model_options {
    size_t           struct_size;
    int32_t          gpu_layers;      /* -1: all layers on the GPU (default); 0: CPU only */
    int64_t          context_tokens;  /* context window; 0: 4096 (capped at the model's limit) */
    int32_t          threads;         /* 0: automatic */
    lisa_progress_fn progress;        /* optional load progress */
    void*            progress_user;
} lisa_model_options_t;

#define LISA_MODEL_OPTIONS_INIT { sizeof(lisa_model_options_t), -1, 0, 0, NULL, NULL }

/*
 * Load a GGUF model file. options may be NULL. Release with
 * lisa_model_free. Note: on macOS the first load after install compiles
 * GPU shaders, which can take tens of seconds; use `progress`.
 */
LISA_API int lisa_model_load(lisa_context_t* ctx, const char* path,
                             const lisa_model_options_t* options, lisa_model_t** out);

/* Release a model. NULL is ignored. */
LISA_API void lisa_model_free(lisa_model_t* model);

typedef struct lisa_model_info {
    size_t      struct_size;
    const char* name;            /* from the model file; owned by the handle */
    const char* architecture;    /* e.g. "qwen3" */
    const char* profile;         /* LISA profile in use, e.g. "qwen3-4b-q4_k_m", or "generic" */
    int64_t     file_size;
    int64_t     context_train;   /* context length the model was trained with */
    int64_t     context_tokens;  /* context window in use */
    int64_t     embedding_dim;   /* for embedding models; 0 otherwise */
    int32_t     is_embedding;
    int32_t     gpu;             /* 1 if layers run on the GPU */
} lisa_model_info_t;

#define LISA_MODEL_INFO_INIT { sizeof(lisa_model_info_t), NULL, NULL, NULL, 0, 0, 0, 0, 0, 0 }

LISA_API int lisa_model_info(const lisa_model_t* model, lisa_model_info_t* info);

/* --- Known models (verified files with licences) --- */

typedef struct lisa_known_model {
    const char* id;
    const char* file_name;
    int64_t     file_size;
    const char* sha256;       /* lowercase hex */
    const char* license;      /* SPDX identifier */
    const char* source;       /* download location and revision */
    int32_t     is_embedding;
} lisa_known_model_t;

/* Number of known models; lisa_known_model(i) for i in [0, count). */
LISA_API int64_t lisa_known_model_count(void);
LISA_API int     lisa_known_model(int64_t index, lisa_known_model_t* out);

/*
 * Hash a model file and compare it with the known models.
 *   LISA_OK:             a known model; *match (if non-NULL) describes it.
 *   LISA_E_UNSUPPORTED:  readable, but not a known model (it may still load).
 *   LISA_E_NOT_FOUND:    no such file.
 * sha256_hex (if non-NULL, 65 bytes) receives the file's SHA-256.
 */
LISA_API int lisa_model_verify(const char* path, lisa_known_model_t* match, char* sha256_hex);

/* --- Generation --- */

typedef struct lisa_message {
    const char* role;     /* "system", "user", or "assistant" */
    const char* content;
} lisa_message_t;

/*
 * Receives generated text as it is produced, in pieces that always end on
 * a UTF-8 character boundary. Return 0 to continue, non-zero to stop.
 */
typedef int (*lisa_token_fn)(void* user, const char* text, int64_t len);

typedef struct lisa_generate_options {
    size_t        struct_size;
    int64_t       max_tokens;     /* default 512 */
    float         temperature;    /* 0: deterministic (greedy); < 0: model default */
    uint64_t      seed;           /* for temperature > 0 */
    lisa_token_fn on_token;       /* optional streaming callback */
    void*         on_token_user;
} lisa_generate_options_t;

#define LISA_GENERATE_OPTIONS_INIT \
    { sizeof(lisa_generate_options_t), 512, -1.0f, 0, NULL, NULL }

/*
 * Chat: format messages with the model's chat template and generate the
 * assistant's reply. *out_text (optional) receives the reply, allocated
 * with the context allocator; release with lisa_free. *out_tokens
 * (optional) receives the number of generated tokens.
 */
LISA_API int lisa_chat(lisa_model_t* model, const lisa_message_t* messages, int64_t count,
                       const lisa_generate_options_t* options,
                       char** out_text, int64_t* out_tokens);

/* Continue raw prompt text (no chat template). Same outputs as lisa_chat. */
LISA_API int lisa_generate(lisa_model_t* model, const char* prompt,
                           const lisa_generate_options_t* options,
                           char** out_text, int64_t* out_tokens);

/* --- Embeddings --- */

typedef enum lisa_embed_kind {
    LISA_EMBED_DOCUMENT = 0,  /* text to be searched */
    LISA_EMBED_QUERY    = 1   /* a question used to search */
} lisa_embed_kind;

/*
 * Embed count texts into out (count * dim floats), L2-normalised. dim: 0
 * for the model's native dimension, or smaller if the model supports
 * truncated embeddings (else LISA_E_INVALID_ARGUMENT). Queries and
 * documents may be embedded differently, as the model requires.
 */
LISA_API int lisa_embed(lisa_model_t* model, lisa_embed_kind kind,
                        const char* const* texts, int64_t count,
                        float* out, int64_t dim);

/* ==== Memory returned by LISA ========================================= */

/* Free memory LISA returned (e.g. generated text). NULL is ignored. */
LISA_API void lisa_free(lisa_context_t* ctx, void* ptr);

#ifdef __cplusplus
}
#endif

#endif /* LISA_H */
