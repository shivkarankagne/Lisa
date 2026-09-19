# CivetWeb — intake record

| Field | Value |
| :--- | :--- |
| Project | CivetWeb (embeddable HTTP server in C) |
| Source URL | https://github.com/civetweb/civetweb/archive/refs/tags/v1.16.tar.gz |
| Version / tag | v1.16 (tag ref d7ba35bbb649209c66e582d5a0244ba988a15159) |
| Commit or archive hash | SHA-256 f0e471c1bf4e7804a6cfb41ea9d13e7d623b2bcc7bc1e2a4dd54951a24d60285 |
| License (SPDX) | MIT (CivetWeb); `md5.inl`: zlib-style (L. Peter Deutsch); `sha1.inl`: public domain |
| Files used | `src/civetweb.c`, `include/civetweb.h`, and the inline parts it includes in this configuration: `md5.inl`, `sha1.inl`, `sort.inl`, `match.inl`, `response.inl`, `timer.inl`, `handle_form.inl` |
| Local modifications | None |
| Required notices | CivetWeb MIT license (first section of `LICENSE.md`); md5 notice in `md5.inl` |
| Added binary size | Measured with W9 (report 010) |
| Shipped in binary | Yes |
| Date of intake | 2026-09-19 |
| Reviewed by | |

`LICENSE.md` also lists Lua, SQLite, Duktape, zlib and others; those
belong to optional modules that are not vendored or compiled here.

## Why this component

Plan decision 8 and §5 (W9): replaces the hand-written `src/api/http.c`
(defects D5–D7). Handles header parsing across reads, keep-alive,
chunked responses, and a worker thread pool. C, MIT, statically linked.

## Build configuration

Built as `lisa_civetweb` with warnings suppressed and:
`NO_SSL` (local-only server, no TLS), `NO_CGI`, `NO_FILES` (no file
serving: LISA serves its own embedded assets), `NO_CACHING`,
`USE_IPV6` off. LISA binds it to 127.0.0.1 only.

## Update procedure

Download the new release tag archive, record its SHA-256, replace the
files listed above (add any new `.inl` the configuration includes),
update this record, run the full test suite.
