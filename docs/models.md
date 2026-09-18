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

## Embeddings: Qwen3-Embedding-0.6B (Q8_0)

| Field | Value |
| :--- | :--- |
| LISA profile id | `qwen3-embedding-0.6b-q8_0` |
| File | `Qwen3-Embedding-0.6B-Q8_0.gguf` |
| Size | 639,150,592 bytes |
| SHA-256 | `06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439` |
| Source | https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF, revision `370f27d7550e0def9b39c1f16d3fbaa13aa67728` (published by Qwen) |
| License | Apache-2.0 |
| Output | 1024 dimensions, L2-normalised; may be truncated (Matryoshka) |

Why this instead of multilingual-e5-small (the size originally preferred):
e5-small is only available as GGUF from third-party converters of unknown
provenance, which a security reviewer cannot accept. Qwen3-Embedding-0.6B
is published as GGUF by its authors, under Apache-2.0, is multilingual
(including Indian languages), and supports truncating vectors (e.g. to 512
or 256 dimensions) to keep indexes small. If a smaller model is needed
later, e5-small can be converted in-house from the official weights with a
pinned converter and recorded here.

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
