/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Ingest: sync a collection with files and folders. See ingest.h.
 */

#include "ingest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hash/xxhash/xxhash.h"
#include "../platform/platform.h"

#define DEFAULT_EMBED_BATCH 16
#define EMBED_ATTEMPTS      3      /* a busy GPU is a moment, not a verdict */
#define EMBED_RETRY_MS      400
#define HASH_CHUNK (1 << 20)

typedef struct {
    const ingest_params_t* p;
    int64_t                dim;
    ingest_progress_t      prog;
    int                    store_rc;   /* first store error */
    int                    cancelled;
    char**                 seen;       /* absolute paths visited (for removals) */
    int64_t                n_seen, cap_seen;
    const char*            walk_root;  /* folder being walked; NULL for a named file */
    int                    recipe_changed;  /* collection was embedded by an older LISA */
} run_t;

/* ---- helpers ---------------------------------------------------------- */

static char* xstrdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char* xstrndup(const char* s, int64_t n) {
    char* d = (char*)malloc((size_t)n + 1);
    if (d) {
        memcpy(d, s, (size_t)n);
        d[n] = '\0';
    }
    return d;
}

/* "xxh3:<16 hex>" of the file's bytes, or NULL if it cannot be read. */
static char* hash_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) return NULL;
    unsigned char* buf = (unsigned char*)malloc(HASH_CHUNK);
    XXH3_state_t* st = XXH3_createState();
    char* out = NULL;
    if (buf && st && XXH3_64bits_reset(st) == XXH_OK) {
        size_t n;
        int ok = 1;
        while ((n = fread(buf, 1, HASH_CHUNK, f)) > 0) {
            if (XXH3_64bits_update(st, buf, n) != XXH_OK) {
                ok = 0;
                break;
            }
        }
        if (ok && !ferror(f)) {
            out = (char*)malloc(32);
            if (out) snprintf(out, 32, "xxh3:%016llx", (unsigned long long)XXH3_64bits_digest(st));
        }
    }
    XXH3_freeState(st);
    free(buf);
    fclose(f);
    return out;
}

static int remember(run_t* r, const char* path) {
    if (r->n_seen == r->cap_seen) {
        int64_t cap = r->cap_seen ? r->cap_seen * 2 : 256;
        char** g = (char**)realloc(r->seen, (size_t)cap * sizeof(char*));
        if (g == NULL) return INGEST_ENOMEM;
        r->seen = g;
        r->cap_seen = cap;
    }
    r->seen[r->n_seen] = xstrdup(path);
    if (r->seen[r->n_seen] == NULL) return INGEST_ENOMEM;
    r->n_seen++;
    return INGEST_OK;
}

