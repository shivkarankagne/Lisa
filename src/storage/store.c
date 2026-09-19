/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * LISA Storage v2. See store.h for the contract and
 * docs/formats/collection-v2.md for the on-disk format.
 */

#include "store.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"
#include "../platform/platform.h"

#define VEC_HEADER_SIZE  64
#define VEC_MAGIC        "LISAVECT"
#define VEC_BOM          0x01020304u
#define MAX_DIM          65536
#define MIGRATE_BATCH    4096
#define DEAD_ID          UINT64_MAX
#define E_MISSING        -100   /* internal: vector file missing (retry) */
#define MAP_RETRIES      3

struct lisa_store {
    char*        dir;
    int          mode;
    sqlite3*     db;
    lisa_lock_t* lock;       /* writer only */
    FILE*        vf;         /* writer only: current vector file, r+b */

    char*        model;
    int64_t      dim;

    /* State as of this handle's last look at the database. */
    int64_t      gen;
    int64_t      slot_count;
    int64_t      change;

    lisa_map_t*  map;        /* read-only mapping of the vector file */
    uint8_t*     live;       /* slot_count entries */
    uint64_t*    slot_ids;   /* slot_count entries; DEAD_ID if dead */
    int64_t      cap;
    int64_t      live_count;
};

/* ---- small helpers ---------------------------------------------------- */

static int host_is_little_endian(void) {
    const uint32_t x = 1;
    return *(const uint8_t*)&x == 1;
}

static void put_u32(unsigned char* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static void put_u64(unsigned char* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static uint32_t get_u32(const unsigned char* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint64_t get_u64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static char* xstrdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char* vec_path(const char* dir, int64_t gen) {
    char name[64];
    snprintf(name, sizeof(name), "vectors.%lld.lisa", (long long)gen);
    return lisa_path_join(dir, name);
}

static int64_t row_bytes(int64_t dim) {
    return dim * (int64_t)sizeof(float);
}

/* ---- vector file ------------------------------------------------------ */

static int write_vec_header(FILE* f, int64_t dim, int64_t gen) {
    unsigned char h[VEC_HEADER_SIZE];
    memset(h, 0, sizeof(h));
    memcpy(h, VEC_MAGIC, 8);
    put_u32(h + 8, LISA_STORE_FORMAT_VERSION);
    put_u32(h + 12, VEC_BOM);
    put_u64(h + 16, (uint64_t)dim);
    put_u64(h + 24, (uint64_t)gen);
    return fwrite(h, 1, sizeof(h), f) == sizeof(h) ? LISA_STORE_OK : LISA_STORE_EIO;
}

static int check_vec_header(const unsigned char* h, int64_t size, int64_t dim, int64_t gen) {
    if (size < VEC_HEADER_SIZE) return LISA_STORE_EFORMAT;
    if (memcmp(h, VEC_MAGIC, 8) != 0) return LISA_STORE_EFORMAT;
    if (get_u32(h + 8) != LISA_STORE_FORMAT_VERSION) return LISA_STORE_EFORMAT;
    if (get_u32(h + 12) != VEC_BOM) return LISA_STORE_EFORMAT;
    if ((int64_t)get_u64(h + 16) != dim) return LISA_STORE_EFORMAT;
    if ((int64_t)get_u64(h + 24) != gen) return LISA_STORE_EFORMAT;
    return LISA_STORE_OK;
}

/* Create vectors.<gen>.lisa with only a header, synced. */
static int create_vec_file(const char* dir, int64_t dim, int64_t gen) {
    char* path = vec_path(dir, gen);
    if (path == NULL) return LISA_STORE_ENOMEM;
    FILE* f = fopen(path, "wb");
    free(path);
    if (f == NULL) return LISA_STORE_EIO;
    int rc = write_vec_header(f, dim, gen);
    if (rc == LISA_STORE_OK && lisa_file_sync(f) != LISA_PLAT_OK) rc = LISA_STORE_EIO;
    if (fclose(f) != 0 && rc == LISA_STORE_OK) rc = LISA_STORE_EIO;
    return rc;
}

/* Map the current generation's file if the mapping is missing or short. */
static int ensure_map(lisa_store_t* s) {
    int64_t need = VEC_HEADER_SIZE + s->slot_count * row_bytes(s->dim);
    if (s->map != NULL && lisa_map_size(s->map) >= need) return LISA_STORE_OK;

    lisa_unmap(s->map);
    s->map = NULL;
    char* path = vec_path(s->dir, s->gen);
    if (path == NULL) return LISA_STORE_ENOMEM;
    int prc = lisa_map_file(path, 0, &s->map);
    free(path);
    if (prc == LISA_PLAT_ENOENT) return E_MISSING;
    if (prc != LISA_PLAT_OK) return LISA_STORE_EIO;

    int rc = check_vec_header((const unsigned char*)lisa_map_data(s->map),
                              lisa_map_size(s->map), s->dim, s->gen);
    if (rc == LISA_STORE_OK && lisa_map_size(s->map) < need) rc = LISA_STORE_EFORMAT;
    if (rc != LISA_STORE_OK) {
        lisa_unmap(s->map);
        s->map = NULL;
    }
    return rc;
}

static const float* slot_row(const lisa_store_t* s, int64_t slot) {
    const unsigned char* base = (const unsigned char*)lisa_map_data(s->map);
    return (const float*)(base + VEC_HEADER_SIZE + slot * row_bytes(s->dim));
}

static int load_state(lisa_store_t* s);
static int exec(sqlite3* db, const char* sql);

/*
 * ensure_map, and if the vector file has disappeared (another process
 * compacted and removed it), reload the committed state and try again.
 */
static int map_current(lisa_store_t* s) {
    int rc = ensure_map(s);
    for (int i = 0; rc == E_MISSING && i < MAP_RETRIES; i++) {
        rc = exec(s->db, "BEGIN;");
        if (rc != LISA_STORE_OK) return rc;
        rc = load_state(s);
        exec(s->db, "COMMIT;");
        if (rc == LISA_STORE_OK) rc = ensure_map(s);
    }
    return rc == E_MISSING ? LISA_STORE_EFORMAT : rc;
}

static int open_writer_file(lisa_store_t* s) {
    if (s->vf) fclose(s->vf);
    s->vf = NULL;
    char* path = vec_path(s->dir, s->gen);
    if (path == NULL) return LISA_STORE_ENOMEM;
    s->vf = fopen(path, "r+b");
    free(path);
    return s->vf ? LISA_STORE_OK : LISA_STORE_EFORMAT;
}

/* ---- SQLite helpers --------------------------------------------------- */

static int exec(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? LISA_STORE_OK : LISA_STORE_EIO;
}

static void rollback(sqlite3* db) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
}

static int meta_get_int(sqlite3* db, const char* key, int64_t* out) {
    sqlite3_stmt* st = NULL;
    int rc = LISA_STORE_EFORMAT;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = ?;", -1, &st, NULL) != SQLITE_OK)
        return LISA_STORE_EFORMAT;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER) {
        *out = sqlite3_column_int64(st, 0);
        rc = LISA_STORE_OK;
    }
    sqlite3_finalize(st);
    return rc;
}

static int meta_get_text(sqlite3* db, const char* key, char** out) {
    sqlite3_stmt* st = NULL;
    int rc = LISA_STORE_EFORMAT;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = ?;", -1, &st, NULL) != SQLITE_OK)
        return LISA_STORE_EFORMAT;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_TEXT) {
        *out = xstrdup((const char*)sqlite3_column_text(st, 0));
        rc = *out ? LISA_STORE_OK : LISA_STORE_ENOMEM;
    }
    sqlite3_finalize(st);
    return rc;
}

