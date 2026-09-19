# utf8proc — intake record

| Field | Value |
| :--- | :--- |
| Project | utf8proc (JuliaStrings) |
| Source URL | https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v2.11.3.tar.gz |
| Version / tag | v2.11.3 (tag object e5e799221b45bbb90f5fdc5c69b6b8dfbf017e78) |
| Commit or archive hash | SHA-256 abfed50b6d4da51345713661370290f4f4747263ee73dc90356299dfc7990c78 |
| License (SPDX) | MIT AND Unicode-DFS-2016 (Unicode data tables) |
| Files used | `utf8proc.c`, `utf8proc.h`, `utf8proc_data.c` (included by `utf8proc.c`) |
| Local modifications | None |
| Required notices | MIT license text and the Unicode data license (both in `LICENSE`) |
| Added binary size | Measured with W5 |
| Shipped in binary | Yes |
| Date of intake | 2026-09-18 |
| Reviewed by | |

## Why this component

Unicode normalisation (NFC) and UTF-8 validation for extracted document
text, so the same text always produces the same bytes regardless of how
the source encoded it. Small, portable C, widely used (Julia, PostgreSQL
extensions).

## Build configuration

Built as `lisa_utf8proc` with `UTF8PROC_STATIC` defined and warnings
suppressed.

## Update procedure

Download the new release tag archive, record its SHA-256, replace the
three files, update this record, run the full test suite.
