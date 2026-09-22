/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * test_http — the local HTTP API (src/http/server.h) over real sockets,
 * using CivetWeb's client. Model-free tests run a server without models;
 * the rest need both default models in <models_dir> (ignored without).
 *
 * Usage: test_http <scratch_dir> <fixtures_dir> <models_dir>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/platform/platform.h"

#include "unity.h"
#include "civetweb.h"
#include "yyjson.h"
#include "lisa.h"
#include "../src/app/app.h"
#include "../src/http/server.h"

#define TOKEN "test-token-0123456789abcdef"

static const char* g_scratch;
static const char* g_fixtures;
static const char* g_models;
static int g_have_models;
static app_t g_app;
static lisa_server_t* g_srv;       /* no models */
static int g_port;
static char g_body[1 << 16];
static char g_csp[512];      /* Content-Security-Policy of the last response */
static char g_type[128];     /* Content-Type of the last response */

void setUp(void) {}
void tearDown(void) {}

/*
 * Send one request; returns the HTTP status and leaves the body in
 * g_body. extra: additional header lines ("Name: value\r\n"), or NULL.
 * auth: 1 to send the right token.
 */
/*
 * When set, the body is read until this text appears rather than until
 * the connection closes. A streamed answer arrives over minutes on a
 * slow machine, and mg_read gives up after CivetWeb's 30 s client
 * timeout with no way to tell that apart from the end of the body.
 */
static const char* g_read_until = NULL;

static int http_at(int port, const char* method, const char* path, const char* extra, int auth,
                   const char* body) {
    char ebuf[256];
    char host[64];
    snprintf(host, sizeof(host), "Host: 127.0.0.1:%d\r\n", port);
    int has_host = extra && strstr(extra, "Host:") != NULL;
    /*
     * mg_download would wait only its default time for the response;
     * answering a question can take minutes on a slow machine (a CI Mac
     * generates on the CPU), so connect and wait explicitly.
     */
    struct mg_connection* c = mg_connect_client("127.0.0.1", port, 0, ebuf, sizeof(ebuf));
    if (c == NULL) {
        snprintf(g_body, sizeof(g_body), "connect failed: %s", ebuf);
        return -1;
    }
    mg_printf(c, "%s %s HTTP/1.1\r\n%s%s%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
              method, path, has_host ? "" : host, extra ? extra : "",
              auth ? "Authorization: Bearer " TOKEN "\r\n" : "", body ? strlen(body) : (size_t)0);
    if (body && body[0]) mg_write(c, body, strlen(body));
    if (mg_get_response(c, ebuf, sizeof(ebuf), 600000) < 0) {   /* 10 minutes */
        snprintf(g_body, sizeof(g_body), "no response: %s", ebuf);
        mg_close_connection(c);
        return -1;
    }
    const struct mg_response_info* ri = mg_get_response_info(c);
    int status = ri->status_code;
    g_csp[0] = g_type[0] = '\0';
    for (int i = 0; i < ri->num_headers; i++) {
        if (strcmp(ri->http_headers[i].name, "Content-Security-Policy") == 0)
            snprintf(g_csp, sizeof(g_csp), "%s", ri->http_headers[i].value);
        if (strcmp(ri->http_headers[i].name, "Content-Type") == 0)
            snprintf(g_type, sizeof(g_type), "%s", ri->http_headers[i].value);
    }
    size_t len = 0;
    time_t deadline = time(NULL) + 600;
    while (len + 1 < sizeof(g_body)) {
        int n = mg_read(c, g_body + len, sizeof(g_body) - 1 - len);
        if (n > 0) {
            len += (size_t)n;
            continue;
        }
        g_body[len] = '\0';
        if (g_read_until == NULL || strstr(g_body, g_read_until) != NULL) break;
        if (time(NULL) >= deadline) break;
        lisa_sleep_ms(100);   /* the model is still generating */
    }
    g_body[len] = '\0';
    mg_close_connection(c);
    return status;
}

static int http(const char* method, const char* path, const char* extra, int auth, const char* body) {
    return http_at(g_port, method, path, extra, auth, body);
}