static int meta_set_int(sqlite3* db, const char* key, int64_t value) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?);",
                           -1, &st, NULL) != SQLITE_OK)
        return LISA_STORE_EIO;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, value);
    int rc = sqlite3_step(st) == SQLITE_DONE ? LISA_STORE_OK : LISA_STORE_EIO;
    sqlite3_finalize(st);
    return rc;
}

static int meta_set_text(sqlite3* db, const char* key, const char* value) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?);",
                           -1, &st, NULL) != SQLITE_OK)
        return LISA_STORE_EIO;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st) == SQLITE_DONE ? LISA_STORE_OK : LISA_STORE_EIO;
    sqlite3_finalize(st);
    return rc;
}

static int open_db(const char* dir, sqlite3** out) {
    char* path = lisa_path_join(dir, "meta.sqlite");
    if (path == NULL) return LISA_STORE_ENOMEM;
    sqlite3* db = NULL;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    free(path);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return LISA_STORE_EIO;
    }
    sqlite3_busy_timeout(db, 10000);
    if (exec(db, "PRAGMA journal_mode = WAL;") != LISA_STORE_OK ||
        exec(db, "PRAGMA synchronous = FULL;") != LISA_STORE_OK ||
        exec(db, "PRAGMA foreign_keys = ON;") != LISA_STORE_OK) {
        sqlite3_close(db);
        return LISA_STORE_EIO;
    }
    *out = db;
    return LISA_STORE_OK;
}

static const char* k_schema =
    "CREATE TABLE meta ("
    "  key   TEXT PRIMARY KEY,"
    "  value"
    ");"
    "CREATE TABLE chunks ("
    "  id           INTEGER PRIMARY KEY,"
    "  slot         INTEGER NOT NULL UNIQUE,"
    "  doc_id       TEXT    NOT NULL,"
    "  chunk_index  INTEGER NOT NULL,"
    "  source_path  TEXT    NOT NULL,"
    "  src_offset   INTEGER NOT NULL,"
    "  src_length   INTEGER NOT NULL,"
    "  text         TEXT    NOT NULL,"
    "  content_hash TEXT    NOT NULL"
    ");"
    "CREATE INDEX chunks_doc_id ON chunks(doc_id);";

