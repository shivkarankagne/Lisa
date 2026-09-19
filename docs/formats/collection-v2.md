# Collection format v2

Storage v2 collection directory, as written by `src/storage/store.c`.
Database format version: **2** (`LISA_STORE_FORMAT_VERSION`); vector file
format version: **1**. Any change to this
document requires a version bump and a migration (plan §7, upgrade rule 3).

## Directory

| File | Purpose |
| :--- | :--- |
| `meta.sqlite` (+ `-wal`, `-shm`) | SQLite database, WAL mode. Source of truth. |
| `vectors.<gen>.lisa` | Vector rows for generation `<gen>`. Only the generation recorded in `meta.vector_gen` is valid; others are leftovers of an interrupted compaction and are removed by the next writer. |
| `write.lock` | Advisory lock (flock) held by the single writer. Contents unused. |

A collection is created in a sibling temporary directory
(`<dir>.creating-<n>`) and renamed into place, so `<dir>` never exists
half-created.

## `meta.sqlite`

```sql
CREATE TABLE meta (
  key   TEXT PRIMARY KEY,
  value                        -- no declared type: stored exactly as written
);

CREATE TABLE chunks (
  id           INTEGER PRIMARY KEY,   -- stable chunk ID (never reused)
  slot         INTEGER NOT NULL UNIQUE, -- row in the vector file
  doc_id       TEXT    NOT NULL,
  chunk_index  INTEGER NOT NULL,
  source_path  TEXT    NOT NULL,
  src_offset   INTEGER NOT NULL,
  src_length   INTEGER NOT NULL,
  text         TEXT    NOT NULL,
  content_hash TEXT    NOT NULL,
  page         INTEGER NOT NULL DEFAULT 0   -- 1-based start page; 0 = unpaged (v2)
);
CREATE INDEX chunks_doc_id ON chunks(doc_id);

-- v2: one row per source document known to the collection
CREATE TABLE documents (
  doc_id       TEXT PRIMARY KEY,   -- equals chunks.doc_id
  source_path  TEXT    NOT NULL,   -- absolute path of the file
  content_hash TEXT    NOT NULL,   -- hash of the file bytes
  size         INTEGER NOT NULL,
  mtime_ns     INTEGER NOT NULL,
  title        TEXT    NOT NULL,
  chunk_count  INTEGER NOT NULL,
  status       TEXT    NOT NULL,   -- "ok" | "no_text" | "error"
  message      TEXT    NOT NULL
);
CREATE INDEX documents_source_path ON documents(source_path);
```

`meta` keys:

| Key | Type | Meaning |
| :--- | :--- | :--- |
| `format_version` | integer | `2` |
| `embedding_model` | text | Model that produced the vectors; fixed at creation |
| `dim` | integer | Vector dimension, 1..65536; fixed at creation |
| `next_id` | integer | Next chunk ID to assign (IDs start at 0) |
| `slot_count` | integer | Number of committed rows in the current vector file |
| `vector_gen` | integer | Current vector file generation |
| `change_counter` | integer | Incremented by every write transaction; lets readers skip no-op refreshes |

Connection settings: `journal_mode = WAL`, `synchronous = FULL`.

## `vectors.<gen>.lisa`

Little-endian throughout.

| Offset | Size | Field |
| :--- | :--- | :--- |
| 0 | 8 | Magic `LISAVECT` |
| 8 | 4 | Vector file format version (u32) = 1 |
| 12 | 4 | Byte-order mark (u32) = `0x01020304`, read little-endian |
| 16 | 8 | `dim` (u64); must equal `meta.dim` |
| 24 | 8 | Generation (u64); must equal `<gen>` in the file name |
| 32 | 32 | Reserved, zero |
| 64 | … | Rows: `float32[dim]` per slot, packed, row-major |

Row `i` starts at byte `64 + i * dim * 4`. Rows at or beyond
`meta.slot_count` are not committed and must be ignored (they may be the
partial output of an interrupted insert). The file must be at least
`64 + slot_count * dim * 4` bytes, or the collection is corrupt.

Rows are packed (no per-row padding): the row base is 64-byte aligned and
rows are 64-byte aligned whenever `dim` is a multiple of 16 (e.g. 384,
768, 1024). NEON loads do not require alignment, and packing keeps every
row reachable by the kernels without a stride parameter.

A slot is **live** if a `chunks` row references it. Slots with no row are
tombstones (deleted chunks) until compaction.

## Write protocol

**Insert** (one transaction, `BEGIN IMMEDIATE`):

1. Read `slot_count`, `next_id`.
2. Write the new rows at slots `slot_count …` in the vector file; flush
   and `fsync` (`F_FULLFSYNC` on macOS).
3. Insert `chunks` rows; update `slot_count`, `next_id`, `change_counter`.
4. `COMMIT`.

A crash before step 4 leaves uncommitted bytes past `slot_count`, which
are ignored and later overwritten.

**Delete**: one transaction deleting `chunks` rows (and, for a whole
document, its `documents` row) and incrementing `change_counter`. The
vector file is not touched.

**Replace a document** (v2): one transaction that deletes the document's
chunks, appends and inserts its new chunks (same protocol as insert), and
writes its `documents` row. A crash leaves the old or the new version.

**Compact** (one transaction):

1. Write `vectors.<gen+1>.lisa` containing the live rows in slot order;
   `fsync`.
2. Renumber `chunks.slot` to `0 … live-1` (ascending, so no conflicts);
   set `slot_count = live`, `vector_gen = gen + 1`; increment
   `change_counter`.
3. `COMMIT`, then remove `vectors.<gen>.lisa`.

Crash before commit: generation `gen` stays current and the new file is a
leftover. Crash after commit: generation `gen + 1` is current and the old
file is a leftover. Leftovers are removed when a writer next opens.

Readers that mapped the old file keep a valid mapping (POSIX keeps an
unlinked, mapped file alive) until they refresh.

## Migration from database format 1

Opening a format-1 collection (any mode) upgrades it in place, holding
the writer lock (taken briefly by read-only opens; `EBUSY` if another
writer holds it):

1. `VACUUM INTO 'meta.v1-backup-<n>.sqlite'` — a consistent copy of the
   old database, never overwritten.
2. One transaction: `ALTER TABLE chunks ADD COLUMN page ... DEFAULT 0`,
   create `documents`, set `format_version = 2`.

A `format_version` newer than this build is rejected (`EFORMAT`).

## Compatibility

- v1 / v1.1 collections (`header.bin` + `vectors.bin`, see
  `src/storage/storage.h`) are not opened by v2; convert them with
  `lisa_store_migrate_v1()`, which gives old vector `i` the ID `i`.
- A `format_version` newer than 2, a bad magic, byte-order mark,
  dimension, or generation in the vector header, or a short vector file,
  is rejected with `LISA_STORE_EFORMAT`.
