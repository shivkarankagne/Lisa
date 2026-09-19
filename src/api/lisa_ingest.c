/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Public ingest API (include/lisa.h, "Ingest") over src/ingest/ingest.h:
 * background jobs, model/collection pairing, document listing.
 */

#include "api_internal.h"

#include <stdlib.h>
#include <string.h>

#include "../ingest/ingest.h"
#include "../platform/platform.h"

/* ---- model identity ----------------------------------------------------- */

/*
 * The identity recorded in a collection for an embedding model: the LISA
 * profile id for known models, else "gguf:" + the model's own name.
 */
void lisa_api_model_id(const lisa_model_t* m, char* buf, size_t n) {
    lm_info_t li;
    lm_get_info(m->lm, &li);
    if (strcmp(li.profile_id, "generic") != 0) snprintf(buf, n, "%s", li.profile_id);
    else snprintf(buf, n, "gguf:%s", li.name[0] ? li.name : "unnamed");
}

/* Check that dim is usable with this embedding model. */
int lisa_api_check_dim(const lisa_model_t* m, int64_t dim) {
    lm_info_t li;
    lm_get_info(m->lm, &li);
    if (!li.is_embedding) return LISA_E_WRONG_MODEL_KIND;
    if (dim < 1 || dim > li.embedding_dim) return LISA_E_INVALID_ARGUMENT;
    if (dim < li.embedding_dim && !li.supports_truncation) return LISA_E_INVALID_ARGUMENT;
    return LISA_OK;
}

int lisa_collection_create_for_model(lisa_context_t* ctx, const char* path,
                                     lisa_model_t* model, int64_t dim) {
    if (ctx == NULL || path == NULL || model == NULL || dim < 0) return LISA_E_INVALID_ARGUMENT;
    if (API_BUSY(model)) return LISA_E_BUSY;
    lm_info_t li;
    lm_get_info(model->lm, &li);
    if (!li.is_embedding) return LISA_E_WRONG_MODEL_KIND;
    if (dim == 0) dim = li.embedding_dim;
    int rc = lisa_api_check_dim(model, dim);
    if (rc != LISA_OK) return rc;
    char id[256];
    lisa_api_model_id(model, id, sizeof(id));
    return lisa_collection_create(ctx, path, id, dim);
}

/* ---- documents ---------------------------------------------------------- */

typedef struct {
    lisa_document_fn fn;
    void*            user;
} doc_cb_t;

static int each_doc(void* user, const lisa_store_doc_t* d) {
    doc_cb_t* cb = (doc_cb_t*)user;
    lisa_document_t out;
    out.path = d->source_path;
    out.title = d->title;
    out.status = d->status;
    out.message = d->message;
    out.chunk_count = d->chunk_count;
    out.size = d->size;
    return cb->fn(cb->user, &out);
}

int lisa_collection_documents(lisa_collection_t* coll, const char* path_prefix,
                              lisa_document_fn fn, void* user) {
    if (coll == NULL || fn == NULL) return LISA_E_INVALID_ARGUMENT;
    if (API_BUSY(coll)) return LISA_E_BUSY;
    doc_cb_t cb = { fn, user };
    return lisa_api_from_store(lisa_store_doc_list(coll->store, path_prefix, each_doc, &cb));
}

/* ---- jobs --------------------------------------------------------------- */

struct lisa_ingest_job {
    lisa_context_t*    ctx;
    lisa_collection_t* coll;
    lisa_model_t*      model;
    char**             paths;
    int64_t            n_paths;
    doc_chunk_params_t chunk;
    int                include_hidden;
    int64_t            dim;

    lisa_thread_t*     thread;
    int                joined;
    lisa_mutex_t*      mutex;      /* guards the fields below */
    ingest_progress_t  prog;
    lisa_job_state     state;
    int                status;
    int64_t            start_ns, end_ns;
    atomic_int         cancel;
};

static int job_embed(void* user, const char* const* texts, int64_t n, float* out, int64_t dim) {
    lisa_ingest_job_t* j = (lisa_ingest_job_t*)user;
    return lm_embed(j->model->lm, LM_EMBED_DOCUMENT, texts, n, out, dim) == LM_OK ? 0 : 1;
}

static int job_progress(void* user, const ingest_progress_t* p) {
    lisa_ingest_job_t* j = (lisa_ingest_job_t*)user;
    lisa_mutex_lock(j->mutex);
    j->prog = *p;
    j->prog.current = NULL;
    lisa_mutex_unlock(j->mutex);
    return atomic_load(&j->cancel) == 0;
}

static int from_ingest(int rc, int store_status) {
    switch (rc) {
    case INGEST_OK:         return LISA_OK;
    case INGEST_EINVAL:     return LISA_E_INVALID_ARGUMENT;
    case INGEST_ENOTFOUND:  return LISA_E_NOT_FOUND;
    case INGEST_ESTORE:     return lisa_api_from_store(store_status);
    case INGEST_ECANCELLED: return LISA_E_CANCELLED;
    case INGEST_ENOMEM:     return LISA_E_NO_MEMORY;
    default:                return LISA_E_INTERNAL;
    }
}

