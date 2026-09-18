# LISA — Report and Update 005

**Date:** 2026-09-18
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W4 — Models
**Commit:** `1538833`

---

## 1. Status

W4 is complete. One performance target is **not met** at the context size
it was stated for; see §4.

| Acceptance criterion (plan §6 W4) | Status |
| :--- | :--- |
| Default generation + embedding models chosen; licences permit commercial redistribution; intake recorded | Done — `docs/models.md` |
| Output matches upstream llama.cpp (fixed seed, greedy) | Done — identical to an independent decode loop written directly against llama.cpp |
| Metal enabled through ggml; CPU path works | Done — both tested |
| Other ggml backends disabled but not removed | Done |
| Model file verified (size + hash) | Done — `lisa_model_verify`; the `lisa model` CLI command arrives in W9 |
| Hardware floor measured | Measured on a 16 GB M2 (§4); 8 GB not tested on real hardware |
| First token ≤ 5 s on 8 GB | **Not met for a ~2,000-token prompt (8.4 s)**; met for ≤ ~1,100 tokens |

## 2. Models

| Role | Model | File | Licence | Provenance |
| :--- | :--- | :--- | :--- | :--- |
| Generation | Qwen3-4B, Q4_K_M | 2.50 GB | Apache-2.0 | Official Qwen GGUF, pinned revision + SHA-256 |
| Embeddings | Qwen3-Embedding-0.6B, Q8_0 | 0.64 GB | Apache-2.0 | Official Qwen GGUF, pinned revision + SHA-256 |

Deviation from the preferred "multilingual small" embedding model
(e5-small, ~120 MB): it exists as GGUF only from third-party converters,
which fails security review. Qwen3-Embedding is official, multilingual,
and supports truncated vectors (e.g. 256/512 dims) to keep indexes small.

## 3. What was built

- Public API (additive): model load/free/info, progress + cancel,
  `lisa_chat`, `lisa_generate` (streaming, early stop, greedy or seeded
  sampling), `lisa_embed` (query/document, normalised, truncatable),
  known-model table, `lisa_model_verify`, `lisa_free`; status codes
  `CANCELLED`, `TOO_LONG`, `WRONG_MODEL_KIND`.
- `src/models/`: the only code that touches llama.cpp. A profile table
  holds per-model behaviour: Qwen3 non-thinking mode (empty think block)
  and the model card's sampling settings; Qwen3-Embedding's query
  instruction, EOS, and last-token pooling.
- CI downloads the models (pinned, hash-verified, cached).
- `bench_models` for the measurements below.

## 4. Measurements

Apple M2, 16 GB, macOS 26.6.2, Release build. Greedy decoding. Warm runs
(Metal shaders already compiled — first launch adds ~17.6 s, see
report 002).

| Measure | Metal (GPU) | CPU only |
| :--- | :--- | :--- |
| Load generation model | 0.45 s | 2.73 s |
| Load embedding model | 0.18 s | 0.81 s |
| Prompt processing | ~235–240 tokens/s | ~70 tokens/s |
| Time to first token, 2,001-token prompt | **8.6 s** | 28.7 s |
| Generation | 25.2 tokens/s | 13.8 tokens/s |
| Embedding, ~650-token chunks | 2.2 chunks/s (~1,450 tokens/s) | 0.8 chunks/s |
| Peak resident memory, both models loaded | 4.2 GB | 6.2 GB |

Prompt-processing speed was checked directly against llama.cpp with
batch sizes 512/1024/2048 and flash attention on/off: 213–239 tokens/s
in every configuration. It is the hardware limit for this model on a base
M2 GPU, not a LISA setting.

**Consequence for W8 (context engine):** time to first token is roughly
`prompt_tokens / 240` seconds on this machine. To answer within 5 s, the
assembled prompt must stay near **1,100 tokens** (e.g. 4–5 chunks of
~200 tokens plus instructions and question). The context budget must be
configurable, and W12 must re-measure on the slowest supported machine.

## 5. Findings

1. **UBSan in ggml.** `ggml_graph_nbytes` advances a NULL pointer to
   compute a layout (never dereferenced). Handled with a one-function
   entry in `third_party/sanitizer-ignorelist.txt`; vendored code
   unchanged. No other sanitizer findings in llama.cpp or in LISA.
2. **Exit without freeing a model aborts** (ggml Metal assert in a static
   destructor). LISA's tests always free models; the W9 CLI and server
   must free models on every exit path, including signals.
3. **`.gitignore` pattern `models/` also matched `src/models/`.** Anchored
   to `/models/` before commit.

## 6. Tests

21 tests, all passing in Release and ASan/UBSan builds. `test_models`
(8 tests): known-model table, load errors, SHA-256 (FIPS vector),
verification of both downloaded files, cancelled load, generation
(deterministic chat containing "Paris", no think block, greedy output
identical to direct llama.cpp, streaming reassembly, early stop, seeded
sampling reproducible, wrong-kind errors), CPU-only path and context
overflow, embeddings (normalised; English and Hindi queries retrieve the
right documents; deterministic; query/document forms differ; truncated
vectors still rank correctly).

## 7. Open items

- Embeddings process one text per decode; batching several texts per
  decode is an L1 optimisation.
- The 8 GB hardware floor is inferred from 4.2 GB peak memory, not
  tested on an 8 GB machine; W12 includes that test.
- Qwen3-4B has a 32,768-token training context; LISA uses 4,096 by
  default.

## 8. Next

W5 — documents: txt, md (md4c), pdf (PDFium, static linking verified
first); extractor registry; normalised text with offsets; chunker.
