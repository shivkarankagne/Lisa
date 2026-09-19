# LISA — Report and Update 007

**Date:** 2026-09-19
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W6 — Ingest
**Commits:** `9ba084f` (platform), `b6012c9` (storage v2), `651bcae`
(embedding context), `023c233` (ingest engine), `9d24fa1` (public API)

---

## 1. Status

W6 is complete.

| Acceptance criterion (plan §6 W6) | Status |
| :--- | :--- |
| Ingest run twice on an unchanged folder does nothing | Done — zero embeddings, zero writes (tested by counting embed calls) |
| Edited file → old chunks removed, new chunks added | Done — one atomic transaction per document |
| Deleted file → its chunks removed | Done |
| Background job with an ID and progress | Done — job handle with live status; HTTP job IDs arrive with W9 |
| Ask works during ingest | Done — search from a separate read handle during a running job (tested) |

## 2. What was built

- **Platform:** file info (size, mtime), sorted directory walk (hidden
  skipped, symlinked directories not followed), absolute paths, threads,
  mutexes.
- **Storage format v2:** `page` on chunks; `documents` table (path, hash,
  size, mtime, title, chunk count, status, message);
  `lisa_store_replace_doc` swaps a document atomically. Format-1
  collections are backed up (`VACUUM INTO`) and migrated on open.
- **Ingest engine (`src/ingest/`):** size+mtime fast path; content-hash
  check; extract → chunk → embed (title or file name prepended) →
  atomic replace; `no_text` and `error` records so re-runs skip them;
  removal of deleted files; a missing path aborts before any change; a
  sibling folder sharing a name prefix is never touched.
- **Public API (0.2.0):** `lisa_ingest_start/_status/_cancel/_wait/_free`,
  blocking `lisa_ingest`, `lisa_collection_create_for_model`,
  `lisa_collection_documents`, `page` in `lisa_chunk_t`,
  `LISA_AUDIT_INGEST`. While a job runs, its collection and model
  return `LISA_E_BUSY`.
- **Models:** embedding context raised to 8,192 tokens so dense scripts
  cannot overflow.

## 3. Also in this period

- **CI:** first full run on GitHub: release job 22/22. The sanitizer job
  timed out only in `test_models` (4B inference under ASan on CI's
  virtual Macs); it now skips model inference, which the release job and
  local sanitizer runs still cover.
- **PDFium:** a bootstrap bug in `build_pdfium.sh` (relative path) found
  by the first CI run and fixed; CI and developers now fetch a prebuilt,
  SHA-256-verified PDFium from a GitHub Release (`fetch_pdfium.sh`).
- **Licence:** draft BUSL-1.1 (free for individuals, paid for
  organisations); parked pending legal review.

## 4. Tests

24 tests, all passing in Release and ASan/UBSan builds. New or extended:

- `test_platform`: file info, absolute paths, directory walk (order,
  hidden, symlink loop, early stop), 4 threads × 10,000 increments.
- `test_store`: replace_doc, document records, prefix listing, reader
  refresh, format v1 → v2 migration with backup.
- `test_ingest` (8): first run over mixed files, unchanged re-run, touch,
  edit and delete, cancel, missing path, embedding failure, prefix
  safety, search round-trip, and a real-model run where English and
  Hindi questions find the right documents.
- `test_ingest_api` (6, only `lisa.h`): page round-trip,
  create_for_model, blocking ingest + search, background job with busy
  handles and concurrent search, cancel and resume, start errors.

## 5. Open items

- `test_store_crash` covers insert, delete, and compaction; replace_doc
  uses the same transaction helpers but is not yet in the kill test.
- Embedding runs one text per decode (batching is L1). Informally, 40
  short notes ingest in a few seconds on the M2.
- One ingest job per process at a time (PDFium is not thread-safe); the
  W9 server must queue jobs.

## 6. Next

W7 — retrieval: FTS5 keyword search + vector search fused with
reciprocal rank fusion, metadata filter hook, behind `lisa_index`.
