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
| Module 6 — Storage Engine | create / open / get / close. No insert or delete yet. |
| Module 13 — CLI | Search by index file or by storage collection |

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

The retrieval engine has a measured baseline. Conditions and numbers are
recorded in `LISA_REPORT_AND_UPDATE_001.md`. Do not change benchmark
conditions or claim new numbers without a separate measured package.

---

## License

MIT.

---

*Built by LISA.*
