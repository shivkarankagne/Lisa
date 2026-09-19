# yyjson — intake record

| Field | Value |
| :--- | :--- |
| Project | yyjson (JSON library for C) |
| Source URL | https://github.com/ibireme/yyjson/archive/refs/tags/0.12.0.tar.gz |
| Version / tag | 0.12.0 (tag ref 7871d321ff4cd8068c1f777c97975dc2fb640ab3) |
| Commit or archive hash | SHA-256 b16246f617b2a136c78d73e5e2647c6f1de1313e46678062985bdcf1f40bb75d |
| License (SPDX) | MIT |
| Files used | `src/yyjson.c`, `src/yyjson.h` |
| Local modifications | None |
| Required notices | MIT license text |
| Added binary size | Measured with W9 (report 010) |
| Shipped in binary | Yes |
| Date of intake | 2026-09-19 |
| Reviewed by | |

## Why this component

Plan §5 (W9): JSON reading and writing for the HTTP API, the CLI's
`--json` output, and the config file. Two files, C89, no dependencies,
strict RFC 8259 parsing, fast. 0.12.0 was chosen over 0.13.0 (released
2026-09-08) for maturity.

## Build configuration

Built as `lisa_yyjson` with warnings suppressed and default options.

## Update procedure

Download the new release tag archive, record its SHA-256, replace the two
files, update this record, run the full test suite.