/* ---- in-memory slot table --------------------------------------------- */

static int reserve_slots(lisa_store_t* s, int64_t n) {
    if (n <= s->cap) return LISA_STORE_OK;
    int64_t cap = s->cap ? s->cap : 1024;
    while (cap < n) cap *= 2;
    uint8_t* live = (uint8_t*)realloc(s->live, (size_t)cap);
    if (live == NULL) return LISA_STORE_ENOMEM;
    s->live = live;
    uint64_t* ids = (uint64_t*)realloc(s->slot_ids, (size_t)cap * sizeof(uint64_t));
    if (ids == NULL) return LISA_STORE_ENOMEM;
    s->slot_ids = ids;
    s->cap = cap;
    return LISA_STORE_OK;
}

/* Rebuild the slot table from the database. Call inside a read txn. */
static int load_slots(lisa_store_t* s, int64_t slot_count) {
    int rc = reserve_slots(s, slot_count);
    if (rc != LISA_STORE_OK) return rc;
    if (slot_count > 0) {
        memset(s->live, 0, (size_t)slot_count);
        for (int64_t i = 0; i < slot_count; i++) s->slot_ids[i] = DEAD_ID;
    }
    s->live_count = 0;

    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT id, slot FROM chunks;", -1, &st, NULL) != SQLITE_OK)
        return LISA_STORE_EIO;
    int step;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(st, 0);
        int64_t slot = sqlite3_column_int64(st, 1);
        if (slot < 0 || slot >= slot_count || id < 0) {
            sqlite3_finalize(st);
            return LISA_STORE_EFORMAT;
        }
        s->live[slot] = 1;
        s->slot_ids[slot] = (uint64_t)id;
        s->live_count++;
    }
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) return LISA_STORE_EIO;
    s->slot_count = slot_count;
    return LISA_STORE_OK;
}

/* Read gen/slot_count/change and reload slots, inside a read txn. */
static int load_state(lisa_store_t* s) {
    int64_t gen, slot_count, change;
    int rc = meta_get_int(s->db, "vector_gen", &gen);
    if (rc == LISA_STORE_OK) rc = meta_get_int(s->db, "slot_count", &slot_count);
    if (rc == LISA_STORE_OK) rc = meta_get_int(s->db, "change_counter", &change);
    if (rc != LISA_STORE_OK) return rc;
    if (gen < 0 || slot_count < 0) return LISA_STORE_EFORMAT;

    rc = load_slots(s, slot_count);
    if (rc != LISA_STORE_OK) return rc;
    if (gen != s->gen) {
        lisa_unmap(s->map);
        s->map = NULL;
    }
    s->gen = gen;
    s->change = change;
    return LISA_STORE_OK;
}

/* ---- public API ------------------------------------------------------- */

void lisa_store_chunk_free(lisa_store_chunk_t* c) {
    if (c == NULL) return;
    free(c->doc_id);
    free(c->source_path);
    free(c->text);
    free(c->content_hash);
    memset(c, 0, sizeof(*c));
}

int lisa_store_create(const char* dir, const char* embedding_model, int64_t dim) {
    if (dir == NULL || dir[0] == '\0' || embedding_model == NULL || embedding_model[0] == '\0')
        return LISA_STORE_EINVAL;
    if (dim < 1 || dim > MAX_DIM) return LISA_STORE_EINVAL;
    if (!host_is_little_endian()) return LISA_STORE_EFORMAT;
    if (lisa_path_exists(dir)) return LISA_STORE_EEXIST;

    /*
     * Build the collection in a temporary sibling directory and rename it
     * into place, so a crash never leaves a half-created collection at dir.
     */
    char suffix[48];
    snprintf(suffix, sizeof(suffix), ".creating-%lld", (long long)lisa_time_monotonic_ns());
    size_t tl = strlen(dir) + strlen(suffix) + 1;
    char* tmp = (char*)malloc(tl);
    if (tmp == NULL) return LISA_STORE_ENOMEM;
    snprintf(tmp, tl, "%s%s", dir, suffix);

    int rc = lisa_mkdir(tmp) == LISA_PLAT_OK ? LISA_STORE_OK : LISA_STORE_EIO;
    sqlite3* db = NULL;
    if (rc == LISA_STORE_OK) rc = create_vec_file(tmp, dim, 0);
    if (rc == LISA_STORE_OK) rc = open_db(tmp, &db);
    if (rc == LISA_STORE_OK) {
        rc = exec(db, "BEGIN IMMEDIATE;");
        if (rc == LISA_STORE_OK) rc = exec(db, k_schema);
        if (rc == LISA_STORE_OK) rc = meta_set_int(db, "format_version", LISA_STORE_FORMAT_VERSION);
        if (rc == LISA_STORE_OK) rc = meta_set_text(db, "embedding_model", embedding_model);
        if (rc == LISA_STORE_OK) rc = meta_set_int(db, "dim", dim);
        if (rc == LISA_STORE_OK) rc = meta_set_int(db, "next_id", 0);
        if (rc == LISA_STORE_OK) rc = meta_set_int(db, "slot_count", 0);
        if (rc == LISA_STORE_OK) rc = meta_set_int(db, "vector_gen", 0);
        if (rc == LISA_STORE_OK) rc = meta_set_int(db, "change_counter", 0);
        if (rc == LISA_STORE_OK) rc = exec(db, "COMMIT;");
        else rollback(db);
        /* Checkpoint so the directory is self-contained before the rename. */
        if (rc == LISA_STORE_OK) sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
        sqlite3_close(db);
    }
    if (rc == LISA_STORE_OK && lisa_rename_replace(tmp, dir) != LISA_PLAT_OK) {
        rc = lisa_path_exists(dir) ? LISA_STORE_EEXIST : LISA_STORE_EIO;
    }
    free(tmp);
    return rc;
}

