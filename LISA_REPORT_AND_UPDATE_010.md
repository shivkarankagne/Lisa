# LISA — Report and Update 010

**Date:** 2026-09-19
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W9 — Interfaces
**Commits:** `c9d6c6e` (vendor yyjson + CivetWeb), `98f9892` (platform),
`f96bb50` (CLI + HTTP API)

---

## 1. Status

W9 is complete, with one item handed to W10 (below).

| Acceptance criterion (plan §6 W9) | Status |
| :--- | :--- |
| CLI: `ingest`, `search`, `ask`, `serve`, `model`, `migrate`, `--version` | Done |
| CLI: `gui` | Command exists; says the GUI arrives with W10 (exit 5). The GUI itself is W10 |
| HTTP JSON API under `/v1`: health, collections, ingest, jobs, search, ask | Done — `docs/http-api.md` |
| Binds to 127.0.0.1 only | Done |
| Rejects foreign `Host` / `Origin` | Done — DNS rebinding and cross-site requests get 403 |
| Per-session token | Done — 64 hex chars from `getentropy`, or `--token`; handed to the GUI in W10 |
| Collection names validated, resolved under `--data` | Done — `[A-Za-z0-9_-]{1,64}` |
| Collections opened once and cached | Done — one read handle per collection, refreshed per request |
| `http.c` removed; D5–D7 fixed | Done |

## 2. What was built

- **Vendored** (plan §5): yyjson 0.12.0 and CivetWeb v1.16, both MIT,
  with intake records. CivetWeb is built with `NO_SSL NO_CGI NO_FILES
  NO_CACHING`. Code added: CivetWeb about 85 KB, yyjson about 217 KB
  (`__TEXT`, before the linker drops unused code). `lisa` is 15.3 MB
  and still links only system libraries and frameworks.
- **Platform:** `lisa_mkdirs`, `lisa_dir_list`, default data directory,
  executable path, secure random bytes, stop-signal handling, sleep.
- **Program layer (`src/app/`):** data directory, versioned
  `config.json`, collection names, model lookup and loading, and the JSON
  shapes shared by the CLI's `--json` and the HTTP API.
- **HTTP (`src/http/`):** the routes above. Ingest jobs run one at a
  time, in order, on a worker thread with their own embedding model, so
  search and ask keep working during ingest. Search and ask are
  serialised (models are not thread-safe). Ask streams as Server-Sent
  Events; closing the connection stops generation. The enterprise
  extension points are wired in: the auth provider runs after the token
  check and its principal reaches the retrieval filter; unknown routes
  are offered to the routes extension.
- **CLI (`src/cli/`):** rewritten. Streaming answers with a sources
  list, `--json` everywhere it matters, stable exit codes (see
  `src/cli/README.md`), Ctrl-C stops ingest and ask safely and frees
  the models.
- **Public API 0.5.0 (additive):** `lisa_collection_migrate_v1`, used
  by `lisa migrate`.

### Decisions made here (defaults; change if wanted)

- Data directory default: `~/Library/Application Support/LISA` on macOS,
  `$XDG_DATA_HOME/lisa` or `~/.local/share/lisa` elsewhere.
- Default port 8765. `--port 0` picks a free port.
- Models: `lisa model --set <file>` records the path in `config.json`
  and detects chat vs embedding; otherwise a known model file is found
  in `<data>/models`, `models/` next to the binary, or one level up.
- The CLI and server use LISA only through `lisa.h`. For OS services
  (directories, signals, randomness) they use `src/platform/`, the layer
  every OS call goes through. `CLAUDE.md` was worded as "only the public
  API"; this reading is recorded here.

## 3. Tests

27 tests, all passing in Release and ASan/UBSan builds.

- `test_http` (new, 7; replaces `test_api.sh`): real sockets through
  CivetWeb's client. Health; token missing, wrong, or malformed; foreign
  Host, look-alike Host, foreign/https/`null` Origin; case-insensitive
  `localhost`; name validation and an encoded `../` path; methods;
  unknown routes; bad JSON; 1 MiB limit; ingest path checks; unknown
  jobs; port in use; generated token; auth provider (deny, principal
  reaching an extension route, release called) and routes extension.
  With models: ingest job queued → succeeded, collection listing,
  search, ask as JSON and as SSE, multi-message ask refused, unknown
  collection.
- `test_cli.sh` (rewritten, 50 checks): version, help, usage errors and
  exit codes, `migrate` of a 0.1 collection, `serve` start/health/token
  and clean stop on SIGINT; with models: `model --set`, `model`,
  `ingest` and unchanged re-run, `search`, `ask` with sources, the
  `--json` forms (PDF page citation), and the not-found answer.
- `test_platform`, `test_public_api`: the new functions.

Under ASan, `test_http` and `test_cli` disable only the
container-overflow check, for the llama.cpp false positive recorded in
report 009.

## 4. Open items

- **GUI** (`lisa gui`) is W10. The server already serves everything it
  needs; W10 adds the embedded page and passes it the token.
- **Job cancel** over HTTP is not in the plan's route list and is not
  built; Ctrl-C of `lisa serve` cancels a running job.
- **CI time:** the release job took 41 minutes for W7; W8 and W9 add
  model tests (`test_ask`, `test_http`, `test_cli`). If it nears the
  90-minute limit, trim model-heavy tests on CI.
- Carried over: retrieval on homogeneous corpora (009), Hindi not-found
  detection (009), `test_store_crash` without `replace_doc` (007).

## 5. Next

W10 — GUI: collections, add folder with progress, ask box with
streamed answer and clickable citations, settings; web UI compiled into
the binary, served by this API, opened in a native window.