/* As http(), but reads the body until `until` appears. */
static int http_stream(const char* method, const char* path, const char* body, const char* until) {
    g_read_until = until;
    int rc = http_at(g_port, method, path, NULL, 1, body);
    g_read_until = NULL;
    return rc;
}

/* Value of a top-level (or "a.b") string field in g_body, or "". */
static const char* field(const char* key, char* out, size_t cap) {
    out[0] = '\0';
    yyjson_doc* d = yyjson_read(g_body, strlen(g_body), 0);
    yyjson_val* v = yyjson_doc_get_root(d);
    char k[128];
    snprintf(k, sizeof(k), "%s", key);
    for (char* part = strtok(k, "."); part && v; part = strtok(NULL, ".")) v = yyjson_obj_get(v, part);
    if (yyjson_is_str(v)) snprintf(out, cap, "%s", yyjson_get_str(v));
    else if (yyjson_is_bool(v)) snprintf(out, cap, "%s", yyjson_get_bool(v) ? "true" : "false");
    else if (yyjson_is_int(v)) snprintf(out, cap, "%lld", (long long)yyjson_get_sint(v));
    yyjson_doc_free(d);
    return out;
}

static const char* err_code(void) {
    static char b[64];
    return field("error.code", b, sizeof(b));
}

/* ---- model-free -------------------------------------------------------- */

static void test_health_and_token(void) {
    char b[64];
    TEST_ASSERT_EQUAL_INT(200, http("GET", "/v1/health", NULL, 0, NULL));
    TEST_ASSERT_EQUAL_STRING("ok", field("status", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING(LISA_VERSION_STRING, field("version", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("false", field("models.chat", b, sizeof(b)));

    TEST_ASSERT_EQUAL_INT(401, http("GET", "/v1/collections", NULL, 0, NULL));
    TEST_ASSERT_EQUAL_STRING("unauthorized", err_code());
    TEST_ASSERT_EQUAL_INT(401, http("GET", "/v1/collections", "Authorization: Bearer wrong\r\n", 0, NULL));
    TEST_ASSERT_EQUAL_INT(401, http("GET", "/v1/collections", "Authorization: " TOKEN "\r\n", 0, NULL));
    TEST_ASSERT_EQUAL_INT(200, http("GET", "/v1/collections", NULL, 1, NULL));
    TEST_ASSERT_EQUAL_STRING("{\"collections\":[]}", g_body);
}

static void test_host_and_origin(void) {
    char h[128];
    TEST_ASSERT_EQUAL_INT(403, http("GET", "/v1/health", "Host: evil.example:80\r\n", 1, NULL));
    TEST_ASSERT_EQUAL_STRING("forbidden_host", err_code());
    snprintf(h, sizeof(h), "Host: 127.0.0.1:%d.evil.example\r\n", g_port);
    TEST_ASSERT_EQUAL_INT(403, http("GET", "/v1/health", h, 1, NULL));
    snprintf(h, sizeof(h), "Host: LOCALHOST:%d\r\n", g_port);
    TEST_ASSERT_EQUAL_INT(200, http("GET", "/v1/collections", h, 1, NULL));

    TEST_ASSERT_EQUAL_INT(403, http("GET", "/v1/collections", "Origin: http://evil.example\r\n", 1, NULL));
    TEST_ASSERT_EQUAL_STRING("forbidden_origin", err_code());
    snprintf(h, sizeof(h), "Origin: https://127.0.0.1:%d\r\n", g_port);
    TEST_ASSERT_EQUAL_INT(403, http("GET", "/v1/collections", h, 1, NULL));
    TEST_ASSERT_EQUAL_INT(403, http("GET", "/v1/collections", "Origin: null\r\n", 1, NULL));
    snprintf(h, sizeof(h), "Origin: http://localhost:%d\r\n", g_port);
    TEST_ASSERT_EQUAL_INT(200, http("GET", "/v1/collections", h, 1, NULL));
}

static void test_routes_and_validation(void) {
    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/a.b/search", NULL, 1, "{\"query\":\"x\"}"));
    TEST_ASSERT_EQUAL_STRING("invalid_name", err_code());
    int st = http("POST", "/v1/collections/..%2F..%2Fetc/search", NULL, 1, "{\"query\":\"x\"}");
    TEST_ASSERT_TRUE(st == 400 || st == 404);   /* never resolved outside the data dir */
    TEST_ASSERT_EQUAL_INT(405, http("GET", "/v1/collections/docs/search", NULL, 1, NULL));
    TEST_ASSERT_EQUAL_INT(405, http("POST", "/v1/collections", NULL, 1, "{}"));
    TEST_ASSERT_EQUAL_INT(404, http("GET", "/v1/nothing", NULL, 1, NULL));
    TEST_ASSERT_EQUAL_INT(404, http("POST", "/v1/collections/docs/delete", NULL, 1, "{}"));
    TEST_ASSERT_EQUAL_INT(404, http("GET", "/", NULL, 1, NULL));

    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/docs/search", NULL, 1, "not json"));
    TEST_ASSERT_EQUAL_STRING("invalid_json", err_code());
    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/docs/search", NULL, 1, "[1,2]"));
    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/docs/search", NULL, 1, "{\"query\":\"\"}"));
    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/docs/search", NULL, 1, "{\"query\":\"x\",\"topk\":0}"));
    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/docs/search", NULL, 1, "{\"query\":\"x\",\"mode\":\"magic\"}"));
    /* Valid request, but this server has no models. */
    TEST_ASSERT_EQUAL_INT(503, http("POST", "/v1/collections/docs/search", NULL, 1, "{\"query\":\"x\"}"));
    TEST_ASSERT_EQUAL_INT(503, http("POST", "/v1/collections/docs/ask", NULL, 1,
                                    "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"));
    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/docs/ask", NULL, 1, "{\"messages\":[]}"));
    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/docs/ask", NULL, 1, "{\"messages\":[{\"role\":\"user\"}]}"));

    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/docs/ingest", NULL, 1, "{\"paths\":[]}"));
    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/docs/ingest", NULL, 1, "{\"paths\":[\"relative/dir\"]}"));
    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/docs/ingest", NULL, 1, "{\"paths\":[\"/no/such/dir/x\"]}"));
    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/docs/ingest", NULL, 1, "{\"paths\":[7]}"));
    TEST_ASSERT_EQUAL_INT(404, http("GET", "/v1/jobs/999", NULL, 1, NULL));
    TEST_ASSERT_EQUAL_INT(404, http("GET", "/v1/jobs/abc", NULL, 1, NULL));
}