int lisa_store_open(const char* dir, int mode, const char* expected_model,
                    lisa_store_t** out) {
    if (dir == NULL || out == NULL) return LISA_STORE_EINVAL;
    if (mode != LISA_STORE_READ && mode != LISA_STORE_WRITE) return LISA_STORE_EINVAL;
    *out = NULL;
    if (!host_is_little_endian()) return LISA_STORE_EFORMAT;
    if (!lisa_path_is_dir(dir)) return LISA_STORE_ENOTFOUND;

    char* db_path = lisa_path_join(dir, "meta.sqlite");
    if (db_path == NULL) return LISA_STORE_ENOMEM;
    int has_db = lisa_path_exists(db_path);
    free(db_path);
    if (!has_db) return LISA_STORE_EFORMAT;

    lisa_store_t* s = (lisa_store_t*)calloc(1, sizeof(*s));
    if (s == NULL) return LISA_STORE_ENOMEM;
    s->mode = mode;
    s->gen = -1;
    s->dir = xstrdup(dir);
    if (s->dir == NULL) {
        free(s);
        return LISA_STORE_ENOMEM;
    }

    int rc = LISA_STORE_OK;
    if (mode == LISA_STORE_WRITE) {
        char* lock_path = lisa_path_join(dir, "write.lock");
        if (lock_path == NULL) rc = LISA_STORE_ENOMEM;
        else {
            int lrc = lisa_lock_acquire(lock_path, 0, &s->lock);
            free(lock_path);
            if (lrc == LISA_PLAT_EBUSY) rc = LISA_STORE_EBUSY;
            else if (lrc != LISA_PLAT_OK) rc = LISA_STORE_EIO;
        }
    }
    if (rc == LISA_STORE_OK) rc = open_db(dir, &s->db);

    if (rc == LISA_STORE_OK) {
        rc = exec(s->db, "BEGIN;");
        if (rc == LISA_STORE_OK) {
            int64_t version = 0;
            rc = meta_get_int(s->db, "format_version", &version);
            if (rc == LISA_STORE_OK && version != LISA_STORE_FORMAT_VERSION) rc = LISA_STORE_EFORMAT;
            if (rc == LISA_STORE_OK) rc = meta_get_text(s->db, "embedding_model", &s->model);
            if (rc == LISA_STORE_OK) rc = meta_get_int(s->db, "dim", &s->dim);
            if (rc == LISA_STORE_OK && (s->dim < 1 || s->dim > MAX_DIM)) rc = LISA_STORE_EFORMAT;
            if (rc == LISA_STORE_OK) rc = load_state(s);
            exec(s->db, "COMMIT;");
        }
    }
    if (rc == LISA_STORE_OK && expected_model != NULL && strcmp(expected_model, s->model) != 0)
        rc = LISA_STORE_EMODEL;
    if (rc == LISA_STORE_OK) rc = map_current(s);

    if (rc == LISA_STORE_OK && mode == LISA_STORE_WRITE) {
        rc = open_writer_file(s);
        /* Remove files an interrupted compaction may have left behind. */
        for (int64_t g = s->gen - 1; rc == LISA_STORE_OK && g <= s->gen + 1; g += 2) {
            if (g < 0) continue;
            char* p = vec_path(dir, g);
            if (p) {
                lisa_remove_file(p);
                free(p);
            }
        }
    }

    if (rc != LISA_STORE_OK) {
        lisa_store_close(s);
        return rc;
    }
    *out = s;
    return LISA_STORE_OK;
}

void lisa_store_close(lisa_store_t* s) {
    if (s == NULL) return;
    if (s->vf) fclose(s->vf);
    lisa_unmap(s->map);
    if (s->db) sqlite3_close(s->db);
    lisa_lock_release(s->lock);
    free(s->live);
    free(s->slot_ids);
    free(s->model);
    free(s->dir);
    free(s);
}

const char* lisa_store_model(const lisa_store_t* s) {
    return s ? s->model : NULL;
}

int64_t lisa_store_dim(const lisa_store_t* s) {
    return s ? s->dim : 0;
}

int64_t lisa_store_count(const lisa_store_t* s) {
    return s ? s->live_count : 0;
}

