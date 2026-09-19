# LISA — Report and Update 006

**Date:** 2026-09-19
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W5 — Documents
**Commits:** `f667836` (part 1), `b6e47f3` (part 2)

---

## 1. Status

W5 is complete.

| Acceptance criterion (plan §6 W5) | Status |
| :--- | :--- |
| txt, md, pdf; each format one extractor registered by file type | Done |
| Output: normalised UTF-8 + offsets (page number for PDF) | Done |
| Chunker: configurable size and overlap; respects paragraphs | Done |
| PDFium links statically (verified first; fallback: build from source) | Done via the fallback — see §3 |

## 2. What was built

- **Normalisation:** UTF-8 (±BOM), UTF-16 LE/BE (BOM), else
  Windows-1252; CRLF/CR → LF; form feed → paragraph break; control
  characters removed; Unicode NFC (utf8proc). All offsets refer to this
  normalised text.
- **Extractors:** plain text; Markdown (md4c — markup removed, visible
  words kept, HTML dropped, entities decoded, first H1 = title); PDF
  (PDFium — per-page text layer, page offsets, metadata title; encrypted
  and corrupt files reported distinctly; scanned pages yield no text).
- **Registry** keyed by extension: the seam for DOCX, HTML, OCR (L3).
- **Chunker:** packs paragraphs to a target size (default 1,000
  characters, max 1,500, overlap 150); splits long paragraphs at
  sentence ends including the Devanagari danda; hard-splits unbreakable
  text at the limit; records the start page of each chunk.

## 3. PDFium

pdfium-binaries publishes only a shared library for macOS; a separate
dylib would break the single-executable rule. Per the plan's fallback,
`scripts/build_pdfium.sh` builds a static `libpdfium.a` (17 MB archive)
from a pinned commit with V8 and XFA disabled.

- Requires **full Xcode** on macOS (Chromium's build reads SDK
  information the Command Line Tools lack). Installed on the development
  Mac on 2026-09-19. GitHub's macOS runners include Xcode; CI builds
  PDFium once and caches it.
- First build: ~4.2 GB source checkout plus ~1,100 compile steps.
- Links only system frameworks: AppKit, CoreFoundation, CoreGraphics,
  Foundation, Security.
- A test program with PDFium linked is 6.4 MB.

## 4. Findings

1. **Chunker overlap bug (fixed):** when the previous chunk was shorter
   than the overlap size, the next chunk re-contained all of it. Found by
   the randomised test (200 cases); overlap now always starts after the
   previous chunk's start.
2. **Test depended on test order (fixed):** `test_documents` assumed
   another test had created the scratch directory. Exposed by a clean
   build.
3. **Toolchain switch needs a clean build:** after Xcode was installed,
   llama.cpp's precompiled header no longer matched the compiler.
   Deleting `build/` and `build-asan/` fixed it; noted in `CLAUDE.md`.
4. **Build setup gotchas in `build_pdfium.sh` (fixed):** depot_tools must
   be bootstrapped with its `.cipd_bin` directory on `PATH`; the Xcode
   licence must be accepted (`sudo xcodebuild -license accept`).

## 5. Tests

22 tests, all passing in Release and ASan/UBSan builds.
`test_documents` (13 tests): control characters and line endings; UTF-8
BOM, UTF-16 LE/BE, Windows-1252 fallback; NFC and Devanagari; registry;
text file; Markdown (lists, tables, entities, links, code, HTML, title);
chunking (empty, single paragraph, packing with overlap, sentence splits
in English and Hindi, unbreakable text, pages, 200 randomised texts
checking every documented guarantee); PDF (two pages, page numbers,
title, corrupt and missing files).

PDFium itself is not built with sanitizers (it is compiled separately);
LISA's PDF code is.

## 6. Open items

- Chunk page numbers are produced but not yet stored: storage v2 has no
  page column. W6 adds it (format version bump with migration).
- Extraction is single-threaded for PDFs (PDFium is not thread-safe);
  W6's background ingest must run PDF extraction on one thread.
- OCR for scanned PDFs and more formats (DOCX, HTML, ...) are L3.

## 7. Next

W6 — ingest: folder sync (add, update, remove by content hash),
background job with progress, ingest and ask at the same time.
