# LISA — Report and Update 002

**Date:** 2026-09-18
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W1 — Build foundation
**Commits:** `e721f62` … `d3f5edc` (8 commits)

---

## 1. Status

W1 is complete except for one item that cannot be verified locally: the
CI workflow has not run yet, because the commits are not pushed.

| Acceptance criterion (plan §6 W1) | Status |
| :--- | :--- |
| CMake builds `build/lisa`; `ctest` builds every test from source and runs all of them, incl. ASan/UBSan | Done — 16 tests, both configurations |
| Product built at `-O2` | Done |
| D1 fixed | Done |
| D2 fixed: no per-query copy | Done — product path; legacy asm path kept test-only |
| All OS calls through `src/platform/` | Done, except `src/api/http.c` (sockets), which W9 replaces |
| Kernels called only through the registry | Done for the product path |
| `third_party/` layout + intake template | Done |
| SQLite and llama.cpp vendored | Done (not yet linked into `lisa`: W2 and W4) |
| Unity tests | Done — vendored; new tests use it |
| CI on ARM64 macOS | Written; **not yet run** (needs push) |

## 2. Findings

### 2.1 The `-O2` "miscompile" was an ABI bug in the assembly kernel

Report 001 attributed fuzzer failures at `-O2` to "compiler
miscompilation of the comparison". That was wrong. The fuzzer printed
`diff=1.9e-05 cond=1`, i.e. `1.9e-05 > 0.0001` evaluated true.

Cause: the kernel used `v8` as a scratch register. AAPCS64 requires
`d8`–`d15` (low halves of `v8`–`v15`) to be preserved across calls. At
`-O2` the caller held the `0.0001f` tolerance in `d8` across the kernel
call; the kernel overwrote it. At `-O0` values live in memory, which hid
the bug.

The kernel also used the full `x2`/`x3` registers for 32-bit `int`
arguments, whose upper halves are undefined (D1 as originally described).

Both fixed. `test_kernel_abi` calls the kernel with sentinels in
`d8`–`d15` and garbage in the upper halves of `x2`/`x3`; it fails on the
old kernel and passes on the new one.

### 2.2 The per-query copy dominated search time

The old product path padded and copied the full dataset on every query.
The new `lisa_search` computes distances through the kernel registry in
256-vector blocks using a NEON intrinsics kernel that handles any
dimension, with O(k) memory.

Informal timing (not a benchmark report; plan L1 owns the formal one):

| Path | Mean per query |
| :--- | :--- |
| Scalar reference | 5.6 ms |
| Old: asm kernel + per-query copy | 2.7 ms |
| New: `lisa_search` (NEON intrinsics) | 0.55 ms |

Conditions: Apple M2, macOS 26.6.2, Apple clang 21, `-O2`, 10,000 × 768
random vectors, k = 5, 100 queries after 5 warm-up, single thread.

CLI output indices are identical to the old path on the test data;
distances differ only at rounding level (6th decimal).

Report 001's 1.644 ms figure was measured with the ABI-violating kernel
and should not be reused.

### 2.3 llama.cpp builds as a single-binary-compatible static library

- Static libraries, Metal shaders embedded, `GGML_NATIVE` off, OpenMP off.
- A probe program linking it depends only on macOS system frameworks
  (Accelerate, Metal, MetalKit, Foundation, CoreFoundation) and system
  libraries.
- **First launch compiles Metal shaders: ~17.6 s on an M2.** Cached by
  macOS afterwards (~0.01 s). W4 must show progress on first launch; W12
  must measure it on a clean machine.

## 3. Files

| Area | Files |
| :--- | :--- |
| Build | `CMakeLists.txt` (new), `Makefile` (removed), `.gitignore` |
| Kernels | `src/kernels/{kernels.h, kernels_internal.h, kernels_scalar.c, registry.c}`, `src/kernels/arm64/l2_neon.c`, `src/kernels/arm64/lisa_asm.h`, `src/kernels/arm64/lisa_ultra_mac.s` (fixed), `src/kernels/arm64/lisa.s` (removed: contained "404: Not Found") |
| Retrieval | `src/retrieval/retrieval.h` (added `lisa_search`), `src/retrieval/retrieval_search.c` |
| Platform | `src/platform/platform.h`, `src/platform/platform_posix.c` |
| Callers | `src/cli/main.c`, `src/api/http.c` (use `lisa_search`), `src/storage/storage.c` (uses platform layer) |
| Third party | `third_party/{README.md, INTAKE_TEMPLATE.md}`, `sqlite/`, `unity/`, `llama.cpp/` |
| Tests | `test_kernel_abi.c` + `kernel_abi_probe.s`, `test_kernels.c`, `test_search.c`, `test_platform.c`, `test_sqlite.c`, `test_llama_link.c`, `gen_test_data.c`; `test_cli.sh` / `test_api.sh` paths configurable (no assertion changes) |
| CI | `.github/workflows/ci.yml` |

## 4. API changes

All additive; no existing signature, error code, or output format
changed.

- `retrieval.h`: new `lisa_search()` with `int64_t` sizes and a new
  error `-4` (allocation failure, only when k > 256).
- New module headers: `kernels.h`, `platform.h`, `lisa_asm.h`.
- The CLI and HTTP server now call `lisa_search` instead of
  `lisa_search_exact_asm`. Output format unchanged.

## 5. Tests

16 tests, all passing in Release (`-O2`) and Debug + ASan/UBSan:

    test_retrieval, test_retrieval_asm, test_edge_cases, test_storage,
    test_storage_mutation, test_kernel_abi, test_kernels, test_platform,
    test_search, test_search_scalar, test_sqlite, test_llama_link,
    fuzz_retrieval_asm (1000 iterations), gen_test_data, test_cli, test_api

Before W1, `make test` ran prebuilt binaries and skipped
`test_storage_mutation` and `test_api.sh`.

## 6. Open items and assumptions

- **CI not yet run.** Needs the commits pushed to GitHub.
- `src/api/http.c` still makes socket calls outside the platform layer
  (D5–D7); removed in W9.
- The legacy assembly kernel and `lisa_search_exact_asm` remain,
  test-only. Whether assembly returns is decided by the L1 shoot-out.
- Shell tests still write under `/tmp` (pre-existing); to be moved into
  the build tree when they are next touched.
- Existing tests keep their hand-written check style; only new tests use
  Unity.
- Repository size grew by ~44 MB (llama.cpp 34 MB, SQLite 9.8 MB).

## 7. Next

W2 — storage v2: memory-mapped vector file + SQLite metadata (WAL),
stable IDs, versioned formats, embedding model ID per collection,
migration from v1/v1.1. Fixes D4.
