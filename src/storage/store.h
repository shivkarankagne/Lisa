/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_STORE_H
#define LISA_STORE_H

/*
 * LISA Storage v2 — persistent collection of chunk vectors + metadata.
 *
 * Layout of a collection directory:
 *
 *   meta.sqlite            SQLite database (WAL mode). Source of truth for
 *                          which chunks exist, their stable IDs, their
 *                          metadata, and which vector slots are in use.
 *   vectors.<gen>.lisa     Vector file for generation <gen>: 64-byte
 *                          header followed by float32 rows (see
 *                          docs/formats/collection-v2.md). Only the generation
 *                          recorded in meta.sqlite is valid.
 *   write.lock             Advisory lock held by the single writer.
 *
 * Guarantees:
 *
 *   - Every chunk has a stable 64-bit ID, assigned at insert and never
 *     reused or renumbered, including across delete and compaction.
 *   - Crash safety: a process killed at any point leaves the collection
 *     readable at its last committed state. Vectors are written and
 *     synced before the metadata transaction that references them
 *     commits; compaction writes a new generation file and switches to it
 *     in one transaction.
 *   - One writer, many readers. A second writer fails with
 *     LISA_STORE_EBUSY. Readers do not block the writer and see new
 *     commits after lisa_store_refresh().
 *   - The embedding model and dimension are fixed at creation. Opening
 *     with a different expected model fails with LISA_STORE_EMODEL.
 *
 * Threading: a handle must be used by one thread at a time. Use one
 * handle per thread (e.g. a writer handle for ingest, reader handles for
 * queries).
 */

#include <stdint.h>

#define LISA_STORE_FORMAT_VERSION 3   /* database format; older versions are migrated on open */
#define LISA_STORE_VECTOR_VERSION 1   /* vector file format (independent of the database) */

#define LISA_STORE_OK          0
#define LISA_STORE_EINVAL     -1   /* invalid argument */
#define LISA_STORE_EIO        -2   /* file or database I/O error */
#define LISA_STORE_EFORMAT    -3   /* not a v2 collection, corrupt, or newer format */
#define LISA_STORE_EMODEL     -4   /* embedding model differs from expected */
#define LISA_STORE_EBUSY      -5   /* another writer holds the collection */
#define LISA_STORE_ENOTFOUND  -6   /* ID or collection does not exist */
#define LISA_STORE_ENOMEM     -7   /* allocation failure */
#define LISA_STORE_EREADONLY  -8   /* write on a handle opened read-only */
#define LISA_STORE_EEXIST     -9   /* create on an existing path */

#define LISA_STORE_READ   0
#define LISA_STORE_WRITE  1

typedef struct lisa_store lisa_store_t;

/*
 * Metadata for one chunk. On insert, all strings must be non-NULL
 * (empty strings allowed) and are copied. On read (lisa_store_get),
 * strings are allocated and owned by the lisa_store_chunk_t; release with
 * lisa_store_chunk_free().
 */
typedef struct {
    char*   doc_id;        /* caller-defined document key */
    int64_t chunk_index;   /* position of the chunk within its document */
    char*   source_path;   /* file the chunk came from */
    int64_t offset;        /* byte offset of the chunk in the source text */
    int64_t length;        /* byte length of the chunk */
    char*   text;          /* chunk text */
    char*   content_hash;  /* hash of the source document content */
    int64_t page;          /* 1-based page the chunk starts on; 0 if unpaged */
} lisa_store_chunk_t;

/* Free the strings of a chunk returned by lisa_store_get. */
void lisa_store_chunk_free(lisa_store_chunk_t* chunk);

/*
 * Create a new, empty collection at dir (must not exist; parent must).
 * embedding_model: non-empty identifier of the model that produces the
 * vectors. dim: vector dimension, 1..65536.
 */
int lisa_store_create(const char* dir, const char* embedding_model, int64_t dim);

/*
 * Open a collection. mode is LISA_STORE_READ or LISA_STORE_WRITE.
 * expected_model: if non-NULL, must equal the collection's model, else
 * LISA_STORE_EMODEL. On success *out receives a handle to close with
 * lisa_store_close(). A writer also removes stale vector files left by an
 * interrupted compaction.
 */
int lisa_store_open(const char* dir, int mode, const char* expected_model,
                    lisa_store_t** out);

/* Close a handle (releases the writer lock). NULL is ignored. */
void lisa_store_close(lisa_store_t* store);

/* Collection properties. Strings are owned by the handle. */
const char* lisa_store_model(const lisa_store_t* store);

/*
 * A number the caller keeps in the collection, for its own use: ingest
 * records how it prepared the text it embedded, so a collection written
 * by an older LISA can be rebuilt instead of mixing two kinds of vector.
 * Reading an unset key gives 0.
 */
int64_t lisa_store_get_user_version(lisa_store_t* store);
int     lisa_store_set_user_version(lisa_store_t* store, int64_t value);
int64_t     lisa_store_dim(const lisa_store_t* store);
int64_t     lisa_store_count(const lisa_store_t* store);  /* live chunks */

/*
 * Insert count chunks in one transaction. vectors: count * dim floats.
 * chunks: count metadata records. On success, if out_ids is non-NULL it
 * receives the new IDs (count entries). All or nothing.
 */
int lisa_store_insert(lisa_store_t* store, int64_t count, const float* vectors,
                      const lisa_store_chunk_t* chunks, uint64_t* out_ids);

/*
 * Delete chunks by ID in one transaction. All or nothing: if any ID does
 * not exist, nothing is deleted and LISA_STORE_ENOTFOUND is returned.
 */
