# LISA — Report and Update 013

**Date:** 2026-09-20
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W11 — Release (everything except code signing)

---

## 1. Status

W11 is complete except signing and notarisation, which need an Apple
Developer ID the user will buy later.

| Acceptance criterion (plan §6 W11) | Status |
| :--- | :--- |
| Semantic version in binary, API, and file formats | Done — `lisa --version` prints the binary version, the API version, and the collection and vector file format versions; a test keeps the published numbers equal to what storage writes |
| macOS binary signed and notarised | **Open** — needs a Developer ID ($99/year). Until then: right-click → Open on first run (stated in the README and the release notes) |
| SHA-256 checksums published with each release | Done — `scripts/release.sh` writes `<archive>.sha256` and publishes both |
| Config file with a version field | Done since W9 — `<data>/config.json`, `{"version": 1, ...}`; a newer version is refused |
| Log file with levels | Done — `<data>/lisa.log`, levels error/warn/info/debug, `--log-level` or `LISA_LOG_LEVEL`; errors and warnings also go to the terminal; the file rolls at 4 MB. It records what LISA did, never document text, questions or answers |
| README quickstart: download, model, ingest, ask in under five minutes | Done — rewritten (the old one described the 0.1 engine) |
| No telemetry; no network except the local server | Done and stated in the README, `SECURITY.md`, and `lisa --version` |
| `CONTRIBUTING.md`, `SECURITY.md` | Done |

## 2. What was built

- **`scripts/release.sh`**: builds Release, prints the version, fails if
  `otool -L` shows any non-system library, runs the full test suite,
  packages `lisa` plus README, LICENSE, NOTICE, SECURITY and the docs
  into `lisa-<version>-macos-<arch>.tar.gz`, writes the SHA-256 file,
  and with `--publish` creates the GitHub release.
- **`src/app/log.{h,c}`**: the log described above, used by the CLI
  (ingest results, serve start/stop) and the server (ingest job
  results).
- **`lisa --version`** now also prints the API and on-disk format
  versions and the no-telemetry line; `--log-level` works on every
  command.
- **`include/lisa.h`**: `LISA_COLLECTION_FORMAT_VERSION`,
  `LISA_VECTOR_FILE_VERSION` (additive), checked against
  `src/storage/store.h` by `test_public_api`.
- **README, SECURITY.md, CONTRIBUTING.md** written for people outside
  the project: what LISA is, the five-minute quickstart with the pinned
  model revisions, where data lives, how to report a vulnerability, what
  the local server checks, and that code contributions wait for a CLA.

## 3. Measurements

Release build, M2:

- `lisa` 15,510,008 bytes (15.5 MB); release archive 6.2 MB.
- `otool -L` lists only `/usr/lib/*` and `/System/Library/*`.
- Full suite green in Release and ASan/UBSan (28 tests).

## 4. Also in this period

- **The repository is public** (the user's decision, 2026-09-20), which
  also makes CI free: private-repo macOS minutes had run out and jobs
  were refused by GitHub ("payments have failed or your spending limit
  needs to be increased"). The licence is still the unreviewed BUSL
  draft; the README says so.
- **CI fix:** `test_http` used CivetWeb's `mg_download`, which waits
  only its default time; on a CI Mac (models on the CPU) an ask took
  longer and the test reported a connection failure. It now connects and
  waits explicitly (10 minutes). The release job's limit is 300 minutes,
  because CI Macs are much slower than the reference machine.

## 5. Open items

- Signing and notarisation (needs the Developer ID), then the release
  script gains a `codesign` + `notarytool` step.
- The release has not been published yet: `scripts/release.sh --publish`
  is ready when the user wants a v0.6.0 pre-release on GitHub.
- The polish list from report 012 is unchanged.

## 6. Next

W0 (competitor baseline: AnythingLLM and Open WebUI + Ollama, same
documents and questions) and W12 (evaluation set, clean-machine test,
offline test, crash test, 8-hour soak).