static int chunk_valid(const lisa_store_chunk_t* c) {
    return c->doc_id && c->source_path && c->text && c->content_hash &&
           c->chunk_index >= 0 && c->offset >= 0 && c->length >= 0;
}

int lisa_store_insert(lisa_store_t* s, int64_t count, const float* vectors,
                      const lisa_store_chunk_t* chunks, uint64_t* out_ids) {
    if (s == NULL || count <= 0 || vectors == NULL || chunks == NULL) return LISA_STORE_EINVAL;
    if (s->mode != LISA_STORE_WRITE) return LISA_STORE_EREADONLY;
    for (int64_t i = 0; i < count; i++) {
        if (!chunk_valid(&chunks[i])) return LISA_STORE_EINVAL;
    }

    int rc = exec(s->db, "BEGIN IMMEDIATE;");
    if (rc != LISA_STORE_OK) return rc;

    int64_t slot_count = 0, next_id = 0, change = 0;
    rc = meta_get_int(s->db, "slot_count", &slot_count);
    if (rc == LISA_STORE_OK) rc = meta_get_int(s->db, "next_id", &next_id);
    if (rc == LISA_STORE_OK) rc = meta_get_int(s->db, "change_counter", &change);
    if (rc == LISA_STORE_OK) rc = reserve_slots(s, slot_count + count);

    /* 1. Vectors beyond the committed end, synced before the commit. */
    if (rc == LISA_STORE_OK) {
        int64_t off = VEC_HEADER_SIZE + slot_count * row_bytes(s->dim);
        size_t bytes = (size_t)(count * row_bytes(s->dim));
        if (lisa_file_seek(s->vf, off) != LISA_PLAT_OK ||
            fwrite(vectors, 1, bytes, s->vf) != bytes ||
            lisa_file_sync(s->vf) != LISA_PLAT_OK)
            rc = LISA_STORE_EIO;
    }

    /* 2. Metadata referencing them, in the same transaction as the counters. */
    sqlite3_stmt* st = NULL;
    if (rc == LISA_STORE_OK &&
        sqlite3_prepare_v2(s->db,
            "INSERT INTO chunks(id, slot, doc_id, chunk_index, source_path,"
            " src_offset, src_length, text, content_hash)"
            " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);", -1, &st, NULL) != SQLITE_OK)
        rc = LISA_STORE_EIO;
    for (int64_t i = 0; rc == LISA_STORE_OK && i < count; i++) {
        const lisa_store_chunk_t* c = &chunks[i];
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, next_id + i);
        sqlite3_bind_int64(st, 2, slot_count + i);
        sqlite3_bind_text(st, 3, c->doc_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 4, c->chunk_index);
        sqlite3_bind_text(st, 5, c->source_path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 6, c->offset);
        sqlite3_bind_int64(st, 7, c->length);
        sqlite3_bind_text(st, 8, c->text, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 9, c->content_hash, -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_DONE) rc = LISA_STORE_EIO;
    }
    sqlite3_finalize(st);

    if (rc == LISA_STORE_OK) rc = meta_set_int(s->db, "slot_count", slot_count + count);
    if (rc == LISA_STORE_OK) rc = meta_set_int(s->db, "next_id", next_id + count);
    if (rc == LISA_STORE_OK) rc = meta_set_int(s->db, "change_counter", change + 1);
    if (rc == LISA_STORE_OK) rc = exec(s->db, "COMMIT;");
    if (rc != LISA_STORE_OK) {
        rollback(s->db);
        return rc;
    }

    /* Committed: update this handle's view. */
    for (int64_t i = 0; i < count; i++) {
        s->live[slot_count + i] = 1;
        s->slot_ids[slot_count + i] = (uint64_t)(next_id + i);
        if (out_ids) out_ids[i] = (uint64_t)(next_id + i);
    }
    s->slot_count = slot_count + count;
    s->live_count += count;
    s->change = change + 1;
    return LISA_STORE_OK;
}

/* Mark slots dead in this handle's view after a committed delete. */
static void kill_slots(lisa_store_t* s, const int64_t* slots, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        if (slots[i] < s->slot_count && s->live[slots[i]]) {
            s->live[slots[i]] = 0;
            s->slot_ids[slots[i]] = DEAD_ID;
            s->live_count--;
        }
    }
}