static void test_body_limit(void) {
    size_t n = (1 << 20) + 100;
    char* big = (char*)malloc(n + 1);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, ' ', n);
    big[0] = '{';
    big[n - 1] = '}';
    big[n] = '\0';
    TEST_ASSERT_EQUAL_INT(413, http("POST", "/v1/collections/docs/search", NULL, 1, big));
    free(big);
}

static void test_start_errors_and_generated_token(void) {
    lisa_server_t* s = NULL;
    const char* err = NULL;
    server_options_t o = { &g_app, g_port, NULL, NULL, TOKEN, NULL, 0 };
    TEST_ASSERT_NOT_EQUAL(LISA_OK, server_start(&o, &s, &err));   /* port in use */
    TEST_ASSERT_NULL(s);
    o.port = 0;
    o.token = "short";
    TEST_ASSERT_EQUAL_INT(LISA_E_INVALID_ARGUMENT, server_start(&o, &s, &err));
    o.token = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, server_start(&o, &s, &err));
    const char* t = server_token(s);
    TEST_ASSERT_EQUAL_size_t(SERVER_TOKEN_LEN, strlen(t));
    for (const char* p = t; *p; p++) TEST_ASSERT_TRUE((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'));
    TEST_ASSERT_TRUE(server_port(s) > 0 && server_port(s) != g_port);
    server_stop(s);
    server_stop(NULL);
}

/* Extension points: an auth provider and extra routes, as enterprise would add. */
static int g_auth_calls, g_released;

static int deny_bad_agent(void* user, const lisa_auth_request_t* r, lisa_principal_t* out) {
    (void)user;
    g_auth_calls++;
    for (int64_t i = 0; i < r->header_count; i++) {
        if (strcmp(r->header_names[i], "X-User") == 0 && strcmp(r->header_values[i], "mallory") == 0)
            return LISA_E_DENIED;
    }
    out->user_id = "alice";
    return LISA_OK;
}

static void release_principal(void* user, lisa_principal_t* p) {
    (void)user;
    (void)p;
    g_released++;
}

static lisa_context_t* g_ext_ctx;

static int extra_route(void* user, const lisa_http_request_t* r, lisa_http_response_t* resp) {
    (void)user;
    if (strcmp(r->path, "/v1/admin/whoami") != 0) return LISA_E_NOT_FOUND;
    const char* who = r->principal && r->principal->user_id ? r->principal->user_id : "nobody";
    resp->status_code = 200;
    resp->content_type = "text/plain";
    resp->body_len = (int64_t)strlen(who);
    resp->body = (char*)malloc((size_t)resp->body_len);   /* default context allocator is malloc */
    memcpy(resp->body, who, (size_t)resp->body_len);
    return LISA_OK;
}

static void test_extension_points(void) {
    lisa_auth_provider_t ap = { sizeof(ap), NULL, deny_bad_agent, release_principal };
    lisa_http_routes_t routes = { sizeof(routes), NULL, extra_route };
    lisa_context_config_t cfg = LISA_CONTEXT_CONFIG_INIT;
    cfg.auth = &ap;
    cfg.http_routes = &routes;
    TEST_ASSERT_EQUAL_INT(LISA_OK, lisa_context_create(&cfg, &g_ext_ctx));
    lisa_context_t* saved = g_app.ctx;
    g_app.ctx = g_ext_ctx;

    lisa_server_t* s = NULL;
    server_options_t o = { &g_app, 0, NULL, NULL, TOKEN, NULL, 0 };
    TEST_ASSERT_EQUAL_INT(LISA_OK, server_start(&o, &s, NULL));
    int port = server_port(s);
    char h[64];
    snprintf(h, sizeof(h), "Host: 127.0.0.1:%d\r\n", port);

    TEST_ASSERT_EQUAL_INT(200, http_at(port, "GET", "/v1/admin/whoami", h, 1, NULL));
    TEST_ASSERT_EQUAL_STRING("alice", g_body);
    TEST_ASSERT_EQUAL_INT(404, http_at(port, "GET", "/v1/admin/other", h, 1, NULL));
    char h2[128];
    snprintf(h2, sizeof(h2), "%sX-User: mallory\r\n", h);
    TEST_ASSERT_EQUAL_INT(403, http_at(port, "GET", "/v1/admin/whoami", h2, 1, NULL));
    TEST_ASSERT_EQUAL_INT(401, http_at(port, "GET", "/v1/admin/whoami", h, 0, NULL));   /* token first */
    TEST_ASSERT_EQUAL_INT(3, g_auth_calls);
    TEST_ASSERT_EQUAL_INT(2, g_released);

    server_stop(s);
    g_app.ctx = saved;
    lisa_context_destroy(g_ext_ctx);
}

static const unsigned char k_index[] = "<!doctype html><title>t</title>";
static const unsigned char k_js[] = "console.log(1);";
static const server_asset_t k_assets[] = {
    { "/index.html", "text/html; charset=utf-8", k_index, sizeof(k_index) - 1 },
    { "/app.js", "text/javascript; charset=utf-8", k_js, sizeof(k_js) - 1 },
};

static void test_static_files_and_settings(void) {
    lisa_server_t* s = NULL;
    server_options_t o = { &g_app, 0, NULL, NULL, TOKEN, k_assets, 2 };
    TEST_ASSERT_EQUAL_INT(LISA_OK, server_start(&o, &s, NULL));
    int port = server_port(s);
    char h[64];
    snprintf(h, sizeof(h), "Host: 127.0.0.1:%d\r\n", port);

    /* Static files need no token, carry a strict CSP, and "/" is index.html. */
    TEST_ASSERT_EQUAL_INT(200, http_at(port, "GET", "/", h, 0, NULL));
    TEST_ASSERT_EQUAL_STRING("<!doctype html><title>t</title>", g_body);
    TEST_ASSERT_EQUAL_STRING("text/html; charset=utf-8", g_type);
    TEST_ASSERT_NOT_NULL(strstr(g_csp, "default-src 'none'"));
    TEST_ASSERT_NOT_NULL(strstr(g_csp, "connect-src 'self'"));
    TEST_ASSERT_NOT_NULL(strstr(g_csp, "frame-ancestors 'none'"));
    TEST_ASSERT_EQUAL_INT(200, http_at(port, "GET", "/app.js", h, 0, NULL));
    TEST_ASSERT_EQUAL_STRING("text/javascript; charset=utf-8", g_type);
    /* Not a file: the API rules apply (token first). */
    TEST_ASSERT_EQUAL_INT(401, http_at(port, "GET", "/favicon.ico", h, 0, NULL));
    TEST_ASSERT_EQUAL_INT(401, http_at(port, "POST", "/", h, 0, "{}"));
    /* Host and Origin are still checked for static files. */
    TEST_ASSERT_EQUAL_INT(403, http_at(port, "GET", "/", "Host: evil.example\r\n", 0, NULL));

    /* Settings. */
    char b[1024];
    TEST_ASSERT_EQUAL_INT(401, http_at(port, "GET", "/v1/settings", h, 0, NULL));
    TEST_ASSERT_EQUAL_INT(200, http_at(port, "GET", "/v1/settings", h, 1, NULL));
    TEST_ASSERT_EQUAL_STRING(g_app.data_dir, field("data_dir", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING(LISA_VERSION_STRING, field("version", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("false", field("models.chat.loaded", b, sizeof(b)));
    TEST_ASSERT_EQUAL_INT(400, http_at(port, "POST", "/v1/settings", h, 1, "{}"));
    TEST_ASSERT_EQUAL_INT(400, http_at(port, "POST", "/v1/settings", h, 1, "{\"chat_model\":\"relative.gguf\"}"));
    TEST_ASSERT_EQUAL_INT(400, http_at(port, "POST", "/v1/settings", h, 1, "{\"chat_model\":\"/no/such.gguf\"}"));
    TEST_ASSERT_EQUAL_INT(405, http_at(port, "DELETE", "/v1/settings", h, 1, NULL));
    /* Watched folders: set, read back, and rejected when wrong. */
    char watch[1200];
    snprintf(watch, sizeof(watch), "{\"watch\":{\"collection\":\"watched\",\"folders\":[\"%s\"]}}", g_scratch);
    TEST_ASSERT_EQUAL_INT(200, http_at(port, "POST", "/v1/settings", h, 1, watch));
    TEST_ASSERT_EQUAL_STRING("false", field("restart_required", b, sizeof(b)));   /* applies at once */
    TEST_ASSERT_EQUAL_STRING("true", field("watching", b, sizeof(b)));
    TEST_ASSERT_EQUAL_INT(200, http_at(port, "GET", "/v1/settings", h, 1, NULL));
    TEST_ASSERT_EQUAL_STRING("watched", field("watch.collection", b, sizeof(b)));
    TEST_ASSERT_NOT_NULL(strstr(g_body, g_scratch));
    TEST_ASSERT_NOT_NULL(strstr(g_body, "\"suggested\""));
    TEST_ASSERT_EQUAL_INT(400, http_at(port, "POST", "/v1/settings", h, 1,
                                       "{\"watch\":{\"collection\":\"watched\",\"folders\":[\"relative\"]}}"));
    TEST_ASSERT_EQUAL_INT(400, http_at(port, "POST", "/v1/settings", h, 1,
                                       "{\"watch\":{\"collection\":\"bad name\",\"folders\":[\"/tmp\"]}}"));
    TEST_ASSERT_EQUAL_INT(400, http_at(port, "POST", "/v1/settings", h, 1,
                                       "{\"watch\":{\"collection\":\"watched\",\"folders\":[\"/no/such/folder\"]}}"));
    /* Stop watching. */
    TEST_ASSERT_EQUAL_INT(200, http_at(port, "POST", "/v1/settings", h, 1,
                                       "{\"watch\":{\"collection\":null,\"folders\":[]}}"));

    /* The job list. */
    TEST_ASSERT_EQUAL_INT(200, http_at(port, "GET", "/v1/jobs", h, 1, NULL));
    TEST_ASSERT_NOT_NULL(strstr(g_body, "\"jobs\""));
    TEST_ASSERT_EQUAL_INT(405, http_at(port, "POST", "/v1/jobs", h, 1, "{}"));

    if (g_have_models) {
        char body[1200];
        /* The embedding model is not a chat model. */
        snprintf(body, sizeof(body), "{\"chat_model\":\"%s/Qwen3-Embedding-0.6B-Q8_0.gguf\"}", g_models);
        TEST_ASSERT_EQUAL_INT(400, http_at(port, "POST", "/v1/settings", h, 1, body));
        snprintf(body, sizeof(body), "{\"chat_model\":\"%s/Qwen3-4B-Q4_K_M.gguf\"}", g_models);
        TEST_ASSERT_EQUAL_INT(200, http_at(port, "POST", "/v1/settings", h, 1, body));
        TEST_ASSERT_EQUAL_STRING("true", field("restart_required", b, sizeof(b)));
    }
    server_stop(s);
}

/* ---- with models ------------------------------------------------------- */

#define NEED_MODELS() do { if (!g_have_models) TEST_IGNORE_MESSAGE("models not present"); } while (0)

static void test_ingest_search_ask(void) {
    NEED_MODELS();
    lisa_model_t *embed = NULL, *chat = NULL;
    TEST_ASSERT_EQUAL_INT(LISA_OK, app_load_model(&g_app, APP_MODEL_EMBEDDING, &embed, NULL));
    TEST_ASSERT_EQUAL_INT(LISA_OK, app_load_model(&g_app, APP_MODEL_CHAT, &chat, NULL));
    lisa_server_t* s = NULL;
    server_options_t o = { &g_app, 0, chat, embed, TOKEN, NULL, 0 };
    TEST_ASSERT_EQUAL_INT(LISA_OK, server_start(&o, &s, NULL));
    int saved_port = g_port;
    g_port = server_port(s);

    char docs[900], body[1200], b[256];
    snprintf(docs, sizeof(docs), "%s/http_docs_%d", g_scratch, (int)getpid());
    mkdir(docs, 0755);
    char p[1000];
    snprintf(p, sizeof(p), "%s/pump.txt", docs);
    FILE* f = fopen(p, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs("Pump P-7 failed on Tuesday: the main bearing seized after weeks of rising vibration.", f);
    fclose(f);
    snprintf(body, sizeof(body), "{\"paths\":[\"%s\",\"%s/two_pages.pdf\"]}", docs, g_fixtures);

    TEST_ASSERT_EQUAL_INT(202, http("POST", "/v1/collections/plant/ingest", NULL, 1, body));
    char id[32], path[64];
    field("job.id", id, sizeof(id));
    TEST_ASSERT_TRUE(id[0] != '\0');
    snprintf(path, sizeof(path), "/v1/jobs/%s", id);
    int done = 0;
    for (int i = 0; i < 3000 && !done; i++) {
        TEST_ASSERT_EQUAL_INT(200, http("GET", path, NULL, 1, NULL));
        field("job.state", b, sizeof(b));
        done = strcmp(b, "succeeded") == 0 || strcmp(b, "failed") == 0;
        if (!done) lisa_sleep_ms(200);
    }
    TEST_ASSERT_EQUAL_STRING("succeeded", b);
    TEST_ASSERT_EQUAL_STRING("2", field("job.files_added", b, sizeof(b)));

    TEST_ASSERT_EQUAL_INT(200, http("GET", "/v1/collections", NULL, 1, NULL));
    TEST_ASSERT_NOT_NULL(strstr(g_body, "\"name\":\"plant\""));
    TEST_ASSERT_NOT_NULL(strstr(g_body, "\"chunks\":2"));

    TEST_ASSERT_EQUAL_INT(200, http("POST", "/v1/collections/plant/search", NULL, 1,
                                    "{\"query\":\"How often are bearings inspected?\",\"topk\":1}"));
    TEST_ASSERT_NOT_NULL(strstr(g_body, "two_pages.pdf"));

    TEST_ASSERT_EQUAL_INT(200, http("POST", "/v1/collections/plant/ask", NULL, 1,
                                    "{\"messages\":[{\"role\":\"user\",\"content\":\"Why did pump P-7 fail?\"}]}"));
    TEST_ASSERT_EQUAL_STRING("true", field("found", b, sizeof(b)));
    TEST_ASSERT_NOT_NULL(strstr(g_body, "\"citations\":[{"));
    TEST_ASSERT_NOT_NULL(strstr(g_body, "pump.txt"));

    TEST_ASSERT_EQUAL_INT(200, http_stream("POST", "/v1/collections/plant/ask",
                                           "{\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"Why did pump P-7 fail?\"}]}",
                                           "event: answer\ndata: {\"text\":"));
    TEST_ASSERT_NOT_NULL(strstr(g_body, "event: token\ndata: {\"text\":"));
    TEST_ASSERT_NOT_NULL(strstr(g_body, "event: answer\ndata: {\"text\":"));

    TEST_ASSERT_EQUAL_INT(400, http("POST", "/v1/collections/plant/ask", NULL, 1,
                                    "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"},{\"role\":\"user\",\"content\":\"b\"}]}"));
    TEST_ASSERT_EQUAL_STRING("unsupported", err_code());
    TEST_ASSERT_EQUAL_INT(404, http("POST", "/v1/collections/nothing/ask", NULL, 1,
                                    "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}]}"));

    server_stop(s);
    g_port = saved_port;
    lisa_model_free(chat);
    lisa_model_free(embed);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <scratch_dir> <fixtures_dir> <models_dir>\n", argv[0]);
        return 2;
    }
    mkdir(argv[1], 0755);
    /* The API takes absolute paths only. */
    static char scratch[1024], fixtures[1024];
    if (!realpath(argv[1], scratch) || !realpath(argv[2], fixtures)) return 2;
    g_scratch = scratch;
    g_fixtures = fixtures;
    static char models[1024];
    g_models = realpath(argv[3], models) ? models : argv[3];
    char data[800];
    snprintf(data, sizeof(data), "%s/http_data_%d", g_scratch, (int)getpid());
    if (app_open(&g_app, data, NULL) != LISA_OK) return 1;

    /* Point the models at <models_dir> through config.json, as a user would. */
    char e[1024], c[1024];
    snprintf(e, sizeof(e), "%s/Qwen3-Embedding-0.6B-Q8_0.gguf", g_models);
    snprintf(c, sizeof(c), "%s/Qwen3-4B-Q4_K_M.gguf", g_models);
    g_have_models = access(e, R_OK) == 0 && access(c, R_OK) == 0;
    if (g_have_models && (app_set_model(&g_app, APP_MODEL_EMBEDDING, e) != LISA_OK ||
                          app_set_model(&g_app, APP_MODEL_CHAT, c) != LISA_OK))
        return 1;

    server_options_t o = { &g_app, 0, NULL, NULL, TOKEN, NULL, 0 };
    const char* err = NULL;
    if (server_start(&o, &g_srv, &err) != LISA_OK) {
        fprintf(stderr, "server_start: %s\n", err);
        return 1;
    }
    g_port = server_port(g_srv);

    UNITY_BEGIN();
    RUN_TEST(test_health_and_token);
    RUN_TEST(test_host_and_origin);
    RUN_TEST(test_routes_and_validation);
    RUN_TEST(test_body_limit);
    RUN_TEST(test_start_errors_and_generated_token);
    RUN_TEST(test_extension_points);
    RUN_TEST(test_static_files_and_settings);
    RUN_TEST(test_ingest_search_ask);
    int failures = UNITY_END();
    server_stop(g_srv);
    app_close(&g_app);
    return failures == 0 ? 0 : 1;
}
