# third_party

Vendored open-source components, statically linked into the `lisa`
binary. Rules (see `CLAUDE.md` and `LISA_COMPLETION_PLAN.md` §5):

- Each component lives in `third_party/<name>/` at a pinned version.
- Each has `LICENSE` (upstream text, unmodified) and `INTAKE.md`
  (copy `INTAKE_TEMPLATE.md`).
- Only permissive licenses: MIT, BSD-2/3, ISC, Apache-2.0, zlib,
  public domain.
- Do not modify vendored code unless unavoidable; record every change
  in the component's `INTAKE.md`.
- Third-party code builds without LISA's warning flags (`-w`, or added
  before them in CMake); LISA's own code keeps `-Wall -Wextra`.

| Component | Version | License | Used by |
| :--- | :--- | :--- | :--- |
| [sqlite](sqlite/INTAKE.md) | 3.53.4 | Public domain | Storage v2 metadata, WAL, FTS5 (W2, W7) |
| [md4c](md4c/INTAKE.md) | 0.5.3 | MIT | Markdown to text (W5) |
| [utf8proc](utf8proc/INTAKE.md) | 2.11.3 | MIT + Unicode data licence | Unicode normalisation (W5) |
| [pdfium](pdfium/INTAKE.md) | chromium/8057 (built from source) | BSD-3 / Apache-2.0 + bundled permissive | PDF text extraction (W5) |
| [llama.cpp](llama.cpp/INTAKE.md) | v0.4.1 | MIT (+ bundled permissive vendor code) | Model runtime, embeddings (W4) |
| [yyjson](yyjson/INTAKE.md) | 0.12.0 | MIT | JSON for HTTP, CLI `--json`, config (W9) |
| [civetweb](civetweb/INTAKE.md) | v1.16 | MIT (+ zlib-style md5, public-domain sha1) | Local HTTP server (W9) |
| [unity](unity/INTAKE.md) | 2.7.0 | MIT | C unit tests (test-only, not shipped) |