int lisa_store_delete(lisa_store_t* s, int64_t count, const uint64_t* ids) {
    if (s == NULL || count <= 0 || ids == NULL) return LISA_STORE_EINVAL;
    if (s->mode != LISA_STORE_WRITE) return LISA_STORE_EREADONLY;

    int64_t* slots = (int64_t*)malloc((size_t)count * sizeof(int64_t));
    if (slots == NULL) return LISA_STORE_ENOMEM;

    int rc = exec(s->db, "BEGIN IMMEDIATE;");
    sqlite3_stmt* sel = NULL;
    sqlite3_stmt* del = NULL;
    if (rc == LISA_STORE_OK &&
        (sqlite3_prepare_v2(s->db, "SELECT slot FROM chunks WHERE id = ?;", -1, &sel, NULL) != SQLITE_OK ||
         sqlite3_prepare_v2(s->db, "DELETE FROM chunks WHERE id = ?;", -1, &del, NULL) != SQLITE_OK))
        rc = LISA_STORE_EIO;

    for (int64_t i = 0; rc == LISA_STORE_OK && i < count; i++) {
        if (ids[i] > (uint64_t)INT64_MAX) {
            rc = LISA_STORE_ENOTFOUND;
            break;
        }
        sqlite3_reset(sel);
        sqlite3_bind_int64(sel, 1, (int64_t)ids[i]);
        if (sqlite3_step(sel) != SQLITE_ROW) {
            rc = LISA_STORE_ENOTFOUND;
            break;
        }
        slots[i] = sqlite3_column_int64(sel, 0);
        sqlite3_reset(del);
        sqlite3_bind_int64(del, 1, (int64_t)ids[i]);
        if (sqlite3_step(del) != SQLITE_DONE) rc = LISA_STORE_EIO;
    }
    sqlite3_finalize(sel);
    sqlite3_finalize(del);

    int64_t change = 0;
    if (rc == LISA_STORE_OK) rc = meta_get_int(s->db, "change_counter", &change);
    if (rc == LISA_STORE_OK) rc = meta_set_int(s->db, "change_counter", change + 1);
    if (rc == LISA_STORE_OK) rc = exec(s->db, "COMMIT;");
    if (rc != LISA_STORE_OK) {
        rollback(s->db);
        free(slots);
        return rc;
    }
    kill_slots(s, slots, count);
    s->change = change + 1;
    free(slots);
    return LISA_STORE_OK;
}

int lisa_store_delete_doc(lisa_store_t* s, const char* doc_id, int64_t* out_deleted) {
    if (s == NULL || doc_id == NULL) return LISA_STORE_EINVAL;
    if (s->mode != LISA_STORE_WRITE) return LISA_STORE_EREADONLY;
    if (out_deleted) *out_deleted = 0;

    int rc = exec(s->db, "BEGIN IMMEDIATE;");
    if (rc != LISA_STORE_OK) return rc;

    int64_t n = 0, cap = 64;
    int64_t* slots = (int64_t*)malloc((size_t)cap * sizeof(int64_t));
    if (slots == NULL) rc = LISA_STORE_ENOMEM;

    sqlite3_stmt* st = NULL;
    if (rc == LISA_STORE_OK &&
        sqlite3_prepare_v2(s->db, "SELECT slot FROM chunks WHERE doc_id = ?;", -1, &st, NULL) != SQLITE_OK)
        rc = LISA_STORE_EIO;
    if (rc == LISA_STORE_OK) {
        sqlite3_bind_text(st, 1, doc_id, -1, SQLITE_STATIC);
        int step;
        while (rc == LISA_STORE_OK && (step = sqlite3_step(st)) == SQLITE_ROW) {
            if (n == cap) {
                int64_t* grown = (int64_t*)realloc(slots, (size_t)cap * 2 * sizeof(int64_t));
                if (grown == NULL) {
                    rc = LISA_STORE_ENOMEM;
                    break;
                }
                slots = grown;
                cap *= 2;
            }
            slots[n++] = sqlite3_column_int64(st, 0);
        }
    }
    sqlite3_finalize(st);
    st = NULL;

    if (rc == LISA_STORE_OK && n > 0) {
        if (sqlite3_prepare_v2(s->db, "DELETE FROM chunks WHERE doc_id = ?;", -1, &st, NULL) != SQLITE_OK)
            rc = LISA_STORE_EIO;
        else {
            sqlite3_bind_text(st, 1, doc_id, -1, SQLITE_STATIC);
            if (sqlite3_step(st) != SQLITE_DONE) rc = LISA_STORE_EIO;
        }
        sqlite3_finalize(st);
    }

    int64_t change = 0;
    if (rc == LISA_STORE_OK) rc = meta_get_int(s->db, "change_counter", &change);
    if (rc == LISA_STORE_OK) rc = meta_set_int(s->db, "change_counter", change + 1);
    if (rc == LISA_STORE_OK) rc = exec(s->db, "COMMIT;");
    if (rc != LISA_STORE_OK) {
        rollback(s->db);
        free(slots);
        return rc;
    }
    kill_slots(s, slots, n);
    s->change = change + 1;
    if (out_deleted) *out_deleted = n;
    free(slots);
    return LISA_STORE_OK;
}

