# CLAUDE.md — LISA

Instructions for AI coding agents (Claude, Astra) and engineers working in
this repository. Read this file fully before any change.

## What LISA is

A local-first AI runtime shipped as **one native executable**. A user copies
the `lisa` binary onto a Mac, points it at a folder of documents, and asks
questions. LISA ingests, stores, searches, and answers with citations using a
local model — with no Python, Docker, database server, or LLM server
installed, and no internet.

Licensing: this repository is **source-available under BUSL-1.1**. It is
free for individuals' personal use; organizations need a commercial
license. Paid enterprise features live in a separate private repo
(`lisa-enterprise`) and must never be added here. Do not accept outside
contributions until a Contributor License Agreement is in place.

## Current phase: the working product (W0–W12)

**The only goal right now is a working LISA 1.0** as defined in
`LISA_COMPLETION_PLAN.md` §0. Work only on packages W0–W12 (§6 of the plan).

- `LISA_COMPLETION_PLAN.md` is the source of truth: decisions (§2),
  portability rules (§2a), open-core rules (§2b), known defects (§4),
  reuse map (§5), work packages + acceptance criteria (§6), upgrade seams
  (§7).
- Everything in plan §8 (L1–L11, E1–E4) is **out of scope** until W12
  passes. Do not start it, stub it, or "prepare" for it beyond the seams
  listed in §7.
- If a task seems to need something from §8, stop and ask.

### Work package order

```
W0 (parallel, anytime)
W1 → W2 → W3 → W4, W5 (parallel) → W6, W7 → W8 → W9 → W10 → W5b, W11 → W12
```

W5b (docx + OCR of scanned PDFs) was moved into 1.0 from §8 L3 by the
user on 2026-09-19 (plan decision 16).

W1–W9 are done (reports `LISA_REPORT_AND_UPDATE_002.md`–`_010.md`);
next is **W10** (GUI; W11 may run in parallel). Check the plan's acceptance criteria for the package you
are on; a package is not done until every criterion is met.

## Non-negotiable rules

1. **Single executable.** `otool -L build/lisa` must list only macOS system
   libraries and frameworks. Everything else is statically linked.
2. **No runtime network access.** LISA never contacts external hosts. The
   only socket is the local HTTP server on 127.0.0.1.
3. **Reuse before building.** If a permissively licensed library in plan §5
   does the job, use it. Build in-house only what §5 lists under "What LISA
   builds itself".
4. **Licenses.** Accept MIT, BSD-2/3, ISC, Apache-2.0, zlib, public domain.
   Reject GPL, AGPL, LGPL, SSPL, dual GPL/commercial. Check the exact pinned
   version.
5. **Correctness before speed.** A reference implementation and tests come
   before any optimisation. No performance claim without a measured report
   under stated conditions.
6. **Build the seams (plan §7).** Every package must leave the seam it owns
   in place, so later features plug in without rewrites.
7. **Portable by default (plan §2a).** Only ARM64 macOS is built for 1.0,
   but code must not assume it.
8. **Say when something is incomplete.** Never hide missing functionality
   behind a plausible-looking abstraction or stub.

## What NOT to do

These rules exist so that work on one part never breaks another. If a task
cannot be done without breaking one of them, **stop and ask** — do not work
around it.

### Scope

- **Do not work on more than one work package at a time.** Finish, test,
  and commit one before touching the next.
- **Do not edit files outside the current package's area** unless the
  change is required for it. If it is, say which file, why, and who else
  uses it before editing.
- **Do not make drive-by changes:** no reformatting, renaming, reordering,
  "cleanup", or refactoring of code the task does not require.
- **Do not start or stub anything from plan §8** (L1–L11, E1–E4).
- **Do not delete files, functions, or code that looks unused** without
  searching the whole repo for consumers first and stating the result.
- **Do not edit `LICENSE`, `NOTICE`, `CLAUDE.md`, or
  `LISA_COMPLETION_PLAN.md`** unless the task is to change them.

### Contracts

- **Do not change a public or shared interface silently.** This includes
  `include/lisa.h`, every module header (`src/*/*.h`), file formats, HTTP
  routes and JSON fields, CLI flags, CLI output format, and exit codes.
  Any change needs: all consumers found and updated, a version bump, and a
  note in the report.
- **Do not change the meaning of an existing error code, return value,
  default value, or output field.** Add new ones instead.
- **Do not change an on-disk format without a version bump and a
  migration** that is tested against a saved file in the old format.
- **Do not change the scalar reference implementation's behaviour.** It is
  the oracle every optimised path is tested against.
