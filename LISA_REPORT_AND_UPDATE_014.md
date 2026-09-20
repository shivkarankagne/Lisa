# LISA — Report and Update 014

**Date:** 2026-09-20
**Type:** Tuning report (answer quality on real documents)
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** none — quality work between W11 and W12

---

## 1. Why

Testing 1.0 on the user's own files (a rent agreement, a court petition,
a company filing, a 200-page book) produced "I could not find this in
your documents" for questions the documents plainly answered. Every case
traced to ingestion or extraction, not to ranking. This report covers
the fixes.

## 2. Defects found and fixed

| # | Symptom | Cause | Fix |
| :--- | :--- | :--- | :--- |
| 1 | Scanned PDFs indexed as empty documents | No OCR | `src/documents/pdf.c`: pages with under 16 characters, or over 2% unmapped glyphs, are rendered with PDFium and read with Apple Vision (`lisa_ocr_image`) |
| 2 | Word files not indexed | No `.docx` extractor | `src/documents/docx.c` over miniz (W5b) |
| 3 | Words lost from PDF text (`ﬁ` → nothing) | Ligatures and unmapped glyphs dropped by the normaliser | `src/documents/documents.c`: U+FB00–U+FB04 expand to `ff`/`fi`/`fl`/`ffi`/`ffl`; soft hyphen dropped; U+FFFE/U+FFFF read as `fi` |
| 4 | Whole files silently missing after an ingest | The embedding model failed once (GPU busy) and the file was recorded as done | `src/ingest/ingest.c`: 3 attempts, 400 ms apart; a failed file is recorded with `mtime_ns = 0` so the next run always retries; the message says "will try again" |
| 5 | Front matter (title pages, copyright) ranked above the answer | Each passage was embedded with its title prefixed, so the title dominated the vector | Embed the passage text alone; `EMBED_RECIPE 2` in the new `user_version` meta key forces one rebuild of existing collections |
| 6 | Indexing a Downloads folder took hours and heated the machine | Build and dependency directories were walked | `k_skip_dirs`: `node_modules`, `site-packages`, `dist-packages`, `__pycache__`, `.venv`, `Pods`, `.gradle`, `.cargo`, `.git`, `.svn` |
| 7 | An answer cited page 1 for text on page 2 | A chunk that crossed a page boundary was cited by the page it started on | `src/documents/chunk.c`: prefer to end a chunk at a page boundary, once it is at least a third of the target size |
| 8 | Metal out of memory reported as a successful empty answer | `produced == 0` was `LM_OK` | `src/models/models.c` returns `LM_ERUNTIME`; the CLI adds "another AI app may be holding the GPU" |
| 9 | Citations pointed at nothing | Documents number their own clauses ("15. That both parties…") and the model echoed `[15]` | Passages are labelled `[S1]`, `[S2]`… in the prompt; `ctx_parse_citations` accepts `[S1]` and a bare `[1]`; the CLI and the GUI render `[S<n>]` |
| 10 | A second `lisa` window talked to the first one's data | No record of a running instance | `<data>/server.json` with a pid check (`app_server_announce/running/forget`) |
| 11 | "What is the capital of France?" answered "Paris", citing an unrelated book page | The passage scored 0.39, above the 0.25 floor, and the model answered from its own knowledge | `src/context/context.c`: the refusal rule now applies to facts the model is sure of, and it is asked to check its answer's words appear in a passage |

## 3. Interface changes

- **Answer text**: citation markers are now `[S<number>]`. The JSON
  shape is unchanged — `citations[].number` still carries the number.
  Documented in `docs/http-api.md`.
- **Collection metadata**: new `user_version` key (`src/storage/store.h`:
  `lisa_store_get_user_version` / `lisa_store_set_user_version`),
  additive; an older LISA ignores it.
- **New**: `GET /v1/jobs`, `GET /v1/collections/{name}/documents`,
  `GET`/`POST /v1/settings` (models and watched folders). Additive
  within `/v1`; API version 0.6.0.

## 4. Measured on the user's documents

| Question | Before | After |
| :--- | :--- | :--- |
| Monthly rent | not found | "Rs. 2,000/- (Rupees Two Thousand only)", cited page 2 |
| Company CIN | not found | correct |
| Writ petition subject | not found | correct |
| Question about a scanned book | not found | answered with a page citation |
| Who wrote a book in the collection | not found | "Charles Saatchi", cited |
| "What is the capital of France?" | "Paris.", citing an unrelated page | refused |
| "What is the boiling point of water?" | — | refused |

## 5. Not done

- Indexing speed: embedding is compute-bound on the GPU. Batching was
  tried and reverted — it capped sequences at 512 tokens, so long
  passages were dropped, and the measured gain was only 1.2×. The real
  levers are a smaller embedding model or keyword-first indexing; both
  are deferred.
- Near-duplicate copies of the same file still crowd results when the
  text differs slightly; exact duplicates are already removed.
- No cancel button for a running ingest.
- OCR is English only (Apple Vision); Hindi and Telugu scans need
  Tesseract.
- `.pptx`, `.xlsx` and photos (`.jpg`, `.png`) are not read.

## 6. Next

W0 (competitor baseline, re-run on an idle machine) and W12 (validation:
eval set, clean machine, offline, crash, 8-hour soak), then signing once
the Apple Developer account is bought.
