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
- Third-party targets build with warnings suppressed (`-w`); LISA's own
  code keeps `-Wall -Wextra`.

| Component | Version | License | Used by |
| :--- | :--- | :--- | :--- |
| [sqlite](sqlite/INTAKE.md) | 3.53.4 | Public domain | Storage v2 metadata, WAL, FTS5 (W2, W7) |
| [unity](unity/INTAKE.md) | 2.7.0 | MIT | C unit tests (test-only, not shipped) |
