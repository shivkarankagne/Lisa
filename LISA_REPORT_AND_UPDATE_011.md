# LISA — Report and Update 011

**Date:** 2026-09-19
**Type:** Work package report
**Governing documents:** `LISA_COMPLETION_PLAN.md`, `CLAUDE.md`
**Work package:** W10 — GUI
**Commits:** `d26bbac` (vendor webview), then the GUI commit (see `git log`)

---

## 1. Status

W10 is complete, except two native interactions that need a person to
try once (§4).

| Acceptance criterion (plan §6 W10) | Status |
| :--- | :--- |
| Screens: collections; add folder with progress; ask; streamed answer with clickable citations showing the source passage; settings (model, data directory) | Done |
| Add folder by drag and drop | Built (native, macOS window); **not yet tried by a person** |
| Uses only the W9 HTTP API | Done — plus one additive route, `/v1/settings` (API 0.6.0) |
| All assets compiled into the binary; no requests to external hosts | Done — CSP `default-src 'none'` + `connect-src 'self'`; test asserts every request goes to 127.0.0.1 |
| `lisa gui` opens a native window; falls back to the system browser | Done — window verified to open, serve, and close cleanly; the browser path (`--browser`) is built but not run in tests (it opens your browser) |
| Keyboard-only use | Done — every control named, Tab order reaches the question, Enter asks, Esc closes a source, citations are buttons, skip link |
| Readable at 200% zoom | Done — single column, question first, no horizontal scrolling (tested at 640 CSS px) |
| Light and dark themes | Done — follows the system, or a toggle (auto / light / dark) |
| Adds ≤ 5 MB to the binary (recorded) | Done — **+113 KB** (15,309,944 → 15,422,968 bytes: webview + UI) |

## 2. What was built

- **UI (`gui/`):** plain HTML, CSS and JavaScript; no framework, no
  build step, no Node at build time (Preact in plan §5 was optional and
  would have needed a bundler). Document text is only ever inserted as
  text, never as HTML. Answers stream in as they are written; `[n]`
  markers become buttons that open the quoted passage with file and page.
- **Embedding:** `cmake/embed_assets.cmake` turns the files into a C
  table at build time (pure CMake, no extra tools).
- **Serving:** the W9 server serves the files at `/` without the token,
  with a strict Content-Security-Policy and `X-Frame-Options: DENY`.
  `lisa gui` passes the token in the URL fragment (never sent over the
  network); the page keeps it for the tab and removes it from the URL.
- **Native window (`src/gui/`):** webview/webview 0.12.0 (MIT), WKWebView
  on macOS. "Choose…" opens the system folder dialog; folders dropped on
  the window are passed in natively (web pages cannot see dropped
  folders' paths). Ctrl-C in the terminal closes the window. If the
  default port is taken, `lisa gui` uses any free port.
- **Settings route:** `GET/POST /v1/settings` shows the data directory
  and models, and saves a new known model file to `config.json`
  (applies after restart).
- **Platform:** `lisa_open_url`, `lisa_choose_folder`,
  `lisa_file_drops_install` (Objective-C in `src/platform/platform_macos.m`).

### Found and fixed on the way

- An element with its own `display` (the Settings panel) ignored the
  `hidden` attribute, so Settings showed under the Ask view. Caught from
  the headless screenshots; fixed globally.
- Replacing the web view's class with a runtime subclass (for drops)
  made AppKit assert when the window closed. Replaced by swapping only
  the three drag methods; the window now closes cleanly (exit 0).
- The browser's automatic `/favicon.ico` request got a 401; the page
  now declares an inline empty icon.

## 3. Tests

28 tests. New:

- `test_gui` (24 checks, headless Chrome over the DevTools protocol,
  Node's built-in WebSocket, no packages; skipped where Chrome or Node is
  missing; a test tool, not shipped): page load, token handling, view
  switching, accessible names, Tab order, light and dark themes, add a
  folder to a new collection with progress, streamed answer about the
  right fact, citation opens the passage and takes focus, off-topic
  question shows "not found", settings, 200% zoom, no request leaves
  127.0.0.1, no script errors.
- `test_http` (+1): static files (no token, CSP, content types, Host
  still checked), `/v1/settings` get/post and validation.

## 4. Needs a person (one minute)

The automated tests cannot drag from Finder or click a system dialog.
Please run `build/lisa gui` once and:

1. Drag a folder from Finder onto the window: indexing should start
   into the collection named in the form.
2. Click "Choose…": the macOS folder dialog should open and fill in the
   path.

## 5. Open items

- Other platforms: the native window is macOS-only in 1.0 (Linux and
  Windows use `--browser` behaviour until their web view is added).
- No cancel button for a running ingest in the GUI (the API has no
  cancel route yet).
- Carried over: retrieval on homogeneous corpora and Hindi not-found
  detection (009), `test_store_crash` without `replace_doc` (007).

## 6. Next

W11 — release: version scheme, signed and notarised binary, checksums,
GitHub Release, config/log file, quickstart README, "sends nothing
anywhere" statement, CONTRIBUTING and SECURITY.