int lisa_store_get(lisa_store_t* s, uint64_t id, lisa_store_chunk_t* out) {
    if (s == NULL || out == NULL) return LISA_STORE_EINVAL;
    memset(out, 0, sizeof(*out));
    if (id > (uint64_t)INT64_MAX) return LISA_STORE_ENOTFOUND;

    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT doc_id, chunk_index, source_path, src_offset, src_length, text,"
            " content_hash FROM chunks WHERE id = ?;", -1, &st, NULL) != SQLITE_OK)
        return LISA_STORE_EIO;
    sqlite3_bind_int64(st, 1, (int64_t)id);
    int rc = LISA_STORE_ENOTFOUND;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->doc_id = xstrdup((const char*)sqlite3_column_text(st, 0));
        out->chunk_index = sqlite3_column_int64(st, 1);
        out->source_path = xstrdup((const char*)sqlite3_column_text(st, 2));
        out->offset = sqlite3_column_int64(st, 3);
        out->length = sqlite3_column_int64(st, 4);
        out->text = xstrdup((const char*)sqlite3_column_text(st, 5));
        out->content_hash = xstrdup((const char*)sqlite3_column_text(st, 6));
        rc = (out->doc_id && out->source_path && out->text && out->content_hash)
                 ? LISA_STORE_OK : LISA_STORE_ENOMEM;
        if (rc != LISA_STORE_OK) lisa_store_chunk_free(out);
    }
    sqlite3_finalize(st);
    return rc;
}

int lisa_store_get_vector(lisa_store_t* s, uint64_t id, float* out) {
    if (s == NULL || out == NULL) return LISA_STORE_EINVAL;
    if (id > (uint64_t)INT64_MAX) return LISA_STORE_ENOTFOUND;

    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT slot FROM chunks WHERE id = ?;", -1, &st, NULL) != SQLITE_OK)
        return LISA_STORE_EIO;
    sqlite3_bind_int64(st, 1, (int64_t)id);
    int64_t slot = -1;
    if (sqlite3_step(st) == SQLITE_ROW) slot = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    /*
     * Answer from this handle's view, so the result matches its mapping.
     * The database slot is the fast path; if another handle has changed
     * the collection since this one last refreshed (e.g. compaction
     * renumbered slots), find the ID in the view instead.
     */
    if (slot < 0 || slot >= s->slot_count || !s->live[slot] || s->slot_ids[slot] != id) {
        slot = -1;
        for (int64_t i = 0; i < s->slot_count; i++) {
            if (s->live[i] && s->slot_ids[i] == id) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return LISA_STORE_ENOTFOUND;
    }
    int rc = map_current(s);
    if (rc != LISA_STORE_OK) return rc;
    memcpy(out, slot_row(s, slot), (size_t)row_bytes(s->dim));
    return LISA_STORE_OK;
}

int lisa_store_refresh(lisa_store_t* s) {
    if (s == NULL) return LISA_STORE_EINVAL;
    int rc = exec(s->db, "BEGIN;");
    if (rc != LISA_STORE_OK) return rc;
    int64_t change = 0;
    rc = meta_get_int(s->db, "change_counter", &change);
    if (rc == LISA_STORE_OK && change != s->change) {
        int64_t old_gen = s->gen;
        rc = load_state(s);
        if (rc == LISA_STORE_OK && s->mode == LISA_STORE_WRITE && s->gen != old_gen)
            rc = open_writer_file(s);
    }
    exec(s->db, "COMMIT;");
    return rc;
}

int lisa_store_compact(lisa_store_t* s) {
    if (s == NULL) return LISA_STORE_EINVAL;
    if (s->mode != LISA_STORE_WRITE) return LISA_STORE_EREADONLY;

    int rc = exec(s->db, "BEGIN IMMEDIATE;");
    if (rc != LISA_STORE_OK) return rc;

    /* Make sure this handle reflects the committed state. */
    int64_t change = 0;
    rc = meta_get_int(s->db, "change_counter", &change);
    if (rc == LISA_STORE_OK && change != s->change) rc = load_state(s);
    if (rc == LISA_STORE_OK) rc = ensure_map(s);
    if (rc == E_MISSING) rc = LISA_STORE_EFORMAT;  /* writer: no one else compacts */

    int64_t old_gen = s->gen, new_gen = s->gen + 1;
    char* new_path = vec_path(s->dir, new_gen);
    char* old_path = vec_path(s->dir, old_gen);
    if (rc == LISA_STORE_OK && (new_path == NULL || old_path == NULL)) rc = LISA_STORE_ENOMEM;

    FILE* nf = NULL;
    if (rc == LISA_STORE_OK) {
        nf = fopen(new_path, "wb");
        if (nf == NULL) rc = LISA_STORE_EIO;
        else rc = write_vec_header(nf, s->dim, new_gen);
    }

    /* Copy live rows in slot order; renumber slots 0..live-1. */
    sqlite3_stmt* sel = NULL;
    sqlite3_stmt* upd = NULL;
    if (rc == LISA_STORE_OK &&
        (sqlite3_prepare_v2(s->db, "SELECT id, slot FROM chunks ORDER BY slot;", -1, &sel, NULL) != SQLITE_OK ||
         sqlite3_prepare_v2(s->db, "UPDATE chunks SET slot = ? WHERE id = ?;", -1, &upd, NULL) != SQLITE_OK))
        rc = LISA_STORE_EIO;
    int64_t live = 0;
    while (rc == LISA_STORE_OK) {
        int step = sqlite3_step(sel);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = LISA_STORE_EIO;
            break;
        }
        int64_t id = sqlite3_column_int64(sel, 0);
        int64_t slot = sqlite3_column_int64(sel, 1);
        if (slot < 0 || slot >= s->slot_count) {
            rc = LISA_STORE_EFORMAT;
            break;
        }
        size_t bytes = (size_t)row_bytes(s->dim);
        if (fwrite(slot_row(s, slot), 1, bytes, nf) != bytes) {
            rc = LISA_STORE_EIO;
            break;
        }
        /* Ascending order: the new slot is never held by another row. */
        sqlite3_reset(upd);
        sqlite3_bind_int64(upd, 1, live);
        sqlite3_bind_int64(upd, 2, id);
        if (sqlite3_step(upd) != SQLITE_DONE) rc = LISA_STORE_EIO;
        live++;
    }
    sqlite3_finalize(sel);
    sqlite3_finalize(upd);

    if (nf != NULL) {
        if (rc == LISA_STORE_OK && lisa_file_sync(nf) != LISA_PLAT_OK) rc = LISA_STORE_EIO;
        if (fclose(nf) != 0 && rc == LISA_STORE_OK) rc = LISA_STORE_EIO;
    }
    if (rc == LISA_STORE_OK) rc = meta_set_int(s->db, "slot_count", live);
    if (rc == LISA_STORE_OK) rc = meta_set_int(s->db, "vector_gen", new_gen);
    if (rc == LISA_STORE_OK) rc = meta_set_int(s->db, "change_counter", change + 1);
    if (rc == LISA_STORE_OK) rc = exec(s->db, "COMMIT;");

    if (rc != LISA_STORE_OK) {
        rollback(s->db);
        if (new_path) lisa_remove_file(new_path);
        free(new_path);
        free(old_path);
        return rc;
    }

    /* Committed: switch this handle to the new generation. */
    lisa_unmap(s->map);
    s->map = NULL;
    s->gen = new_gen;
    s->change = change + 1;
    rc = exec(s->db, "BEGIN;");
    if (rc == LISA_STORE_OK) {
        rc = load_slots(s, live);
        exec(s->db, "COMMIT;");
    }
    if (rc == LISA_STORE_OK) rc = open_writer_file(s);
    /* Readers keep their mapping of the old file until they refresh. */
    lisa_remove_file(old_path);
    free(new_path);
    free(old_path);
    return rc;
}

