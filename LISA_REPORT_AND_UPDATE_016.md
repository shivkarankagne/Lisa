# LISA — Report and Update 016

**Date:** 2026-09-22
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W13 — Linux headless port (decision 20)

---

## 1. Status

The portable core builds and passes its tests on x86-64 Linux. This is
the first step of W13: the command-line and local server, no GUI window,
no PDF, no GPU. It is done and verified on GitHub Actions' Ubuntu runner
(the work is written on a Mac, which cannot build Linux binaries).

The Linux CI job — Configure, Build, Test, and a runtime smoke check —
is green. macOS ARM64 (Release and sanitizer) stays green and unchanged.

## 2. What made the port possible

Almost nothing in the core needed changing: it was already written to
the platform seam. The Mac-specific surface was small and isolated, and
the build simply had to stop assuming macOS.

- **One codebase, per-OS backends at build time.** No second copy of the
  core, and no runtime OS switch. Each binary carries only its own
  platform backend.
- **Metal** is enabled only on Apple; Linux uses llama.cpp's portable
  CPU backend.
- **The NEON kernel** compiles only on ARM; the **assembly kernel** (a
  test-only path, plan D2) only on Apple ARM64. Where the assembly is
  absent, a scalar stand-in provides its test entry point so the
  differential tests still run.
- **OCR** is Apple Vision on macOS and a stub elsewhere: a scanned page
  reports "no text", the same as a PDF with no text layer.
- **Objective-C** is enabled only on Apple; the base languages are C,
  C++ and ASM.
- **x86-64** is now an accepted architecture (was ARM64-only, decision
  10); any other is still refused.
- **`lisa gui`** needs no change: it already falls back to opening the
  browser at the local server when there is no native window.

## 3. Bugs the port exposed

Each was found on real Linux through CI and fixed after verifying on
macOS that the fix changed nothing there.

| Symptom on Linux | Cause | Fix |
| :--- | :--- | :--- |
| `'pid_t' undeclared`, core would not compile | `app.c` called `getpid`/`kill` directly, an OS call outside the platform layer that macOS headers happened to satisfy | Added `lisa_process_id` / `lisa_process_alive` to the platform layer; `app.c` uses them and includes no OS headers |
| CMake configuration failed in seconds | `project()` required Objective-C, which gcc lacks | Objective-C enabled only under `if(APPLE)` |
| `test_documents` SEGFAULT | `strdup` implicitly declared under strict `-std=c11` on glibc; its pointer return truncated to `int` | Test targets compile with `_DEFAULT_SOURCE`, as the product enables POSIX per file |
| `test_http` `*** buffer overflow detected ***` | `realpath` given 1,024-byte buffers, but `PATH_MAX` is 4,096 on Linux and glibc's fortified `realpath` aborts below `PATH_MAX` | `realpath` buffers are now `PATH_MAX` |
| `useconds_t` undeclared; `usleep` hidden | BSD/macOS conveniences needing a feature macro on glibc | Tests use `lisa_sleep_ms` from the platform layer |
| `test_llama_link` failed `NEON = 1` / Metal | Apple-ARM64-only assertions | Guarded to Apple ARM64; every build still requires the CPU backend |

## 4. Not done (follow-ups)

- **PDFium on Linux.** The macOS build self-hosts a mac-arm64 static
  PDFium; Linux needs its own. Until then the Linux build is configured
  with `-DLISA_REQUIRE_PDF=OFF` and does not read PDFs.
- **Models and inference on Linux CI.** The Linux job runs the model-free
  tests; model tests are exercised on macOS.
- **`test_ingest` on Linux.** It asserts PDF handling, so it is skipped
  on the Linux job for now; making it PDF-config-aware is a follow-up.
- **The GUI on Linux.** Browser fallback works; a native GTK/WebKitGTK
  window is later.
- **A GPU backend** (CUDA or Vulkan) for Linux.
- **Tesseract OCR** for Linux and Windows.
- **Windows** is the next platform after Linux is fleshed out.

## 5. Next

Merge the `linux-headless` branch to `main` once its macOS release job is
green, then decide the order of the follow-ups above against the
remaining W12 manual tests (clean-machine and offline).
