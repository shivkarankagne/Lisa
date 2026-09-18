# LISA — Report and Update 003

**Date:** 2026-09-18
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W2 — Storage v2
**Commits:** `86add49`, `0d67001`, `f1de8e0`

---

## 1. Status

W2 is complete.

| Acceptance criterion (plan §6 W2) | Status |
| :--- | :--- |
| `vectors.<gen>.lisa` (mmap, LE, versioned header) + `meta.sqlite` (WAL) | Done — spec in `docs/formats/collection-v2.md` |
| Stable `uint64` IDs; delete never renumbers; tombstones + compaction | Done |
| Per-chunk metadata: doc_id, chunk_id, source_path, offset, length, text, content_hash | Done |
| Model ID, dim, format version recorded; different model → clear error | Done (`LISA_STORE_EMODEL`) |
| Crash at any point → last committed state (kill-during-write test) | Done — 40 random SIGKILL trials |
| One writer, many readers; second writer fails cleanly | Done (`LISA_STORE_EBUSY`) |
| v1 / v1.1 migrate with one command | Library call done; `lisa migrate` CLI command in W9 |

## 2. Design

- **SQLite is the source of truth** for which chunks exist, their IDs,
  metadata, and slots. The vector file is an append-only array of rows.
- **Insert:** rows are written past the committed end and fsynced, then
  one transaction inserts metadata and advances `slot_count`. A crash
  before commit leaves ignored bytes.
- **Compaction:** writes `vectors.<gen+1>.lisa`, then one transaction
  renumbers slots and switches the generation. Either generation is
  consistent after a crash; leftovers are removed by the next writer.
- **Readers** map the vector file and see new commits after
  `lisa_store_refresh()` (cheap no-op when `change_counter` is
  unchanged). A reader whose file was compacted away reloads and retries.

## 3. Deviations from the plan text (plan updated)

- File name `vectors.<gen>.lisa` instead of `vectors.lisa`: the
  generation makes compaction an atomic switch.
- Rows are packed rather than individually padded to 64 bytes: NEON loads
  need no alignment, and padding would add a stride parameter to every
  kernel. Rows are 64-byte aligned whenever `dim % 16 == 0`.
- Migration is a library call now; the CLI command arrives with the other
  commands in W9.

## 4. Bug found during W2

`lisa_store_get_vector` on a reader opened before a compaction returned
`ENOTFOUND` for a chunk that still existed: it looked up the slot in the
database, which had already been renumbered. It now answers from the
handle's own view (fast path via the database index, fallback scan).
Covered by `test_compaction_keeps_ids_and_vectors`.

## 5. API changes

All additive.

- `src/storage/store.h` (new): storage v2 API.
- `retrieval.h`: `lisa_search_masked()` (search skipping dead slots).
- `platform.h`: `lisa_file_seek()` (64-bit positioning).
- `lisa_core` now links the vendored SQLite.

The v1 storage module (`storage.h`) is unchanged and still used by the
CLI and HTTP server until W9, and as the migration source.

## 6. Tests

18 tests, all passing in Release and ASan/UBSan builds. New:

- `test_store` (Unity, 11 tests): create/open errors, model mismatch,
  single writer, read-only enforcement, insert/get, stable IDs,
  all-or-nothing insert and delete, delete_doc, persistence, reader
  refresh, search via view skipping deleted chunks, compaction (IDs,
  vectors, file generations, reader across compaction), corrupt header,
  truncated vector file, v1 migration (IDs equal old indices; identical
  search results).
- `test_store_crash`: 40 trials killing the writer with SIGKILL at a
  random time during a fixed sequence of inserts, deletes, and
  compactions. After each kill the collection reopens as writer, equals
  the simulated state after the last acknowledged operation (or the next
  one, when the kill landed between commit and acknowledgement — seen in
  trial 26), every vector and metadata record is intact, and writes and
  compaction still work.
- `test_search`: masked search vs reference on compacted data.

## 7. Measured (informal)

Insert with `F_FULLFSYNC` on Apple M2 internal SSD: about 3 ms per
committed batch (37 chunks, dim 8). Durability costs one full sync per
batch, so ingest (W6) should insert in batches, not per chunk.

## 8. Open items

- `lisa_store_refresh` reloads the whole slot table when anything
  changed (O(n)). Fine at 1.0 targets; incremental refresh is an L6 item
  if profiling shows it matters.
- Readers keep old mappings valid after compaction because POSIX keeps
  unlinked mapped files alive. Windows (L8) will need a different
  removal strategy.
- `synchronous = FULL` without `fullfsync`: survives process crashes (as
  tested); on power loss SQLite may lose the most recent commits but
  stays consistent.

## 9. Next

W3 — public API `include/lisa.h`: opaque handles, `int64_t` sizes, error
codes, ownership rules, semantic version, and the enterprise extension
points as no-ops.