int lisa_store_view(lisa_store_t* s, lisa_store_view_t* out) {
    if (s == NULL || out == NULL) return LISA_STORE_EINVAL;
    int rc = map_current(s);
    if (rc != LISA_STORE_OK) return rc;
    out->vectors = s->slot_count > 0 ? slot_row(s, 0) : NULL;
    out->n_slots = s->slot_count;
    out->dim = s->dim;
    out->live = s->live;
    out->slot_ids = s->slot_ids;
    return LISA_STORE_OK;
}

int lisa_store_migrate_v1(const char* src_dir, const char* dst_dir,
                          const char* embedding_model) {
    if (src_dir == NULL || dst_dir == NULL || embedding_model == NULL) return LISA_STORE_EINVAL;

    int h = storage_open(src_dir);
    if (h <= 0) return h == -2 ? LISA_STORE_ENOTFOUND : LISA_STORE_EFORMAT;
    int64_t n = storage_get_n(h);
    int64_t dim = storage_get_dim(h);
    const float* vectors = storage_get_vectors(h);
    if (n < 0 || dim <= 0 || (n > 0 && vectors == NULL)) {
        storage_close(h);
        return LISA_STORE_EFORMAT;
    }

    int rc = lisa_store_create(dst_dir, embedding_model, dim);
    lisa_store_t* s = NULL;
    if (rc == LISA_STORE_OK) rc = lisa_store_open(dst_dir, LISA_STORE_WRITE, embedding_model, &s);

    lisa_store_chunk_t* meta = NULL;
    if (rc == LISA_STORE_OK && n > 0) {
        int64_t batch = n < MIGRATE_BATCH ? n : MIGRATE_BATCH;
        meta = (lisa_store_chunk_t*)calloc((size_t)batch, sizeof(lisa_store_chunk_t));
        if (meta == NULL) rc = LISA_STORE_ENOMEM;
        for (int64_t i = 0; meta && i < batch; i++) {
            meta[i].doc_id = (char*)"v1";
            meta[i].source_path = (char*)"";
            meta[i].text = (char*)"";
            meta[i].content_hash = (char*)"";
        }
        for (int64_t base = 0; rc == LISA_STORE_OK && base < n; base += batch) {
            int64_t m = (n - base < batch) ? n - base : batch;
            for (int64_t i = 0; i < m; i++) meta[i].chunk_index = base + i;
            rc = lisa_store_insert(s, m, vectors + base * dim, meta, NULL);
        }
    }
    free(meta);
    lisa_store_close(s);
    storage_close(h);
    return rc;
}
