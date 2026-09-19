/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_HTTP_SERVER_H
#define LISA_HTTP_SERVER_H
/*
 * LISA's local HTTP API (JSON, under /v1), on CivetWeb.
 *
 *   GET  /v1/health                              no token needed
 *   GET  /v1/collections
 *   POST /v1/collections/{name}/ingest           {"paths": ["/abs/dir", ...]}      -> 202 {"job": "<id>"}
 *   GET  /v1/jobs/{id}
 *   POST /v1/collections/{name}/search           {"query": "...", "topk": 5, "mode": "hybrid"}
 *   POST /v1/collections/{name}/ask              {"messages": [{"role": "user", "content": "..."}],
 *                                                 "stream": false, "topk": 8}
 *
 * With "stream": true, ask answers as Server-Sent Events: `token` events
 * ({"text": "..."}) as the answer is generated, then one `answer` event
 * with the same JSON a non-streamed ask returns.
 *
 * Errors: {"error": {"code": "...", "message": "..."}} with a 4xx/5xx status.
 *
 * Local security (plan §6 W9):
 * - listens on 127.0.0.1 only;
 * - rejects a Host that is not 127.0.0.1:<port> or localhost:<port>, and
 *   an Origin that is not http://127.0.0.1:<port> or http://localhost:<port>
 *   (DNS rebinding, cross-site requests from web pages);
 * - every route but /v1/health needs "Authorization: Bearer <token>",
 *   a random per-session token;
 * - collection names are [A-Za-z0-9_-]{1,64}, resolved under the data dir.
 *
 * Search and ask are served one at a time (models are not thread-safe);
 * ingest jobs run one at a time, in order, on a worker thread with their
 * own embedding model, so questions are answered while ingest runs.
 */

#include <stdint.h>

#include "../app/app.h"

#define SERVER_DEFAULT_PORT 8765
#define SERVER_TOKEN_LEN    64   /* hex characters */

typedef struct lisa_server lisa_server_t;

typedef struct {
    app_t*        app;          /* borrowed; outlives the server */
    int           port;         /* 0: any free port */
    lisa_model_t* chat;         /* borrowed; may be NULL (ask returns 503) */
    lisa_model_t* embed;        /* borrowed; may be NULL (search/ask return 503) */
    const char*   token;        /* NULL: generate one */
} server_options_t;

/* Start serving. *err (static text) explains a failure. */
int  server_start(const server_options_t* opts, lisa_server_t** out, const char** err);

int         server_port(const lisa_server_t* s);
const char* server_token(const lisa_server_t* s);

/* Stop accepting requests, finish or cancel the running job, and free. */
void server_stop(lisa_server_t* s);

#endif /* LISA_HTTP_SERVER_H */