- **Do not reach into another module's internals.** Use its header. Never
  include another module's `.c` file or declare its functions with
  `extern`.

### Tests

- **Do not change, weaken, skip, or delete an existing test to make a
  change pass.** A failing test means the change is wrong, or the test's
  contract changed — and a contract change needs explicit approval.
- **Do not loosen numeric tolerances** in differential or fuzz tests.
- **Do not run only the tests for the module you changed.** Run the full
  suite, including sanitizer builds, before every commit.
- **Do not commit with any failing test**, build warning you introduced, or
  sanitizer report.
- **Do not make tests depend on network access, wall-clock time, or files
  outside the repo.**

### Build and dependencies

- **Do not change global build flags** (optimisation level, sanitizers,
  warnings, C standard) to fix one module's problem.
- **Do not add a dependency** that is not in plan §5 without an intake
  record and approval. Never add a runtime dependency.
- **Do not modify vendored code in `third_party/`** unless unavoidable;
  record every change in its `INTAKE.md`.
- **Do not add platform-specific code** (`#ifdef __APPLE__`, system
  headers, Apple frameworks) outside `src/platform/` or kernel backends.
- **Do not change benchmark conditions or publish numbers** without a new
  measured report.

### Git

- **Do not mix unrelated changes in one commit.**
- **Do not rewrite history** (`--amend` on pushed commits, rebase of shared
  branches, force push).
- **Do not push** unless asked.
- **Do not commit** build output, test binaries, generated data (`*.bin`),
  model files (`*.gguf`), secrets, or local config.

### Safe change checklist

Before changing code:

1. Run the full test suite and record the baseline.
2. Search the repo for every consumer of what you are about to change.
3. State what will change and what must not change.

After changing code:

1. Full test suite passes (including sanitizers) — same or more tests than
   the baseline, none removed.
2. Bug fixes and new behaviour have new tests.
3. `git diff` contains only files the task needed.
4. The report lists any interface, format, or behaviour change.

## Architecture rules

- **Dependency direction** (lower never depends on higher):
  `platform → kernels → storage / retrieval / documents → models →
  context → api (lisa.h) → cli / http → gui`
- **All OS calls** (files, mmap, locks, threads, time, paths) go through
  `src/platform/`. No `#include <unistd.h>`, `<sys/mman.h>` etc. outside it.
- **All compute kernels** are called through the kernel registry, never
  directly.
- **CLI, HTTP, and GUI use only the public API** in `include/lisa.h`.
- **Sizes, counts, offsets, IDs:** `int64_t` / `uint64_t`. Never `int` or
  `long` for sizes.
- **File formats:** little-endian, fixed-width types, version field,
  documented layout. Every format change bumps its version and adds a
  migration.
- **HTTP API** lives under `/v1`. Changes within v1 are additive only.
- **Enterprise extension points** (`lisa_auth_provider`,
  `lisa_retrieval_filter`, `lisa_audit_sink`, `lisa_storage_crypto`,
  `lisa_http_routes`) exist in the core as no-ops. Never implement
  enterprise behaviour behind them in this repo.

## Third-party code

- Vendored into `third_party/<name>/` at a pinned version or commit. No git
  submodules, no package managers at build time.
- Each component needs `third_party/<name>/LICENSE` (unmodified) and
  `third_party/<name>/INTAKE.md`: project, version/commit, license, source
  URL, files used, local modifications, required notices, added binary
  size, reason for selection.
- Apache-2.0 components: keep upstream `NOTICE`; mark modified files.
- Do not modify vendored code unless unavoidable; record every change in
  `INTAKE.md`.

## Code conventions

- LISA code is C (C11). C++ is allowed only inside `third_party/` (e.g.
  llama.cpp) and in the thin wrapper that calls it.
- Public symbols are prefixed `lisa_`; module-internal symbols use the
  module prefix (e.g. `storage_`) and are `static` where possible.
- Match the surrounding style: 4-space indent, block comments above
  functions describing the contract, errors as documented negative return
  codes.
- New source files start with `/* SPDX-License-Identifier: BUSL-1.1 */`.
- Explicit ownership and lifetime in every public function's comment.
- No global mutable state outside handle tables owned by one module.

## Build and test

    scripts/fetch_pdfium.sh              # once: prebuilt static PDFium into .deps/ (verified)
    cmake -S . -B build                  # Release, -O2
    cmake --build build
    ctest --test-dir build --output-on-failure

    cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLISA_SANITIZE=ON
    cmake --build build-asan
    ctest --test-dir build-asan --output-on-failure

