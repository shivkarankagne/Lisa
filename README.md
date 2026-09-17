# LISA

LISA (Low-level Intelligent Search Accelerator) is a native, local AI
infrastructure runtime.

Current state: the retrieval engine and a minimal persistent storage layer.

---

## What is implemented

| Module | Status |
| :--- | :--- |
| Module 0 — Platform / Build | Single-binary build target (Makefile) |
| Module 4 — Kernels | ARM64 NEON L2 distance kernel |
| Module 5 — Retrieval Engine | Complete |
| Module 6 — Storage Engine | create / open / get / insert / delete / close |
| Module 13 — CLI | Search by index file or by storage collection |
| Module 13 — HTTP API | Read-only, localhost only, no TLS/auth |

Other modules (Runtime, Memory, Tensor, Documents, Embeddings, Model,
Inference, Context, RAG/Agents, Security) are not started.

---

## Build

    make

Produces one native executable:

    build/lisa

Clean:

    make clean

Run the test suite:

    make test

---

## Usage

Search a binary index file:

    lisa --index vectors.bin --dim 768 --topk 5 --query query.txt

Search a storage collection:

    lisa --collection /path/to/collection --topk 5 --query query.txt

Both produce the same output format:

    <index> <distance>

See `src/cli/README.md` for the full CLI contract, including exit codes.

---

## Layout

    src/
      cli/              CLI entry point
      retrieval/        scalar reference + public API
      storage/          persistent local collection
      kernels/arm64/    NEON assembly kernel + wrapper
    tests/              test suite
    benchmark/          benchmark harness
    build/              build output (not committed)

---

## Baseline

Measured on Apple M2, 10,000 vectors, dim 768, k=5, 100 queries, gcc -O2,
single-threaded, warm cache:

| Implementation | mean | min | max |
| :--- | :--- | :--- | :--- |
| Scalar reference | 5.644 ms | 5.575 ms | 6.204 ms |
| ARM64 NEON kernel | 1.644 ms | 1.620 ms | 1.650 ms |

3.43x speedup, assembly over this project's own scalar reference.

This is not a comparison against any other vector search engine. No such
comparison has been run for this codebase. Do not cite one until it has.

Full conditions, methodology, and known limitations are recorded in
`LISA_REPORT_AND_UPDATE_001.md`. Do not change benchmark conditions or
claim new numbers without a separate measured package.

---

## License

MIT. See [LICENSE](LICENSE).

---

*Built by LISA.*

---

## HTTP API

Start the read-only API:

    lisa --serve --port 8080

Endpoints:

    GET  /health
    POST /search?collection=<dir>&topk=<n>

See `src/cli/README.md` for the full contract. The server binds to
127.0.0.1 only. No TLS, no authentication.
