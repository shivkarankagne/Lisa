# llama.cpp / ggml — intake record

| Field | Value |
| :--- | :--- |
| Project | llama.cpp (includes ggml) |
| Source URL | https://github.com/ggml-org/llama.cpp/archive/refs/tags/v0.4.1.tar.gz |
| Version / tag | v0.4.1 (latest stable release, 2026-09-14; commit b29c606e28a0) |
| Commit or archive hash | SHA-256 ef3d5b1907a391500ae11b5e61a8e2022e0deaac9790899cad9c4e02f03bfb9a |
| License (SPDX) | MIT |
| Files used | `CMakeLists.txt`, `cmake/`, `include/`, `src/`, `ggml/`, `vendor/` (except `vendor/cpp-httplib`), `LICENSE`, `licenses/` |
| Local modifications | None to any file. Directories not needed for a library build were not copied (tools, examples, tests, common, docs, models, python, app, `vendor/cpp-httplib`). |
| Required notices | MIT (`LICENSE`); nlohmann/json MIT (`licenses/LICENSE-jsonhpp`); bundled vendor code listed below |
| Added binary size | Probe program linking libllama: 6.8 MB. Measured for `lisa` when linked (W4). |
| Shipped in binary | Yes (from W4) |
| Date of intake | 2026-09-18 |
| Reviewed by | |

### Bundled vendor code (in `vendor/`)

| Component | License |
| :--- | :--- |
| nlohmann/json | MIT |
| stb_image | Public domain / MIT |
| miniaudio | Public domain / MIT-0 |
| sheredom/subprocess.h | Unlicense (public domain) |
| hash: sha1 | Public domain |
| hash: sha256 | Public domain |
| hash: xxhash | BSD-2-Clause |
| hash: rotate-bits | MIT |

All permissive; compatible with LISA's Apache-2.0 core and with
proprietary enterprise builds.

## Why this component

Plan decision 9: model loading (GGUF), tokenisation, quantisation, KV
cache, CPU (NEON) and Metal execution, and embedding models, behind
LISA's own model interfaces (W4). Building an equivalent runtime would be
the largest single cost in the roadmap. ggml also supports CUDA, Vulkan,
and other backends, which stay in the tree but disabled, so future
platforms are a configuration change (plan §2a).

## Build configuration

Set in the top-level `CMakeLists.txt`, added with `EXCLUDE_FROM_ALL` so
only targets that link it build it:

| Option | Value | Why |
| :--- | :--- | :--- |
| `BUILD_SHARED_LIBS` | OFF | Single executable |
| `GGML_METAL` | ON | Apple GPU acceleration |
| `GGML_METAL_EMBED_LIBRARY` | ON | Metal shaders compiled into the binary; no side files |
| `GGML_NATIVE` | OFF | Binary built on one Apple Silicon generation runs on all (no `-mcpu=native`) |
| `GGML_OPENMP` | OFF | Would add a runtime dependency (libomp) |
| `GGML_CCACHE` | OFF | Reproducible builds; no dependence on a local tool |
| `LLAMA_BUILD_COMMON/TESTS/TOOLS/EXAMPLES/SERVER/APP/UI` | OFF | Library only |

Resulting links (probe program): only macOS system frameworks
(Accelerate, Metal, MetalKit, Foundation, CoreFoundation) and system
libraries (libSystem, libc++, libobjc).

## Observed behaviour to account for

- **First launch compiles Metal shaders: ~17.6 s on an M2.** macOS caches
  the result; later launches load them in ~0.01 s. W4 must show progress
  during first launch, and W12 must measure it on a clean machine.
- Accelerate (Apple BLAS) is used automatically on macOS. It is a system
  framework, allowed as an optional backend (plan decision 3).

## Update procedure

1. Download the new release tag archive; record its SHA-256.
2. Replace the directories listed under "Files used" (same exclusions).
3. Re-check `vendor/` licenses and update the table above.
4. Update this record; run the full test suite in both build
   configurations; check `otool -L build/lisa`.
