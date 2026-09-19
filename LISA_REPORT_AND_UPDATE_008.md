# LISA — Report and Update 008

**Date:** 2026-09-19
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W7 — Retrieval
**Commits:** `bf99a87` (storage v3), `f45b29b` (hybrid search API)

---

## 1. Status

W7 is complete.

| Acceptance criterion (plan §6 W7) | Status |
| :--- | :--- |
| Exact vector search via the kernel registry | Done — `lisa_index_exact()` wraps `lisa_search_masked` |
| FTS5 index in the collection's SQLite file | Done — database format 3, `chunks_fts` kept in step by triggers |
| Reciprocal rank fusion, weights configurable | Done — k = 60; `vector_weight`, `keyword_weight` per query |
| Metadata filter hook (enterprise `lisa_retrieval_filter`) | Done — one mask (live, path prefix, filter) restricts both lists |
| Behind `lisa_index`; exact search is the first implementation | Done — `src/retrieval/index.h` |

## 2. What was built

- **Storage format 3:** `chunks_fts` (FTS5, external content, `unicode61
  remove_diacritics 2`), insert/delete/update triggers. Format 1 and 2
  databases are backed up (`VACUUM INTO`) and migrated in one
  transaction that rebuilds the index. `lisa_store_keyword_search`
  (BM25, optional path prefix) and `lisa_store_prefix_mask`. User text
  is quoted word by word and ORed, so it can never inject FTS5 syntax.
- **Retrieval:** `lisa_rrf_fuse` (`src/retrieval/fusion.h`) and the
  `lisa_index_ops_t` seam (`src/retrieval/index.h`) — an approximate
  index (plan §7) plugs in here without API changes.
- **Public API 0.3.0 (additive):** `lisa_query_t`, `lisa_scored_hit_t`,
  `lisa_collection_query` (hybrid, vector-only, or keyword-only) and
  `lisa_collection_query_text` (embeds the question with the
  collection's model, then hybrid). Each hit reports fused score, vector
  distance and rank, BM25 score and rank. Candidate pool per list:
  4 × top_k, min 50, max 2,000. Every query is audited
  (`LISA_AUDIT_SEARCH`).
- **Docs:** `docs/formats/collection-v2.md` describes format 3 and the
  migration chain.

## 3. Interface and format changes

- `include/lisa.h` 0.2.0 → 0.3.0: new types and two functions; nothing
  existing changed.
- Database format 2 → 3 (automatic migration with backup). Vector file
  format unchanged (1). Older LISA builds reject a format-3 collection
  (`EFORMAT`), as designed.

## 4. Tests

25 tests, all passing in Release and ASan/UBSan builds. New or extended:

- `test_store`: keyword search (ranking, prefix, Hindi, deleted chunks,
  hostile query text), format v2 → v3 and v1 → v3 migration.
- `test_query` (11, new): RRF ordering, ties, weights, capacity,
  duplicates; keyword-only, Hindi keyword, vector-only, hybrid fusion,
  zero keyword weight, path prefix on both lists, retrieval filter on
  both lists, removed chunks and stale readers, invalid arguments, old
  `struct_size`, audit events, model mismatch; with the real embedding
  model, a question sharing no words with its answer ("Which machine
  broke down?") finds the pump report, and a keyword question ranks
  first in both lists.

No performance numbers are claimed for W7; query latency is measured
in W8 together with the answer path.

## 5. Open items

- `path_prefix` is a plain string prefix: `/docs` also matches
  `/docs-old/`. Callers pass a trailing `/` for a folder (the tests do);
  the W9 CLI/HTTP layer must add it.
- Keyword queries OR all words; stop words (English "the", Hindi
  particles) are matched too. BM25 weights them down; a stop-word list
  is a perfecting item if W12 validation shows a need.
- Vector search is exact (linear). Fine for 1.0 targets; the ANN index
  is plan §8.
- `test_store_crash` still does not cover `replace_doc` (from 007).

## 6. Next

W8 — context and answer: retrieve → filter → rank → dedupe → budget
(≈1,100 prompt tokens on the M2) → generate, with citations, "not found
in your documents", and streamed tokens.
