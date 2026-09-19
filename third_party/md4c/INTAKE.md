# md4c — intake record

| Field | Value |
| :--- | :--- |
| Project | md4c (Markdown parser for C) |
| Source URL | https://github.com/mity/md4c/archive/refs/tags/release-0.5.3.tar.gz |
| Version / tag | release-0.5.3 (commit 472c417005c2c71b8617de4f7b8d6b30411d78f4) |
| Commit or archive hash | SHA-256 353c346f376b87c954a13f3415ede2d51264cc61dc5abcd38ff1d2aa0d059b9e |
| License (SPDX) | MIT |
| Files used | `src/md4c.c`, `src/md4c.h` (parser only; the HTML renderer is not used) |
| Local modifications | None |
| Required notices | MIT license text |
| Added binary size | Measured with W5 |
| Shipped in binary | Yes |
| Date of intake | 2026-09-18 |
| Reviewed by | |

## Why this component

CommonMark-compliant Markdown parser in C with a callback (SAX-style) API
and no dependencies. LISA uses it to turn `.md` files into plain text with
paragraph structure for chunking (W5).

## Build configuration

Built as `lisa_md4c` with warnings suppressed. Default flags; the parser
is given `MD_DIALECT_GITHUB` (tables, strikethrough, autolinks).

## Update procedure

Download the new release tag archive, record its SHA-256, replace the two
files, update this record, run the full test suite.
