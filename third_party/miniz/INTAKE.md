# miniz — intake record

| Field | Value |
| :--- | :--- |
| Project | miniz (zlib / deflate and zip archive library, single file) |
| Source URL | https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip |
| Version / tag | 3.1.2 (tag ref 77d0dce8627735138c51770d1799a1ef48f2117d) |
| Commit or archive hash | SHA-256 f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a |
| License (SPDX) | MIT |
| Files used | `miniz.c`, `miniz.h` (release amalgamation) |
| Local modifications | None |
| Required notices | MIT license text |
| Added binary size | Measured with W5b (report 012) |
| Shipped in binary | Yes |
| Date of intake | 2026-09-19 |
| Reviewed by | |

## Why this component

Plan decision 16 (W5b): a `.docx` file is a zip archive of XML parts;
LISA reads `word/document.xml` and `docProps/core.xml` from it. Two
files, C, no dependencies.

## Build configuration

Built as `lisa_miniz` with warnings suppressed and
`MINIZ_NO_ARCHIVE_WRITING_APIS` (LISA only reads archives) and
`MINIZ_NO_ZLIB_COMPATIBLE_NAMES` (no clash with a system zlib).

## Update procedure

Download the new release amalgamation, record its SHA-256, replace the
two files, update this record, run the full test suite.
