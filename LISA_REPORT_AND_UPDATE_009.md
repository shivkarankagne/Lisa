# LISA — Report and Update 009

**Date:** 2026-09-19
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W8 — Context + answer
**Commits:** see `git log` (models: token counting; context engine; ask API)

---

## 1. Status

W8 is complete for the API. CLI, HTTP, and GUI streaming arrive with
W9/W10, on top of the streaming callback built here.

| Acceptance criterion (plan §6 W8) | Status |
| :--- | :--- |
| Pipeline of named stages: retrieve → filter → rank → dedupe → budget → generate; stages can be added without changing callers | Done — `src/context/`; a stage is one `{name, fn}` entry |
| Every answer lists citations: document, page/offset, quoted passage | Done — `lisa_citation_t`: path, title, page, offset, length, passage text, document hash, similarity |
| No relevant chunk → says so instead of guessing | Done — two layers (below) |
| Ask API takes a list of messages (one in 1.0) | Done — more than one returns `LISA_E_UNSUPPORTED` until follow-up chat (L2) |
| Tokens stream to CLI, HTTP, GUI | API streams (`on_token`, UTF-8-safe pieces, stoppable). CLI/HTTP in W9, GUI in W10 |
| Default prompt budget keeps first token ≤ 5 s on the reference machine; configurable | Done — 1,100 tokens by default; measured 3.9–4.7 s (below) |

## 2. What was built

- **Models:** `lm_count_tokens` (additive to `models.h`), so the budget
  counts the exact prompt the model will see.
- **Context engine (`src/context/`):** filter (similarity floor), rank
  (stable, by fused score), dedupe (same text, or more than half
  overlap within a document), budget (keeps the best passages that fit,
  skipping any that would overflow, and builds the prompt), citation
  parsing (`[n]`, `[n, m]`, `[n][m]`), not-found detection. No model or
  storage calls: token counting is a callback, so all of it is unit
  tested without a model.
- **Prompt safety:** document text and the question can never become
  chat-template markup (`<|` and `|>` are broken up), so a document
  cannot inject a system or assistant turn.
- **Public API 0.4.0 (additive):** `lisa_ask`, `lisa_answer_free`,
  `lisa_ask_options_t`, `lisa_citation_t`, `lisa_answer_t`,
  `LISA_NOT_FOUND_TEXT`. The answer reports passages retrieved/used,
  prompt and answer tokens, time to first token, and total time.

### "Not found": two layers

Calibrated on Qwen3-Embedding-0.6B with 12 answerable and 12
unanswerable questions over 8 passages (English, Hindi, Hinglish):

- Off-topic questions reach cosine ≤ 0.46 with any passage; answerable
  ones score ≥ 0.47 with their passage. **Layer 1:** passages below
  `min_similarity` (default 0.40) are dropped; if none remain, LISA
  answers `LISA_NOT_FOUND_TEXT` **without running the model** (0.04 s).
- Same-topic questions whose fact is missing ("parental leave policy"
  against an annual-leave passage) score 0.59–0.64, so no threshold can
  catch them. **Layer 2:** the prompt tells the model to reply exactly
  `LISA_NOT_FOUND_TEXT`; Qwen3-4B did so for 6 of 6 such questions and
  answered 5 of 5 answerable ones with correct `[n]` citations.

## 3. Measurements

M2 MacBook, Qwen3-4B Q4_K_M + Qwen3-Embedding-0.6B Q8_0, Metal, Release
build. Corpus: this repository's 13 Markdown documents (143 chunks,
ingested in 24.1 s). Six questions, default options, answer ≤ 160
tokens:

| Question | Prompt tokens | Passages used | First token | Total | Result |
| :--- | ---: | ---: | ---: | ---: | :--- |
| Which HTTP library does LISA use? | 1,069 | 4 | 4.74 s | 5.06 s | Not found (retrieval miss, see §5) |
| Time to first token measured on the M2? | 1,087 | 4 | 4.47 s | 5.51 s | Correct, cited report 005 |
| Why did the assembly kernel crash at -O2? | 961 | 3 | 4.05 s | 7.49 s | Correct, cited report 002 |
| What does the retrieval filter extension do? | 1,061 | 5 | 4.42 s | 5.77 s | Correct, cited the plan |
| How is a format-1 collection migrated? | 936 | 4 | 3.86 s | 5.99 s | Mostly correct, cited the format spec |
| What is the licence for organisations? | 1,004 | 5 | 4.10 s | 5.20 s | Correct, cited the plan |

First token 3.9–4.7 s at the full budget: the ≤ 5 s criterion holds,
with little headroom. Small prompts (150–190 tokens, `test_ask`): first
token 0.7–0.9 s.

## 4. Tests

27 tests, all passing in Release and ASan/UBSan builds. New:

- `test_context` (9, no model): filter, stable rank, dedupe, budget
  (fit, skip oversize, nothing fits, question too long, counter
  failure), markup injection, pipeline order and failure reporting,
  citation parsing edge cases, not-found detection.
- `test_ask` (5, both models, public API only): cited answer with the
  stream equal to the text; PDF citation with page 1 and document
  title; Hindi question answered from a Hindi document; off-topic
  (model not run) and near-miss (model declines) not-found; budget too
  small (`LISA_E_TOO_LONG`); stopping the stream; invalid arguments and
  model mismatch.

Sanitizer note: under ASan, `test_ask` hit a "container-overflow" report
inside llama.cpp (`llama_batch_allocr::init`, `vector<int>::resize(1)`
after `clear()`, byte 0 of the vector's own buffer). The write is to the
element being created, so this is a toolchain false positive (Apple clang
21 libc++ annotation order vs. the `bzero` interceptor), not a LISA bug.
Only that check is disabled, only for `test_ask` (CMake, with the
reason); every other ASan/UBSan check still runs there.

## 5. Open items

- **Retrieval on homogeneous corpora.** On the repo corpus, "Which HTTP
  library does LISA use?" misses: every chunk mentions LISA, so the
  question's common words dominate the keyword list, and the CivetWeb
  chunks land at fused ranks 15–20 (vector rank 7). This is what the
  W12 eval set measures and what plan §8 L2 (reranker, query rewriting)
  addresses; not tuned to one question here.
- A not-found reply in another language (e.g. Hindi) is not recognised
  as not-found; the answer is then marked found and cites all passages.
- Models are not thread-safe: two `lisa_ask` calls on one model at the
  same time are undefined. The W9 server must serialise them.
- `test_store_crash` still does not cover `replace_doc` (from 007).

## 6. Next

W9 — interfaces: CLI (`ingest`, `search`, `ask`, `serve`, `gui`,
`model`, `--version`), JSON HTTP API under `/v1` on CivetWeb + yyjson,
local security, removal of the old `http.c` (D5–D7).
