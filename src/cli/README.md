# lisa — command line

    lisa ingest  [--data <dir>] --collection <name> <path>...
    lisa search  [--data <dir>] --collection <name> --query "<text>" [--topk N]
    lisa ask     [--data <dir>] --collection <name> [--topk N] "<question>"
    lisa serve   [--data <dir>] [--port <port>] [--token <token>]
    lisa gui     [--data <dir>] [--port <port>] [--browser]
    lisa model   [--data <dir>] [--set <file.gguf>]
    lisa migrate --from <v1-dir> --to <dir> [--model <id>]
    lisa --version | --help

`--json` on `ingest`, `search`, `ask`, and `model` prints JSON instead of
text (the same shapes as the HTTP API, see `docs/http-api.md`).

## Data directory

`--data` defaults to `~/Library/Application Support/LISA` on macOS
(`$XDG_DATA_HOME/lisa` or `~/.local/share/lisa` elsewhere). It is created
on first use:

    <data>/config.json          models chosen with `lisa model --set`
    <data>/collections/<name>/  one collection per name ([A-Za-z0-9_-], 1-64 chars)
    <data>/models/              model files placed here are found automatically

## Models

`lisa ask` needs a chat model and an embedding model; `ingest` and
`search` need the embedding model. They are found in this order:

1. the paths recorded in `config.json` (`lisa model --set <file>`; the
   file is identified as chat or embedding automatically);
2. a known model file (`docs/models.md`) in `<data>/models`, in
   `models/` next to the `lisa` binary, or in `models/` one level up.

`lisa model` shows which files are used and verifies them (SHA-256
against the known models).

## Commands

- **ingest** indexes files and folders (txt, md, pdf) into a collection,
  creating it if needed. Unchanged files are skipped; edited files are
  replaced; files deleted from an ingested folder are removed. Ctrl-C
  stops safely after the current document.
- **search** prints the best matching passages (hybrid keyword + meaning).
- **ask** streams an answer from the collection with numbered citations,
  then lists the sources. If the documents do not contain the answer it
  says "I could not find this in your documents."
- **gui** opens LISA in its own window (macOS; `--browser` or other
  systems: the default browser). Add folders (type a path, "Choose…",
  or drop a folder on the window), ask, click a citation to read the
  passage, change models in Settings. Close the window to quit.
- **serve** runs the local HTTP API (`docs/http-api.md`) on 127.0.0.1
  and prints a session token. Ctrl-C stops it.
- **migrate** converts a LISA 0.1 collection (`header.bin` +
  `vectors.bin`) to the current format; the result supports vector
  search only (0.1 stored no text).

## Exit codes

| Code | Meaning |
| ---: | :--- |
| 0 | success |
| 1 | bad command line |
| 2 | collection, file, or model not found |
| 3 | wrong or mismatched model |
| 4 | collection in use by another writer |
| 5 | any other error |
| 130 | interrupted with Ctrl-C |
