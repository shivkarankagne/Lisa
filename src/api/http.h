#ifndef LISA_HTTP_H
#define LISA_HTTP_H

/*
 * LISA HTTP API — read-only.
 *
 * Endpoints:
 *
 *   GET /health
 *     -> 200, body "ok\n"
 *
 *   POST /search?collection=<path>&topk=<int>
 *     Headers: Content-Type: application/octet-stream
 *     Body:    dim * float32 LE (the query vector)
 *     -> 200, body text, one line per result, ascending:
 *            <index> <distance>\n
 *     -> 400 malformed request
 *     -> 404 collection not found
 *     -> 500 internal error
 *
 * Any other path -> 404.
 *
 * No TLS. No authentication. No write endpoints.
 *
 * This server is minimal by design. It handles exactly the two
 * endpoints above. It does not implement chunked encoding, keep-alive
 * beyond a single request per connection, or any HTTP/1.1 feature
 * not required for these two endpoints.
 *
 * Server behaviour:
 *   lisa_http_serve(port) blocks and serves connections until
 *   terminated by a signal. It returns a negative code only on
 *   startup failure.
 */

int lisa_http_serve(int port);

#endif /* LISA_HTTP_H */
