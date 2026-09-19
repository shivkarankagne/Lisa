/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_INGEST_H
#define LISA_INGEST_H

/*
 * LISA ingest — keep a collection in sync with files and folders.
 *
 * For every supported file under the given paths:
 *   - same size and modification time as recorded: skipped (not read);
 *   - same content hash as recorded: record updated, nothing re-indexed;
 *   - new or changed: extracted, chunked, embedded, and swapped into the
 *     collection in one transaction (the old version stays until then);
 *   - unreadable or without text: recorded with status "error" or
 *     "no_text", so later runs skip it until the file changes.
 * Afterwards, documents under a given directory whose files no longer
 * exist are removed.
 *
 * Runs on the calling thread. PDF extraction is not thread-safe, so run
 * at most one ingest at a time per process. The caller owns the store
 * (opened for writing) and the embedder for the duration of the run.
 */

#include <stdint.h>

#include "../documents/documents.h"
#include "../storage/store.h"

#define INGEST_OK          0
#define INGEST_EINVAL     -1
#define INGEST_ENOTFOUND  -2   /* a given path does not exist */
#define INGEST_ESTORE     -3   /* collection error; see store_status */
#define INGEST_ECANCELLED -4   /* the progress callback asked to stop */
#define INGEST_ENOMEM     -5

/*
 * Embed n document texts into out (n * dim floats). Return 0 on success;
 * a non-zero value marks the current document as failed.
 */
typedef int (*ingest_embed_fn)(void* user, const char* const* texts, int64_t n,
                               float* out, int64_t dim);

typedef struct {
    int64_t     files_seen;       /* supported files found */
    int64_t     files_added;      /* new documents indexed */
    int64_t     files_updated;    /* changed documents re-indexed */
    int64_t     files_unchanged;
    int64_t     files_no_text;    /* e.g. scanned PDFs */
    int64_t     files_failed;     /* unreadable, corrupt, encrypted, embedding failed */
    int64_t     files_removed;    /* documents whose files are gone */
    int64_t     files_skipped;    /* unsupported file types */
    int64_t     chunks_added;
    int64_t     chunks_removed;
    const char* current;          /* file being processed; valid only during the callback */
} ingest_progress_t;

/* Called after each file. Return non-zero to continue, 0 to cancel. */
typedef int (*ingest_progress_fn)(void* user, const ingest_progress_t* p);

typedef struct {
    lisa_store_t*      store;          /* opened LISA_STORE_WRITE */
    ingest_embed_fn    embed;
    void*              embed_user;
    int64_t            embed_batch;    /* chunks per embed call; 0 = 16 */
    doc_chunk_params_t chunk;
    int                include_hidden; /* also index files/dirs starting with "." */
    ingest_progress_fn progress;       /* optional */
    void*              progress_user;
} ingest_params_t;

/*
 * Sync the collection with paths (files or directories). All paths are
 * checked first: if any does not exist, nothing is changed and
 * INGEST_ENOTFOUND is returned. *result (optional) receives the counts.
 * *store_status (optional) receives the store error for INGEST_ESTORE.
 */
int ingest_run(const ingest_params_t* p, const char* const* paths, int64_t n_paths,
               ingest_progress_t* result, int* store_status);

#endif /* LISA_INGEST_H */
