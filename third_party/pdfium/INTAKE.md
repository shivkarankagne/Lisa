# PDFium — intake record

PDFium is **built from source** by `scripts/build_pdfium.sh`, once per
platform, and each result is published as a prebuilt archive that
developers and CI download and verify with `scripts/fetch_pdfium.sh` into
`.deps/pdfium` (not committed). pdfium-binaries ships only shared
libraries (`libpdfium.dylib` on macOS, `libpdfium.so` on Linux), and a
shared library would break the single-executable rule, so LISA links its
own static `libpdfium.a`. The macOS archive is built and published by
hand; the Linux archive is built and published by
`.github/workflows/build-pdfium-linux.yml`.

## Published archives

| Platform | Asset (release tag `deps-<asset>`) | SHA-256 |
| :--- | :--- | :--- |
| macOS arm64 | `pdfium-chromium-8057-mac-arm64.tar.gz` | `07ae3e816fee0626ffd1c31cc3b10be3bbadccad3eb3033691b777d1dd5c0ba9` |
| Linux x64 | `pdfium-chromium-8057-linux-x64.tar.gz` | `f9819b5206d0875c2c35392214c0dca99db966577bca960a78b2131bbc8b7b1e` |

| Field | Value |
| :--- | :--- |
| Project | PDFium (Google; the PDF engine in Chrome) |
| Source URL | https://pdfium.googlesource.com/pdfium.git |
| Version / tag | branch `chromium/8057` (the release pdfium-binaries 155.0.8057.0 is built from) |
| Commit | `a5a7089234f121990b336b3841008009dca143bf` |
| License (SPDX) | BSD-3-Clause AND Apache-2.0 (PDFium) plus bundled components below |
| Files used | Static library `libpdfium.a` and headers `public/*.h` from the build |
| Local modifications | None. Build configuration only (below). |
| Required notices | PDFium LICENSE and the bundled components' licences, in THIRD_PARTY_NOTICES (W11) |
| Added binary size | Measured when first linked (W5 part 2) |
| Shipped in binary | Yes |
| Date of intake | 2026-09-19 |
| Reviewed by | |

## Bundled components (compiled into libpdfium.a)

From the licence files of the corresponding pdfium-binaries release:

| Component | Licence |
| :--- | :--- |
| Anti-Grain Geometry 2.3 | AGG licence (permissive, BSD-style) |
| FreeType | FreeType License (BSD-style with credit; the GPLv2 option is not used) |
| Little CMS | MIT |
| libjpeg-turbo | IJG + BSD-3-Clause + zlib |
| OpenJPEG | BSD-2-Clause |
| libpng | libpng licence |
| zlib | zlib |
| ICU | Unicode License v3 |
| abseil | Apache-2.0 |
| fast_float | MIT / Apache-2.0 |
| simdutf | MIT / Apache-2.0 |
| llvm-libc | Apache-2.0 with LLVM exception |

All permissive. V8 (JavaScript) and XFA forms are disabled, which also
excludes their dependencies.

## Why this component

Best available text extraction from PDFs (reading order, ToUnicode maps
for Indian and other scripts, broken files), permissively licensed, and
portable to every future LISA platform. Rejected alternatives: MuPDF
(AGPL), Poppler/xpdf (GPL), Apple PDFKit (macOS only), pdfio (no text
extraction).

## Build configuration (`scripts/build_pdfium.sh`)

| GN argument | Value | Why |
| :--- | :--- | :--- |
| `pdf_is_complete_lib` | true | One static archive with all dependencies |
| `pdf_is_standalone` | true | Build outside Chromium |
| `pdf_enable_v8`, `pdf_enable_xfa` | false | No JavaScript engine or XFA forms: smaller, smaller attack surface |
| `pdf_use_skia` | false | AGG renderer; LISA only extracts text |
| `use_custom_libcxx` | false | Link against the system libc++ like the rest of LISA |
| `is_component_build` | false | Static |
| `is_debug` | false | Release |

Build requirements: git, python3, network, ~5 GB disk, and on macOS a
full **Xcode** installation (Chromium's build reads SDK information that
the Command Line Tools do not provide).

## Prebuilt archive

| Field | Value |
| :--- | :--- |
| Release | `deps-pdfium-chromium-8057-mac-arm64` in this repository |
| Asset | `pdfium-chromium-8057-mac-arm64.tar.gz` (7.0 MB) |
| SHA-256 | `07ae3e816fee0626ffd1c31cc3b10be3bbadccad3eb3033691b777d1dd5c0ba9` |
| Built by | `scripts/build_pdfium.sh` at commit `2724c02`, Xcode 27.0, Apple M2 |
| Contents | `lib/libpdfium.a`, `include/`, `LICENSE`, `VERSION` (provenance) |

Anyone can rebuild with `scripts/build_pdfium.sh` and compare. The fetch
script refuses an archive whose hash does not match.

## Update procedure

1. Pick the branch matching the new pdfium-binaries release; record its
   commit in the script and here.
2. Re-check the bundled component licences.
3. Run `scripts/build_pdfium.sh`, the full test suite, and `otool -L build/lisa`.
4. Package `.deps/pdfium` as a new release asset (new tag per version and
   platform); update `TAG`, `ASSET`, `SHA256`, `COMMIT` in
   `scripts/fetch_pdfium.sh` and the table above.