static int cmp_str(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static int was_seen(const run_t* r, const char* path) {
    return bsearch(&path, r->seen, (size_t)r->n_seen, sizeof(char*), cmp_str) != NULL;
}

static const char* doc_error_message(int rc) {
    switch (rc) {
    case DOC_ENOTFOUND:    return "file disappeared during ingest";
    case DOC_EIO:          return "cannot read file";
    case DOC_EFORMAT:      return "file is corrupt or not in the format its name suggests";
    case DOC_EENCRYPTED:   return "file is password-protected";
    case DOC_ETOOBIG:      return "file is larger than the supported maximum";
    case DOC_ENOMEM:       return "out of memory";
    default:               return "cannot extract text";
    }
}

/* Store a document record with no chunks (status no_text or error). */
static int record_empty(run_t* r, const char* path, const char* hash,
                        const lisa_file_info_t* info, const char* title,
                        const char* status, const char* message, int existed) {
    lisa_store_doc_t d;
    memset(&d, 0, sizeof(d));
    d.doc_id = (char*)path;
    d.source_path = (char*)path;
    d.content_hash = (char*)(hash ? hash : "");
    d.size = info->size;
    d.mtime_ns = info->mtime_ns;
    d.title = (char*)(title ? title : "");
    d.status = (char*)status;
    d.message = (char*)message;
    lisa_store_doc_t old;
    int64_t old_chunks = 0;
    if (existed && lisa_store_doc_get(r->p->store, path, &old) == LISA_STORE_OK) {
        old_chunks = old.chunk_count;
        lisa_store_doc_free(&old);
    }
    int rc = lisa_store_replace_doc(r->p->store, &d, 0, NULL, NULL, NULL);
    if (rc != LISA_STORE_OK) return rc;
    r->prog.chunks_removed += old_chunks;
    return LISA_STORE_OK;
}

/* ---- one document ----------------------------------------------------- */

static int index_file(run_t* r, const char* path, const lisa_file_info_t* info) {
    const ingest_params_t* p = r->p;
    lisa_store_doc_t old;
    int existed = lisa_store_doc_get(p->store, path, &old) == LISA_STORE_OK;

    /*
     * A document that failed before is always read again: the cause is
     * usually temporary (a busy GPU, a locked file), and a collection
     * that quietly leaves documents out gives wrong "not found" answers.
     */
    int failed_before = existed && old.status && strcmp(old.status, "error") == 0;
    if (r->recipe_changed) failed_before = 1;   /* rebuild: vectors were made differently */

    /* Fast path: size and mtime unchanged -> not even read. */
    if (existed && !failed_before && old.size == info->size && old.mtime_ns == info->mtime_ns) {
        lisa_store_doc_free(&old);
        r->prog.files_unchanged++;
        return LISA_STORE_OK;
    }

    char* hash = hash_file(path);
    if (hash == NULL) {
        if (existed) lisa_store_doc_free(&old);
        r->prog.files_failed++;
        return record_empty(r, path, NULL, info, NULL, "error", "cannot read file", existed);
    }
    if (existed && !failed_before && strcmp(hash, old.content_hash) == 0) {
        /* Touched but identical: remember the new size/mtime only. */
        lisa_store_doc_free(&old);
        free(hash);
        r->prog.files_unchanged++;
        return lisa_store_doc_touch(p->store, path, info->size, info->mtime_ns);
    }
    int64_t old_chunks = existed ? old.chunk_count : 0;
    if (existed) lisa_store_doc_free(&old);

    doc_text_t t;
    int drc = doc_extract(path, &t);
    if (drc != DOC_OK) {
        r->prog.files_failed++;
        int rc = record_empty(r, path, hash, info, NULL, "error", doc_error_message(drc), existed);
        free(hash);
        return rc;
    }

    doc_chunk_t* spans = NULL;
    int64_t n = 0;
    drc = doc_chunk(&t, &p->chunk, &spans, &n);
    if (drc != DOC_OK || n == 0) {
        if (drc == DOC_OK) r->prog.files_no_text++;
        else r->prog.files_failed++;
        int rc = record_empty(r, path, hash, info, t.title,
                              drc == DOC_OK ? "no_text" : "error",
                              drc == DOC_OK ? "" : "cannot split text into chunks", existed);
        doc_text_free(&t);
        free(hash);
        return rc;
    }

    /*
     * Chunk texts. The embedded text is the passage alone: prefixing the
     * document title made short front-matter chunks (title page,
     * acknowledgements) the closest match for any question that repeated
     * the title or the author's name, because the title was most of their
     * vector. The title still reaches search through the keyword index.
     */
    char** texts = (char**)calloc((size_t)n, sizeof(char*));
    float* vectors = (float*)malloc((size_t)(n * r->dim) * sizeof(float));
    lisa_store_chunk_t* chunks = (lisa_store_chunk_t*)calloc((size_t)n, sizeof(lisa_store_chunk_t));
    int rc = (texts && vectors && chunks) ? LISA_STORE_OK : LISA_STORE_ENOMEM;
    for (int64_t i = 0; rc == LISA_STORE_OK && i < n; i++) {
        texts[i] = xstrndup(t.text + spans[i].offset, spans[i].length);
        if (texts[i] == NULL) {
            rc = LISA_STORE_ENOMEM;
            break;
        }
        chunks[i].doc_id = (char*)path;
        chunks[i].chunk_index = i;
        chunks[i].source_path = (char*)path;
        chunks[i].offset = spans[i].offset;
        chunks[i].length = spans[i].length;
        chunks[i].text = texts[i];
        chunks[i].content_hash = hash;
        chunks[i].page = spans[i].page;
    }

    int embed_failed = 0;
    int64_t batch = p->embed_batch > 0 ? p->embed_batch : DEFAULT_EMBED_BATCH;
    for (int64_t i = 0; rc == LISA_STORE_OK && !embed_failed && i < n; i += batch) {
        int64_t m = n - i < batch ? n - i : batch;
        /*
         * Embedding can fail for a moment rather than for good: the GPU
         * may be busy with another program. Try again before giving up,
         * and record such a file so the next pass retries it.
         */
        for (int attempt = 0; attempt < EMBED_ATTEMPTS; attempt++) {
            if (p->embed(p->embed_user, (const char* const*)(texts + i), m,
                         vectors + i * r->dim, r->dim) == 0) {
                embed_failed = 0;
                break;
            }
            embed_failed = 1;
            if (attempt + 1 < EMBED_ATTEMPTS) lisa_sleep_ms(EMBED_RETRY_MS << attempt);
        }
    }

    if (rc == LISA_STORE_OK && embed_failed) {
        r->prog.files_failed++;
        /* mtime 0: the file looks changed next time, so it is read again. */
        lisa_file_info_t retry = *info;
        retry.mtime_ns = 0;
        rc = record_empty(r, path, "", &retry, t.title,
                          "error", "could not be indexed (the model was busy); will try again", existed);
    } else if (rc == LISA_STORE_OK) {
        lisa_store_doc_t d;
        memset(&d, 0, sizeof(d));
        d.doc_id = (char*)path;
        d.source_path = (char*)path;
        d.content_hash = hash;
        d.size = info->size;
        d.mtime_ns = info->mtime_ns;
        d.title = t.title ? t.title : (char*)"";
        d.status = (char*)"ok";
        d.message = (char*)"";
        rc = lisa_store_replace_doc(p->store, &d, n, vectors, chunks, NULL);
        if (rc == LISA_STORE_OK) {
            if (existed) r->prog.files_updated++;
            else r->prog.files_added++;
            r->prog.chunks_added += n;
            r->prog.chunks_removed += old_chunks;
        }
    }

    for (int64_t i = 0; i < n; i++) {
        if (texts) free(texts[i]);
    }
    free(texts);
    free(vectors);
    free(chunks);
    free(spans);
    doc_text_free(&t);
    free(hash);
    return rc;
}

/* ---- walking ---------------------------------------------------------- */

static int report(run_t* r, const char* path) {
    if (r->p->progress == NULL) return 1;
    r->prog.current = path;
    int keep_going = r->p->progress(r->p->progress_user, &r->prog);
    r->prog.current = NULL;
    return keep_going;
}

/*
 * Folders that hold software, not documents. Indexing them fills a
 * collection with package metadata and READMEs, and near-empty files
 * ("yarl" in a top_level.txt) produce vectors that match any question.
 * Short files elsewhere are kept: a one-line note is a real document.
 * A path given directly by the user is always indexed; these names are
 * only skipped when met while walking into a folder.
 */
static const char* const k_skip_dirs[] = {
    "node_modules", "site-packages", "dist-packages", "__pycache__", ".venv",
    "Pods", ".gradle", ".cargo", ".git", ".svn", NULL,
    /* Names like build, dist, target or vendor are left alone: they are as
     * likely to be someone's folder of documents as a folder of software. */
};

static int in_skipped_dir(const char* path, const char* root) {
    size_t root_len = root ? strlen(root) : 0;
    const char* p = path + (root_len && strncmp(path, root, root_len) == 0 ? root_len : 0);
    for (const char* seg = p; seg && *seg; ) {
        const char* end = strchr(seg + 1, '/');
        if (end == NULL) break;
        size_t len = (size_t)(end - seg) - (*seg == '/' ? 1 : 0);
        const char* name = seg + (*seg == '/' ? 1 : 0);
        for (const char* const* d = k_skip_dirs; *d; d++) {
            if (strlen(*d) == len && strncmp(name, *d, len) == 0) return 1;
        }
        seg = end;
    }
    return 0;
}

/*
 * How the text handed to the embedding model is prepared. Raising this
 * makes the next run re-read every document, because vectors made with
 * an older recipe cannot be compared with new ones.
 *   1: document title, a blank line, then the passage
 *   2: the passage alone (a short passage was otherwise mostly title)
 */
#define EMBED_RECIPE 2

static int visit(void* user, const char* path, const lisa_file_info_t* info) {
    run_t* r = (run_t*)user;
    if (doc_find_extractor(path) == NULL) {
        r->prog.files_skipped++;
        return 0;
    }
    if (in_skipped_dir(path, r->walk_root)) {
        r->prog.files_skipped++;
        return 0;
    }
    char* abs = lisa_path_absolute(path);
    if (abs == NULL) return 0;  /* vanished during the walk */
    r->prog.files_seen++;
    int rc = remember(r, abs);
    if (rc == INGEST_OK) {
        int src = index_file(r, abs, info);
        if (src != LISA_STORE_OK) {
            r->store_rc = src;
            rc = INGEST_ESTORE;
        }
    }
    if (rc == INGEST_OK && !report(r, abs)) {
        r->cancelled = 1;
        rc = INGEST_ECANCELLED;
    }
    free(abs);
    return rc == INGEST_OK ? 0 : 1;  /* non-zero stops the walk */
}

typedef struct {
    run_t*  r;
    char**  gone;
    int64_t n, cap;
} removal_t;

static int collect_gone(void* user, const lisa_store_doc_t* d) {
    removal_t* rm = (removal_t*)user;
    if (was_seen(rm->r, d->source_path)) return 0;
    if (rm->n == rm->cap) {
        int64_t cap = rm->cap ? rm->cap * 2 : 64;
        char** g = (char**)realloc(rm->gone, (size_t)cap * sizeof(char*));
        if (g == NULL) return 1;
        rm->gone = g;
        rm->cap = cap;
    }
    rm->gone[rm->n] = xstrdup(d->doc_id);
    if (rm->gone[rm->n] == NULL) return 1;
    rm->n++;
    return 0;
}

/* Remove documents under dir (absolute) that were not seen in this run. */
static int remove_missing(run_t* r, const char* dir) {
    size_t dl = strlen(dir);
    char* prefix = (char*)malloc(dl + 2);
    if (prefix == NULL) return INGEST_ENOMEM;
    memcpy(prefix, dir, dl);
    prefix[dl] = lisa_path_sep();   /* "/a/b/" so "/a/bc" does not match */
    prefix[dl + 1] = '\0';

    removal_t rm = { r, NULL, 0, 0 };
    int rc = INGEST_OK;
    int src = lisa_store_doc_list(r->p->store, prefix, collect_gone, &rm);
    if (src != LISA_STORE_OK) {
        r->store_rc = src;
        rc = INGEST_ESTORE;
    }
    for (int64_t i = 0; rc == INGEST_OK && i < rm.n; i++) {
        int64_t n = 0;
        src = lisa_store_delete_doc(r->p->store, rm.gone[i], &n);
        if (src != LISA_STORE_OK) {
            r->store_rc = src;
            rc = INGEST_ESTORE;
            break;
        }
        r->prog.files_removed++;
        r->prog.chunks_removed += n;
        if (!report(r, rm.gone[i])) {
            r->cancelled = 1;
            rc = INGEST_ECANCELLED;
        }
    }
    for (int64_t i = 0; i < rm.n; i++) free(rm.gone[i]);
    free(rm.gone);
    free(prefix);
    return rc;
}

/* ---- entry point ------------------------------------------------------ */

int ingest_run(const ingest_params_t* p, const char* const* paths, int64_t n_paths,
               ingest_progress_t* result, int* store_status) {
    if (result) memset(result, 0, sizeof(*result));
    if (store_status) *store_status = LISA_STORE_OK;
    if (p == NULL || p->store == NULL || p->embed == NULL || paths == NULL || n_paths <= 0)
        return INGEST_EINVAL;

    /* Check every path first: a typo must not remove a whole folder. */
    for (int64_t i = 0; i < n_paths; i++) {
        lisa_file_info_t info;
        if (paths[i] == NULL) return INGEST_EINVAL;
        if (lisa_file_info(paths[i], &info) != LISA_PLAT_OK || (!info.is_dir && !info.is_file))
            return INGEST_ENOTFOUND;
    }

    run_t r;
    memset(&r, 0, sizeof(r));
    r.p = p;
    r.dim = lisa_store_dim(p->store);
    /* Vectors from an older recipe cannot be compared with new ones. */
    r.recipe_changed = lisa_store_get_user_version(p->store) != EMBED_RECIPE;
    int rc = INGEST_OK;

    /* 1. Index files. */
    for (int64_t i = 0; rc == INGEST_OK && i < n_paths; i++) {
        lisa_file_info_t info;
        lisa_file_info(paths[i], &info);
        if (info.is_dir) {
            r.walk_root = paths[i];
            int wrc = lisa_dir_walk(paths[i], p->include_hidden, visit, &r);
            r.walk_root = NULL;
            if (r.cancelled) rc = INGEST_ECANCELLED;
            else if (r.store_rc != LISA_STORE_OK) rc = INGEST_ESTORE;
            else if (wrc < 0 && wrc != LISA_PLAT_OK) rc = wrc == LISA_PLAT_ENOMEM ? INGEST_ENOMEM : INGEST_ESTORE;
        } else if (visit(&r, paths[i], &info) != 0) {
            rc = r.cancelled ? INGEST_ECANCELLED : (r.store_rc ? INGEST_ESTORE : INGEST_ENOMEM);
        }
    }

    /* 2. Remove documents whose files disappeared from the given folders. */
    if (rc == INGEST_OK) qsort(r.seen, (size_t)r.n_seen, sizeof(char*), cmp_str);
    for (int64_t i = 0; rc == INGEST_OK && i < n_paths; i++) {
        lisa_file_info_t info;
        lisa_file_info(paths[i], &info);
        if (!info.is_dir) continue;
        char* abs = lisa_path_absolute(paths[i]);
        if (abs == NULL) {
            rc = INGEST_ENOTFOUND;
            break;
        }
        rc = remove_missing(&r, abs);
        free(abs);
    }

    /* A finished run leaves every vector made the same way. */
    if (rc == INGEST_OK && r.recipe_changed && !r.cancelled)
        lisa_store_set_user_version(p->store, EMBED_RECIPE);

    for (int64_t i = 0; i < r.n_seen; i++) free(r.seen[i]);
    free(r.seen);
    if (result) {
        *result = r.prog;
        result->current = NULL;
    }
    if (store_status) *store_status = r.store_rc;
    return rc;
}
