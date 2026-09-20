# Security

## Reporting a vulnerability

Please report security problems privately, not in a public issue:

- GitHub: **Security → Report a vulnerability** on
  <https://github.com/shivkarankagne/Lisa/security/advisories/new>

Please include what you did, what happened, and the version
(`lisa --version`). We aim to reply within seven days and to fix
confirmed problems in the next release. Please give us a chance to fix
an issue before describing it publicly.

## What LISA does with your data

- **Nothing leaves your computer.** LISA makes no network connections
  except its own server on `127.0.0.1`. There is no telemetry, no update
  check, and no account. (Downloading a model file is something you do
  yourself, with `curl` or a browser.)
- **Your files are not modified or copied.** A collection holds extracted
  text, vectors and file paths, under your data directory.
- **The log** (`<data>/lisa.log`) records what LISA did — never document
  text, questions or answers — so it is safe to attach to a bug report.

## The local server

`lisa serve` and `lisa gui` listen on `127.0.0.1` only, never on a
network interface. Requests must pass three checks:

- a `Host` of `127.0.0.1:<port>` or `localhost:<port>`, and an `Origin`,
  if present, from the same address (this blocks DNS rebinding and
  requests from web pages you visit);
- `Authorization: Bearer <token>`, where the token is 32 random bytes
  generated at start (`GET /v1/health` is the only exception);
- collection names limited to `[A-Za-z0-9_-]`, resolved under the data
  directory only.

The GUI is served with a Content-Security-Policy that allows it to talk
to nothing but this server, and it receives the token in the URL
fragment, which browsers never send over the network.

Anyone who can run programs as you can read your data directory and
connect to the server; LISA does not defend against that.

## Documents are untrusted input

LISA treats every document as untrusted: text from a file can never
become chat-template markup, and answers quote documents as text, never
as HTML. Extraction runs in the same process as the rest of LISA, so a
file that exploits a parser (PDFium, SQLite, miniz, md4c) would be
serious; those components are vendored at pinned versions and updated
when they publish fixes.

## Supported versions

Pre-1.0: only the latest release gets fixes.