int lisa_store_delete(lisa_store_t* store, int64_t count, const uint64_t* ids);

/*
 * Delete every chunk whose doc_id equals doc_id, and the document's
 * record (if any), in one transaction. *out_deleted (if non-NULL)
 * receives the number of chunks deleted (may be 0).
 */
int lisa_store_delete_doc(lisa_store_t* store, const char* doc_id,
                          int64_t* out_deleted);

/* Read one chunk's metadata. ENOTFOUND if the ID does not exist. */
int lisa_store_get(lisa_store_t* store, uint64_t id, lisa_store_chunk_t* out);

/*
 * Copy one chunk's vector (dim floats) into out, as of this handle's view
 * (its last refresh or own write). ENOTFOUND if the ID is not live in it.
 */
int lisa_store_get_vector(lisa_store_t* store, uint64_t id, float* out);

/*
 * Pick up commits made through other handles or processes since this
 * handle last looked. Cheap when nothing changed.
 */
int lisa_store_refresh(lisa_store_t* store);

/*
 * Rewrite the vector file without deleted slots. IDs do not change.
 * Writer only. Crash-safe (see header comment).
 */
int lisa_store_compact(lisa_store_t* store);

/*
 * Read-only view for search. vectors holds n_slots rows of dim floats;
 * slot i is a live chunk iff live[i] != 0, and its ID is slot_ids[i].
 * Valid until the next call on this handle that modifies or refreshes
 * it, or until close.
 */
typedef struct {
    const float*    vectors;
    int64_t         n_slots;
    int64_t         dim;
    const uint8_t*  live;
    const uint64_t* slot_ids;
} lisa_store_view_t;

int lisa_store_view(lisa_store_t* store, lisa_store_view_t* out);

/*
 * Convert a storage v1 / v1.1 collection (src_dir, see storage.h) into a
 * new v2 collection at dst_dir. Vector i of the old collection gets ID i,
 * so result indices from the old format equal IDs in the new one. Chunk
 * metadata is empty (doc_id "v1"). src_dir is not modified.
 */
int lisa_store_migrate_v1(const char* src_dir, const char* dst_dir,
                          const char* embedding_model);

/* ---- document records (format v2) ------------------------------------ */

/*
 * What the collection knows about one source document. Used by ingest to
 * skip unchanged files and to report files that produced no text or
 * failed. Strings from lisa_store_doc_get / _list are owned by the
 * record (release with lisa_store_doc_free).
 */
typedef struct {
    char*   doc_id;        /* key; equals the chunks' doc_id */
    char*   source_path;   /* absolute path of the file */
    char*   content_hash;  /* hash of the file bytes */
    int64_t size;          /* bytes, when last indexed or checked */
    int64_t mtime_ns;      /* modification time, when last indexed or checked */
    char*   title;         /* "" if none */
    int64_t chunk_count;   /* chunks stored for this document */
    char*   status;        /* "ok", "no_text", or "error" */
    char*   message;       /* detail for "error"; "" otherwise */
} lisa_store_doc_t;

void lisa_store_doc_free(lisa_store_doc_t* doc);

/*
 * Replace everything stored for doc->doc_id in one transaction: delete
 * its old chunks, insert count new chunks (their doc_id is set to
 * doc->doc_id; count may be 0), and write the document record with
 * chunk_count = count. A crash leaves either the old or the new version.
 */
int lisa_store_replace_doc(lisa_store_t* store, const lisa_store_doc_t* doc, int64_t count,
                           const float* vectors, const lisa_store_chunk_t* chunks,
                           uint64_t* out_ids);

/* Update a document's size and mtime (content unchanged). ENOTFOUND if unknown. */
int lisa_store_doc_touch(lisa_store_t* store, const char* doc_id, int64_t size, int64_t mtime_ns);

/* Read one document record. ENOTFOUND if unknown. */
int lisa_store_doc_get(lisa_store_t* store, const char* doc_id, lisa_store_doc_t* out);

/* Return non-zero to stop the listing. doc is valid only during the call. */
typedef int (*lisa_store_doc_fn)(void* user, const lisa_store_doc_t* doc);

/*
 * Visit document records whose source_path starts with path_prefix (all
 * records if NULL), ordered by source_path.
 */
int lisa_store_doc_list(lisa_store_t* store, const char* path_prefix,
                        lisa_store_doc_fn fn, void* user);

/* ---- keyword search (format v3) --------------------------------------- */

typedef struct {
    uint64_t id;     /* chunk ID */
    int64_t  slot;   /* its slot in the database's current state */
    double   score;  /* BM25 relevance; higher is better */
} lisa_store_kw_hit_t;

/*
 * Full-text search of chunk text (FTS5, BM25). Each word of `text` is
 * matched as a term (any word may match); punctuation and query syntax in
 * `text` are treated literally. Optional path_prefix restricts results
 * to documents whose source_path starts with it. Writes up to `limit`
 * hits into out, best first; *n_out receives the count (0 if `text` has
 * nothing searchable).
 */
int lisa_store_keyword_search(lisa_store_t* store, const char* text, const char* path_prefix,
                              int64_t limit, lisa_store_kw_hit_t* out, int64_t* n_out);

/*
 * Set mask[slot] = 1 for live slots (in this handle's view) whose chunk
 * comes from a document under path_prefix, and 0 elsewhere. mask has
 * n_slots entries (lisa_store_view).
 */
int lisa_store_prefix_mask(lisa_store_t* store, const char* path_prefix, uint8_t* mask);

#endif /* LISA_STORE_H */