- Every change: all tests pass, including ASan/UBSan variants.
- New behaviour needs tests. Bug fixes need a regression test.
- Test data (`*.bin`, query files) is generated locally and is gitignored.
  Model files (`*.gguf`) are never committed. Model tests need the files
  in `models/` (see `docs/models.md`); without them they are reported as
  ignored, not passed.
- Programs must free every loaded model before exiting: llama.cpp aborts
  at exit otherwise.
- After a compiler or Xcode change, delete `build/` and `build-asan/`
  (precompiled headers go stale).

## Known defects (plan §4)

| ID | Where | Status |
| :--- | :--- | :--- |
| D1 | Assembly kernel ABI (32-bit args in x2/x3; clobbered callee-saved d8) | Fixed in W1 (`e721f62`) |
| D2 | Asm wrapper copied the whole dataset per query | Fixed in W1 (`7ed0041`): product uses `lisa_search`; the asm path is test-only |
| D3 | `-O0` build | Fixed in W1 (`6f49747`): cause was D1, not the compiler |
| D4 | `storage_delete` renumbers indices | Fixed by storage v2 in W2 (`f1de8e0`); v1 module kept for the CLI/HTTP until W9 |
| D5–D7 | `src/api/http.c`: re-reads collection per request, arbitrary paths, single `recv`; direct socket calls outside `src/platform/` | Fixed in W9: `http.c` removed, replaced by `src/http/` on CivetWeb |
| D8 | Build/test hygiene | Fixed in W1 (`6f49747`, `c5a95cd`, `d3f5edc`) |

## Layout

Current:

    src/platform/       OS abstraction (POSIX)
    src/kernels/        kernel registry, scalar + NEON intrinsics kernels
    src/kernels/arm64/  legacy assembly kernel + wrapper (test-only path)
    src/retrieval/      scalar reference + lisa_search; RRF fusion; lisa_index seam (exact)
    src/storage/        storage v2 (store.h); v1.1 (storage.h) only for `lisa migrate`
    src/cli/            `lisa` command line (only lisa.h, src/app, src/http, src/platform)
    src/app/            program layer: data dir, config.json, model lookup, shared JSON
    src/http/           local HTTP API /v1 on CivetWeb (docs/http-api.md)
    src/gui/            `lisa gui` native window (webview); assets generated from gui/
    gui/                web UI (plain HTML/CSS/JS, no build step), embedded by cmake/embed_assets.cmake
    include/lisa.h      public API (W3)
    src/api/            lisa.h implementation
    src/models/         model runtime over llama.cpp (only code that includes llama.h)
    src/documents/      extraction (txt, md, pdf), normalisation, chunker
    src/ingest/         folder sync (skip unchanged, atomic replace, removals)
    src/context/        answer pipeline stages (filter, rank, dedupe, budget), citations
    scripts/            fetch_pdfium.sh (normal), build_pdfium.sh (upgrades; needs Xcode)
    third_party/        sqlite, llama.cpp, unity (see third_party/README.md)
    models/             downloaded model files (gitignored; see docs/models.md)
    docs/formats/       on-disk format specs
    tests/  benchmark/  .github/workflows/

Target for 1.0 (create directories as their package starts):

    include/lisa.h      the only public header (W3)
    src/platform/       OS abstraction, POSIX implementation (W1)
    src/kernels/        kernel registry + scalar + NEON (W1)
    src/storage/        storage v2 (W2)
    src/retrieval/      exact index, FTS5, fusion (W7)
    src/models/         llama.cpp wrapper: generate, embed (W4)
    src/documents/      extractor registry, txt/md/pdf, chunker (W5)
    src/ingest/         folder sync, background jobs (W6)
    src/context/        context pipeline + answer loop (W8)
    src/cli/  src/http/ interfaces (W9)
    gui/                web UI sources, compiled into the binary (W10)
    third_party/        vendored components
    tests/  benchmark/  docs/

## Finishing a work package

Each package ends with:

1. All acceptance criteria in plan §6 met.
2. Tests pass (including sanitizers).
3. Docs updated (READMEs, API comments, format specs).
4. A short report: files changed, API changes, test results, measured
   numbers (if any), open assumptions, anything incomplete.
5. A git commit per logical change. Commit messages: imperative subject
   ≤ 72 chars, body explains why.

## Before changing shared code

- Find every consumer of the function, struct, or format first.
- Changing `lisa.h`, a file format, or an HTTP route = API change: state the
  compatibility impact and bump the version (plan §7 upgrade rules).
