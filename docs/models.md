# Default models — intake record

Model weights are licensed separately from code (plan §5). These are the
models LISA 1.0 is built and tested with. They are **not** in the
repository or the binary; users download them (W11 documents how), and
`lisa_model_verify()` checks a file against this table (also compiled into
`src/models/models.c`).

Decision: 2026-09-18 — Qwen family (Apache-2.0) for generation, multilingual
embeddings (plan §10, open question 1).

## Generation: Qwen3-4B (Q4_K_M)

| Field | Value |
| :--- | :--- |
| LISA profile id | `qwen3-4b-q4_k_m` |
| File | `Qwen3-4B-Q4_K_M.gguf` |
| Size | 2,497,280,256 bytes |
| SHA-256 | `7485fe6f11af29433bc51cab58009521f205840f5b4ae3a32fa7f92e8534fdf5` |
| Source | https://huggingface.co/Qwen/Qwen3-4B-GGUF, revision `bc640142c66e1fdd12af0bd68f40445458f3869b` (published by Qwen) |
| License | Apache-2.0 |
| Restrictions on use | None beyond Apache-2.0 (no acceptable-use policy) |

Why: official GGUF from the model's authors (clean provenance for
security review); 4-bit file fits an 8 GB Mac with room for context;
multilingual including Hindi; Apache-2.0 permits commercial and government
use and redistribution.

How LISA uses it: non-thinking mode (the assistant turn starts with an
empty `<think></think>` block, as Qwen's template does for
`enable_thinking=False`). Default sampling per the model card:
temperature 0.7, top-p 0.8, top-k 20, presence penalty 1.5. Temperature 0
gives deterministic (greedy) output.

## Embeddings: e5-small-v2 (Q8_0) — the default

| Field | Value |
| :--- | :--- |
| LISA profile id | `e5-small-v2-q8_0` |
| File | `e5-small-v2-q8_0.gguf` |
| Size | 36,685,088 bytes |
| SHA-256 | `afdfb5c342d2efc2a051c426dd1d00913495d5f2bbceaea100d2f3892aa31cbc` |
| Source | https://huggingface.co/ggml-org/e5-small-v2-Q8_0-GGUF (published by ggml-org, the llama.cpp project) |
| License | MIT |
| Output | 384 dimensions, mean-pooled |

Measured against Qwen3-Embedding-0.6B on the 51-document benchmark corpus
(M2, 16 GB, idle), same questions, same chat model:

| | Qwen3-Embedding-0.6B | e5-small-v2 |
| :--- | :--- | :--- |
| Indexing 51 documents | 3,135 s | **133 s** |
| Index on disk | 67 MB | **38 MB** |
| Model file | 639 MB | **37 MB** |
| Answers containing the fact | 50 / 54 | **51 / 54** |
| Citing the right document | 52 / 54 | 52 / 54 |
| Passage recall@10 | 52 / 54 | **53 / 54** |
| Unanswerable refused | 4 / 4 | 4 / 4 |

Twenty-three times faster to index, a smaller index, and marginally better
scores. Indexing speed is what decides whether someone keeps LISA: a
folder of a few thousand files took a working day with the larger model.

How LISA uses it: mean pooling, with the prefixes the model was trained
with — `query: ` for questions and `passage: ` for documents. Without them
the vectors are noticeably worse.

**What it costs.** e5-small-v2 is trained on English. Documents in other
scripts are still found, because the keyword index (FTS5) is
language-agnostic and the two are fused: the Hindi and Marathi questions
in the eval set are answered correctly with this model. But a question in
another language that needs *semantic* matching rather than shared words
may not be. Anyone who needs that should set the Qwen model below.

The model's position table holds 512 tokens, so LISA caps chunks to fit
it and truncates anything longer for the vector only — the text is stored
whole, found by keyword, and quoted in full in citations.

## Embeddings: Qwen3-Embedding-0.6B (Q8_0) — multilingual alternative

| Field | Value |
| :--- | :--- |
| LISA profile id | `qwen3-embedding-0.6b-q8_0` |
| File | `Qwen3-Embedding-0.6B-Q8_0.gguf` |
| Size | 639,150,592 bytes |
| SHA-256 | `06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439` |
| Source | https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF, revision `370f27d7550e0def9b39c1f16d3fbaa13aa67728` (published by Qwen) |
| License | Apache-2.0 |
| Output | 1024 dimensions, L2-normalised; may be truncated (Matryoshka) |

Set it with `lisa model --set <path>` when semantic search in languages
other than English matters more than indexing speed. A collection records
the model that built it; changing model means indexing the folder again.

This was the default until 2026-09-21. It was chosen over e5 because
e5-small was then only available as GGUF from third-party converters of
unknown provenance. That objection no longer holds for `e5-small-v2`,
which ggml-org — the llama.cpp project itself — publishes. (Two
third-party conversions of *multilingual*-e5-small were tried first and
both were broken: one omitted the token type count, the other crashed on
a vocabulary mismatch.)

How LISA uses it: last-token pooling (from the model metadata). Queries
are prefixed with
`Instruct: Given a question, retrieve passages that answer the question\nQuery:`
as the model card recommends (omitting it costs 1–5% retrieval quality);
documents are embedded as-is.

## Adding or updating a model

1. Download from the authors' official repository at a pinned revision.
2. Record size, SHA-256, licence, and any use restrictions here.
3. Add a profile to `k_profiles` in `src/models/models.c`.
4. Run `test_models` and `bench_models`; record results in a report.