static void job_main(void* arg) {
    lisa_ingest_job_t* j = (lisa_ingest_job_t*)arg;
    ingest_params_t p;
    memset(&p, 0, sizeof(p));
    p.store = j->coll->store;
    p.embed = job_embed;
    p.embed_user = j;
    p.chunk = j->chunk;
    p.include_hidden = j->include_hidden;
    p.progress = job_progress;
    p.progress_user = j;

    ingest_progress_t result;
    int store_status = 0;
    int rc = ingest_run(&p, (const char* const*)j->paths, j->n_paths, &result, &store_status);
    int status = from_ingest(rc, store_status);

    lisa_mutex_lock(j->mutex);
    j->prog = result;
    j->status = status;
    j->state = status == LISA_OK ? LISA_JOB_SUCCEEDED
             : status == LISA_E_CANCELLED ? LISA_JOB_CANCELLED : LISA_JOB_FAILED;
    j->end_ns = lisa_time_monotonic_ns();
    lisa_mutex_unlock(j->mutex);

    lisa_api_audit(j->ctx, LISA_AUDIT_INGEST, status, NULL, j->coll->path,
                   result.chunks_added, NULL);
    /* The handles are usable again as soon as the work is done. */
    atomic_store(&j->model->busy, 0);
    atomic_store(&j->coll->busy, 0);
}

static void job_release(lisa_ingest_job_t* j) {
    for (int64_t i = 0; i < j->n_paths; i++) ctx_free(j->ctx, j->paths ? j->paths[i] : NULL);
    ctx_free(j->ctx, j->paths);
    lisa_mutex_destroy(j->mutex);
    ctx_free(j->ctx, j);
}

int lisa_ingest_start(lisa_collection_t* coll, lisa_model_t* model,
                      const char* const* paths, int64_t count,
                      const lisa_ingest_options_t* o, lisa_ingest_job_t** out) {
    if (out == NULL) return LISA_E_INVALID_ARGUMENT;
    *out = NULL;
    if (coll == NULL || model == NULL || paths == NULL || count <= 0) return LISA_E_INVALID_ARGUMENT;
    if (coll->mode != LISA_OPEN_WRITE) return LISA_E_READ_ONLY;

    doc_chunk_params_t chunk = DOC_CHUNK_PARAMS_DEFAULT;
    int include_hidden = 0;
    if (o != NULL) {
        if (o->struct_size < sizeof(size_t)) return LISA_E_INVALID_ARGUMENT;
        if (HAS_FIELD(o, lisa_ingest_options_t, chunk_chars)) chunk.target_chars = o->chunk_chars;
        if (HAS_FIELD(o, lisa_ingest_options_t, chunk_max_chars)) chunk.max_chars = o->chunk_max_chars;
        if (HAS_FIELD(o, lisa_ingest_options_t, chunk_overlap_chars)) chunk.overlap_chars = o->chunk_overlap_chars;
        if (HAS_FIELD(o, lisa_ingest_options_t, include_hidden)) include_hidden = o->include_hidden != 0;
    }
    if (chunk.target_chars < 1 || chunk.max_chars < chunk.target_chars ||
        chunk.overlap_chars < 0 || chunk.overlap_chars >= chunk.target_chars)
        return LISA_E_INVALID_ARGUMENT;

    for (int64_t i = 0; i < count; i++) {
        lisa_file_info_t info;
        if (paths[i] == NULL) return LISA_E_INVALID_ARGUMENT;
        if (lisa_file_info(paths[i], &info) != LISA_PLAT_OK || (!info.is_file && !info.is_dir))
            return LISA_E_NOT_FOUND;
    }

    /* Claim both handles; release on any failure below. */
    int expected = 0;
    if (!atomic_compare_exchange_strong(&coll->busy, &expected, 1)) return LISA_E_BUSY;
    expected = 0;
    if (!atomic_compare_exchange_strong(&model->busy, &expected, 1)) {
        atomic_store(&coll->busy, 0);
        return LISA_E_BUSY;
    }

    int rc = LISA_OK;
    char id[256];
    lisa_api_model_id(model, id, sizeof(id));
    int64_t dim = lisa_store_dim(coll->store);
    if (strcmp(id, lisa_store_model(coll->store)) != 0) rc = LISA_E_MODEL_MISMATCH;
    else rc = lisa_api_check_dim(model, dim);
    if (rc == LISA_E_INVALID_ARGUMENT) rc = LISA_E_MODEL_MISMATCH;  /* dim the model cannot produce */

    lisa_ingest_job_t* j = NULL;
    if (rc == LISA_OK) {
        j = (lisa_ingest_job_t*)ctx_alloc(coll->ctx, sizeof(*j));
        if (j == NULL) rc = LISA_E_NO_MEMORY;
    }
    if (rc == LISA_OK) {
        memset(j, 0, sizeof(*j));
        j->ctx = coll->ctx;
        j->coll = coll;
        j->model = model;
        j->chunk = chunk;
        j->include_hidden = include_hidden;
        j->dim = dim;
        j->state = LISA_JOB_RUNNING;
        atomic_init(&j->cancel, 0);
        j->paths = (char**)ctx_alloc(j->ctx, (size_t)count * sizeof(char*));
        if (j->paths == NULL) rc = LISA_E_NO_MEMORY;
        else memset(j->paths, 0, (size_t)count * sizeof(char*));
        for (int64_t i = 0; rc == LISA_OK && i < count; i++) {
            j->paths[i] = ctx_strdup(j->ctx, paths[i]);
            j->n_paths = i + 1;
            if (j->paths[i] == NULL) rc = LISA_E_NO_MEMORY;
        }
        if (rc == LISA_OK && lisa_mutex_create(&j->mutex) != LISA_PLAT_OK) rc = LISA_E_NO_MEMORY;
        if (rc == LISA_OK) {
            j->start_ns = lisa_time_monotonic_ns();
            if (lisa_thread_start(job_main, j, &j->thread) != LISA_PLAT_OK) rc = LISA_E_INTERNAL;
        }
    }
    if (rc != LISA_OK) {
        if (j) job_release(j);
        atomic_store(&model->busy, 0);
        atomic_store(&coll->busy, 0);
        return rc;
    }
    *out = j;
    return LISA_OK;
}

