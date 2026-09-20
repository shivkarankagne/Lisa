# LISA HTTP API (v1)

`lisa serve` runs this API on `127.0.0.1` (default port 8765). It is for
programs on the same machine, such as the LISA GUI and scripts. Changes
within `/v1` are additive only.

## Security

- Listens on 127.0.0.1 only.
- `Host` must be `127.0.0.1:<port>` or `localhost:<port>`; an `Origin`, if
  sent, must be `http://127.0.0.1:<port>` or `http://localhost:<port>`.
  Anything else gets `403`. This blocks DNS-rebinding and cross-site
  requests from web pages.
- Every route except `GET /v1/health` needs
  `Authorization: Bearer <token>`. `lisa serve` prints a random token at
  start (or uses `--token`). Missing or wrong token: `401`.
- Collection names are `[A-Za-z0-9_-]{1,64}` and resolve under the data
  directory only.
- Request bodies are limited to 1 MiB.
- The GUI's own files (`/`, `/app.js`, `/app.css`) are served without
  the token, with `Content-Security-Policy: default-src 'none';
  script-src 'self'; style-src 'self'; connect-src 'self'; ...` and
  `X-Frame-Options: DENY`: the page can talk only to this server. `lisa
  gui` hands the page the token in the URL fragment (`/#token=...`),
  which browsers never send over the network; the page keeps it for the
  tab and removes it from the address bar.

## Errors

    {"error": {"code": "not_found", "message": "not found"}}

| Status | Codes |
| ---: | :--- |
| 400 | `invalid_json`, `invalid_argument`, `invalid_name`, `unsupported`, `too_long` |
| 401 | `unauthorized` |
| 403 | `forbidden_host`, `forbidden_origin`, `denied` |
| 404 | `not_found` |
| 405 | `method_not_allowed` |
| 409 | `busy`, `model_mismatch` |
| 413 | `too_large` |
| 503 | `no_model` (the server started without the model this needs) |

## Routes

### `GET /v1/health`

    {"status": "ok", "version": "0.6.0", "models": {"chat": true, "embedding": true}}

### `GET /v1/settings`  (since 0.6)

    {"version": "0.6.0", "data_dir": "/Users/me/Library/Application Support/LISA",
     "models": {"chat": {"path": "/.../Qwen3-4B-Q4_K_M.gguf", "loaded": true, "source": "search"},
                "embedding": {"path": "/.../Qwen3-Embedding-0.6B-Q8_0.gguf", "loaded": true, "source": "config"}}}

### `POST /v1/settings`  (since 0.6)

    {"chat_model": "/abs/path.gguf", "embedding_model": "/abs/path.gguf"}

Either or both. Only known model files (`docs/models.md`) of the right
kind, recognised by exact file name and size (`lisa model` verifies the
full SHA-256); others are set with `lisa model --set`. Saved to `config.json`;
applies the next time LISA starts: `{"restart_required": true}`.

### `GET /v1/collections`

    {"collections": [{"name": "plant", "embedding_model": "qwen3-embedding-0.6b-q8_0", "dim": 1024, "chunks": 143}]}

### `POST /v1/collections/{name}/ingest`

    {"paths": ["/Users/me/Documents/manuals"]}

Absolute paths of files or folders. Creates the collection if needed.
Returns `202` with the job; jobs run one at a time, in order, while
search and ask keep working.

    {"job": {"id": "1", "collection": "plant", "state": "queued", "files_seen": 0, ...}}

### `GET /v1/jobs`  (since 0.6)

The last 20 jobs, newest first: `{"jobs": [ {...}, ... ]}`, each the same
object as below.

### `GET /v1/jobs/{id}`

    {"job": {"id": "1", "collection": "plant", "state": "succeeded",
             "files_seen": 3, "files_added": 3, "files_updated": 0, "files_unchanged": 0,
             "files_no_text": 0, "files_failed": 0, "files_removed": 0, "files_skipped": 0,
             "chunks_added": 3, "chunks_removed": 0, "elapsed_seconds": 0.24}}

`state`: `queued`, `running`, `succeeded`, `failed` (with `error`), `cancelled`.

### `POST /v1/collections/{name}/search`

    {"query": "bearing inspection", "topk": 5, "mode": "hybrid"}

`mode`: `hybrid` (default), `vector`, or `keyword`. `topk`: 1-100, default 5.

    {"hits": [{"id": 1, "score": 0.0328, "distance": 0.95, "keyword_score": 1.2,
               "path": "/.../manual.pdf", "page": 1, "offset": 0, "length": 114, "text": "..."}]}

### `POST /v1/collections/{name}/ask`

    {"messages": [{"role": "user", "content": "How often must pump bearings be inspected?"}],
     "stream": false, "topk": 8}

One message in 1.0 (more returns `400 unsupported`; follow-up chat comes
later without changing this shape).

    {"text": "Every 500 hours [S1].", "found": true, "complete": true,
     "citations": [{"number": 1, "chunk_id": 1, "path": "/.../manual.pdf", "title": "LISA Test Manual",
                    "page": 1, "offset": 0, "length": 114, "quote": "...",
                    "content_hash": "xxh3:9fdc7830e0c0eec2", "similarity": 0.67}],
     "passages_retrieved": 2, "passages_used": 2, "prompt_tokens": 182, "answer_tokens": 9,
     "first_token_seconds": 1.59, "total_seconds": 2.02}

`found` is false when the documents do not contain the answer (the text
is then "I could not find this in your documents.").

Citation markers in `text` are `[S<number>]`, matching `number` in
`citations`. The `S` keeps them apart from numbering inside the documents
themselves (a contract clause "15." would otherwise read as `[15]`). A
bare `[<number>]` is still parsed, for models that drop the letter.

With `"stream": true` the response is `text/event-stream`:

    event: token
    data: {"text":"Every"}

    event: token
    data: {"text":" 500 hours [S1]."}

    event: answer
    data: {...the same object as above...}

If the answer fails after streaming started, the last event is
`event: error` with `{"message": "..."}`. Closing the connection stops
generation.
