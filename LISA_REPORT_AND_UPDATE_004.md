# LISA — Report and Update 004

**Date:** 2026-09-18
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W3 — Public API
**Commits:** `57a49ef`, `8d13840`

---

## 1. Status

W3 is complete, with one criterion that is met when W9 lands.

| Acceptance criterion (plan §6 W3) | Status |
| :--- | :--- |
| `include/lisa.h` only public header; opaque handles; ownership per function; error codes enumerated | Done |
| All sizes, counts, IDs are `int64_t` / `uint64_t` | Done |
| API version macro; semantic versioning | Done — plus a configure-time check that header and project versions match |
| Extension points defined, documented, tested with no-op/test implementations | Done — all five |
| CLI, HTTP, GUI use only this API | **W9** — the current CLI and HTTP server are replaced there; the GUI (W10) uses the W9 HTTP API |

## 2. What the API provides

- **Context** (`lisa_context_t`): injected allocator + extension points.
  Shareable between threads; every other handle is one-thread-at-a-time.
- **Collections**: create, open (read/write, expected-model check), info,
  refresh, add, remove, remove_document, get_chunk / lisa_chunk_free,
  compact, search_vector (hits written to a caller-owned array).
- **Status codes**: `LISA_OK` and 12 stable negative codes, each with a
  message from `lisa_status_string()`.
- **Upgrade safety**: every struct passed in starts with `struct_size`;
  LISA reads only the fields the caller's version has. Tested by passing
  deliberately shortened structs.

## 3. Extension points

| Extension | Wired now? | Behaviour with none installed |
| :--- | :--- | :--- |
| Retrieval filter | Yes — every `search_vector` | All live chunks are candidates |
| Audit sink | Yes — create, open, add, remove, read, search, compact | No events |
| Auth provider | Stored; HTTP server calls it from W9 | Local, unauthenticated |
| HTTP routes | Stored; HTTP server offers unhandled requests from W9 | None |
| Storage crypto | **Reserved.** If installed, create/open return `LISA_E_UNSUPPORTED` | Unencrypted (as today) |

The storage-crypto behaviour is deliberate: an enterprise build that
installs encryption must never silently get unencrypted collections.

Safety properties tested: a retrieval filter can only hide chunks (an
attempt to re-enable deleted chunks is ignored); a filter error fails the
search; audit events carry the principal, status, count, and chunk IDs.

## 4. Allocator scope (stated, not hidden)

Everything the API layer allocates goes through the injected allocator
(tested: every allocation through a counting allocator is freed).
Internal modules and vendored libraries (SQLite, llama.cpp) still use the
system allocator. Routing those through the context is not needed for
1.0 and is recorded here rather than implied.

## 5. Other changes

- Internal `lisa_chunk_t` / `lisa_chunk_free` in `store.h` renamed to
  `lisa_store_chunk_t` / `lisa_store_chunk_free` to free the names for the
  public API. Internal only.

## 6. Tests

20 tests, all passing in Release and ASan/UBSan builds. New:

- `test_public_api` (Unity, 9 tests, includes only `lisa.h`): version,
  status strings, config validation and old-struct compatibility,
  collection lifecycle and error mapping with a counting allocator,
  search options (defaults, capacity, bounds, old struct), retrieval
  filter, audit sink, storage-crypto refusal, auth and route installation.
- `test_header_cxx`: `lisa.h` compiles as C++ with `-Wall -Wextra
  -pedantic` and links.

## 7. Open items

- Write operations (`add`, `remove`, ...) do not take a principal; audit
  events for them have `principal = NULL`. When multi-user writes arrive
  (E1), additive `..._ex` variants with an options struct will carry it.
- Text search, ingest, and ask are added to `lisa.h` in W4–W8, additively.

## 8. Next

W4 — models: llama.cpp behind `lisa_model` / `lisa_generate` /
`lisa_embed`; default generation and embedding models (licence-checked);
hardware floor measured.