int lisa_ingest_status(lisa_ingest_job_t* j, lisa_ingest_status_t* st) {
    if (j == NULL || st == NULL || st->struct_size < sizeof(size_t)) return LISA_E_INVALID_ARGUMENT;
    lisa_mutex_lock(j->mutex);
    ingest_progress_t p = j->prog;
    lisa_job_state state = j->state;
    int status = j->status;
    int64_t end = j->end_ns ? j->end_ns : lisa_time_monotonic_ns();
    lisa_mutex_unlock(j->mutex);

    if (HAS_FIELD(st, lisa_ingest_status_t, state)) st->state = state;
    if (HAS_FIELD(st, lisa_ingest_status_t, status)) st->status = status;
    if (HAS_FIELD(st, lisa_ingest_status_t, files_seen)) st->files_seen = p.files_seen;
    if (HAS_FIELD(st, lisa_ingest_status_t, files_added)) st->files_added = p.files_added;
    if (HAS_FIELD(st, lisa_ingest_status_t, files_updated)) st->files_updated = p.files_updated;
    if (HAS_FIELD(st, lisa_ingest_status_t, files_unchanged)) st->files_unchanged = p.files_unchanged;
    if (HAS_FIELD(st, lisa_ingest_status_t, files_no_text)) st->files_no_text = p.files_no_text;
    if (HAS_FIELD(st, lisa_ingest_status_t, files_failed)) st->files_failed = p.files_failed;
    if (HAS_FIELD(st, lisa_ingest_status_t, files_removed)) st->files_removed = p.files_removed;
    if (HAS_FIELD(st, lisa_ingest_status_t, files_skipped)) st->files_skipped = p.files_skipped;
    if (HAS_FIELD(st, lisa_ingest_status_t, chunks_added)) st->chunks_added = p.chunks_added;
    if (HAS_FIELD(st, lisa_ingest_status_t, chunks_removed)) st->chunks_removed = p.chunks_removed;
    if (HAS_FIELD(st, lisa_ingest_status_t, elapsed_seconds))
        st->elapsed_seconds = (double)(end - j->start_ns) / 1e9;
    return LISA_OK;
}

void lisa_ingest_cancel(lisa_ingest_job_t* j) {
    if (j) atomic_store(&j->cancel, 1);
}

int lisa_ingest_wait(lisa_ingest_job_t* j, lisa_ingest_status_t* st) {
    if (j == NULL) return LISA_E_INVALID_ARGUMENT;
    if (!j->joined) {
        lisa_thread_join(j->thread);
        j->thread = NULL;
        j->joined = 1;
    }
    if (st) lisa_ingest_status(j, st);
    return j->status;
}

void lisa_ingest_free(lisa_ingest_job_t* j) {
    if (j == NULL) return;
    lisa_ingest_cancel(j);
    lisa_ingest_wait(j, NULL);
    job_release(j);
}

int lisa_ingest(lisa_collection_t* coll, lisa_model_t* model, const char* const* paths,
                int64_t count, const lisa_ingest_options_t* options,
                lisa_ingest_status_t* status) {
    lisa_ingest_job_t* j = NULL;
    int rc = lisa_ingest_start(coll, model, paths, count, options, &j);
    if (rc != LISA_OK) return rc;
    rc = lisa_ingest_wait(j, status);
    job_release(j);
    return rc;
}
