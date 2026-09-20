/* SPDX-License-Identifier: BUSL-1.1 */
/* LISA HTTP API over CivetWeb (server.h). */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "civetweb.h"
#include "yyjson.h"
#include "../app/app_json.h"
#include "../app/log.h"
#include "../platform/platform.h"

#define MAX_BODY        (1 << 20)
#define MAX_COLLECTIONS 256
#define MAX_JOBS_KEPT   100
#define MAX_MESSAGES    16
#define MAX_PATHS       64
#define MAX_TOPK        100

typedef enum { JOB_QUEUED = 0, JOB_RUNNING, JOB_SUCCEEDED, JOB_FAILED, JOB_CANCELLED } job_state_t;
static const char* const k_job_state[] = { "queued", "running", "succeeded", "failed", "cancelled" };

typedef struct {
    int64_t              id;
    char                 name[APP_NAME_MAX + 1];
    char**               paths;
    int64_t              n_paths;
    job_state_t          state;
    char                 error[200];
    lisa_ingest_status_t st;
} job_t;

typedef struct {
    char               name[APP_NAME_MAX + 1];
    lisa_collection_t* coll;   /* read handle */
} cached_t;

struct lisa_server {
    struct mg_context* mg;
    app_t*             app;
    lisa_model_t*      chat;
    lisa_model_t*      embed;
    int                port;
    char               token[SERVER_TOKEN_LEN + 1];
    char               host_ok[2][40];
    char               origin_ok[2][48];

    lisa_mutex_t*      engine;           /* models and cached read handles */
    cached_t           cache[MAX_COLLECTIONS];
    int                n_cache;

    lisa_mutex_t*      jobs_mu;
    job_t**            jobs;
    int64_t            n_jobs, cap_jobs, next_id;
    lisa_thread_t*     worker;
    lisa_model_t*      ingest_embed;     /* worker's own embedding model */
    const server_asset_t* assets;
    int                n_assets;
    volatile int       stop;
};

/* ---- responses ---------------------------------------------------------- */

static const char* reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

static void send_body(struct mg_connection* conn, int status, const char* type, const char* body, size_t len) {
    mg_printf(conn,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %zu\r\n"
              "Cache-Control: no-store\r\n"
              "X-Content-Type-Options: nosniff\r\n"
              "\r\n",
              status, reason(status), type, len);
    if (len) mg_write(conn, body, len);
}

static int send_doc(struct mg_connection* conn, int status, yyjson_mut_doc* doc) {
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (json == NULL) {
        static const char oom[] = "{\"error\":{\"code\":\"internal\",\"message\":\"out of memory\"}}";
        send_body(conn, 500, "application/json", oom, sizeof(oom) - 1);
        return 500;
    }
    send_body(conn, status, "application/json", json, len);
    free(json);
    return status;
}

static int send_error(struct mg_connection* conn, int status, const char* code, const char* message) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val* e = yyjson_mut_obj_add_obj(doc, root, "error");
    yyjson_mut_obj_add_str(doc, e, "code", code);
    yyjson_mut_obj_add_strcpy(doc, e, "message", message);
    return send_doc(conn, status, doc);
}

/* An error from a lisa_* call. */
static int send_status(struct mg_connection* conn, int rc) {
    int http = 500;
    const char* code = "internal";
    switch (rc) {
    case LISA_E_INVALID_ARGUMENT: http = 400; code = "invalid_argument"; break;
    case LISA_E_NOT_FOUND:        http = 404; code = "not_found"; break;
    case LISA_E_MODEL_MISMATCH:   http = 409; code = "model_mismatch"; break;
    case LISA_E_BUSY:             http = 409; code = "busy"; break;
    case LISA_E_DENIED:           http = 403; code = "denied"; break;
    case LISA_E_UNSUPPORTED:      http = 400; code = "unsupported"; break;
    case LISA_E_TOO_LONG:         http = 400; code = "too_long"; break;
    case LISA_E_FORMAT:           http = 500; code = "format"; break;
    default: break;
    }
    return send_error(conn, http, code, lisa_status_string(rc));
}

/* ---- request helpers ---------------------------------------------------- */

static int read_body(struct mg_connection* conn, char** out, size_t* out_len) {
    *out = NULL;
    *out_len = 0;
    const struct mg_request_info* ri = mg_get_request_info(conn);
    if (ri->content_length > MAX_BODY) return 413;
    size_t cap = ri->content_length > 0 ? (size_t)ri->content_length + 1 : 4096;
    char* buf = (char*)malloc(cap);
    if (buf == NULL) return 500;
    size_t len = 0;
    for (;;) {
        if (len + 1 >= cap) {
            if (cap > MAX_BODY) {
                free(buf);
                return 413;
            }
            char* g = (char*)realloc(buf, cap * 2);
            if (g == NULL) {
                free(buf);
                return 500;
            }
            buf = g;
            cap *= 2;
        }
        int n = mg_read(conn, buf + len, cap - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return 0;
}

/* Parse a JSON object body; sends the error response itself on failure. */
static yyjson_doc* read_json(struct mg_connection* conn, int* status) {
    char* body = NULL;
    size_t len = 0;
    int rc = read_body(conn, &body, &len);
    if (rc == 413) {
        *status = send_error(conn, 413, "too_large", "request body over 1 MiB");
        return NULL;
    }
    if (rc != 0) {
        *status = send_error(conn, 500, "internal", "cannot read the request body");
        return NULL;
    }
    yyjson_doc* doc = yyjson_read(body, len, 0);
    free(body);
    if (doc == NULL || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
        yyjson_doc_free(doc);
        *status = send_error(conn, 400, "invalid_json", "the body must be a JSON object");
        return NULL;
    }
    return doc;
}

static int ieq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return *a == *b;
}

/* Constant-time comparison for the token. */
static int token_ok(const char* got, const char* want) {
    size_t n = strlen(want);
    if (got == NULL || strlen(got) != n) return 0;
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(got[i] ^ want[i]);
    return d == 0;
}

/* ---- collections -------------------------------------------------------- */

/* A read handle for name, cached; call with the engine lock held. */
static int get_collection(lisa_server_t* s, const char* name, lisa_collection_t** out) {
    *out = NULL;
    for (int i = 0; i < s->n_cache; i++) {
        if (strcmp(s->cache[i].name, name) == 0) {
            lisa_collection_refresh(s->cache[i].coll);
            *out = s->cache[i].coll;
            return LISA_OK;
        }
    }
    char* path = app_collection_path(s->app, name);
    if (path == NULL) return LISA_E_INVALID_ARGUMENT;
    int rc = lisa_path_is_dir(path) ? LISA_OK : LISA_E_NOT_FOUND;
    lisa_collection_t* c = NULL;
    if (rc == LISA_OK) rc = lisa_collection_open(s->app->ctx, path, LISA_OPEN_READ, NULL, &c);
    free(path);
    if (rc != LISA_OK) return rc;
    if (s->n_cache == MAX_COLLECTIONS) {
        lisa_collection_close(c);
        return LISA_E_NO_MEMORY;
    }
    snprintf(s->cache[s->n_cache].name, sizeof(s->cache[0].name), "%s", name);
    s->cache[s->n_cache++].coll = c;
    *out = c;
    return LISA_OK;
}

/* ---- handlers ------------------------------------------------------------ */

static int h_health(lisa_server_t* s, struct mg_connection* conn) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "ok");
    yyjson_mut_obj_add_str(doc, root, "version", lisa_version(NULL, NULL, NULL));
    yyjson_mut_val* m = yyjson_mut_obj_add_obj(doc, root, "models");
    yyjson_mut_obj_add_bool(doc, m, "chat", s->chat != NULL);
    yyjson_mut_obj_add_bool(doc, m, "embedding", s->embed != NULL);
    return send_doc(conn, 200, doc);
}

typedef struct {
    lisa_server_t*  s;
    yyjson_mut_doc* doc;
    yyjson_mut_val* arr;
} list_ctx_t;

static int add_collection(void* user, const char* name) {
    list_ctx_t* l = (list_ctx_t*)user;
    lisa_collection_t* c = NULL;
    yyjson_mut_val* o = yyjson_mut_arr_add_obj(l->doc, l->arr);
    yyjson_mut_obj_add_strcpy(l->doc, o, "name", name);
    if (get_collection(l->s, name, &c) == LISA_OK) {
        lisa_collection_info_t info = LISA_COLLECTION_INFO_INIT;
        lisa_collection_info(c, &info);
        yyjson_mut_obj_add_strcpy(l->doc, o, "embedding_model", info.embedding_model);
        yyjson_mut_obj_add_int(l->doc, o, "dim", info.dim);
        yyjson_mut_obj_add_int(l->doc, o, "chunks", info.chunk_count);
    } else {
        yyjson_mut_obj_add_str(l->doc, o, "error", "cannot open");
    }
    return 0;
}

static int h_collections(lisa_server_t* s, struct mg_connection* conn) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    list_ctx_t l = { s, doc, yyjson_mut_obj_add_arr(doc, root, "collections") };
    lisa_mutex_lock(s->engine);
    int rc = app_list_collections(s->app, add_collection, &l);
    lisa_mutex_unlock(s->engine);
    if (rc != LISA_OK) {
        yyjson_mut_doc_free(doc);
        return send_status(conn, rc);
    }
    return send_doc(conn, 200, doc);
}

static void job_json(yyjson_mut_doc* doc, yyjson_mut_val* o, const job_t* j) {
    char id[32];
    snprintf(id, sizeof(id), "%lld", (long long)j->id);
    yyjson_mut_obj_add_strcpy(doc, o, "id", id);
    yyjson_mut_obj_add_strcpy(doc, o, "collection", j->name);
    yyjson_mut_obj_add_str(doc, o, "state", k_job_state[j->state]);
    if (j->error[0]) yyjson_mut_obj_add_strcpy(doc, o, "error", j->error);
    app_json_ingest_status(doc, o, &j->st);
}

static void job_free(job_t* j) {
    if (j == NULL) return;
    for (int64_t i = 0; i < j->n_paths; i++) free(j->paths[i]);
    free(j->paths);
    free(j);
}

static int h_ingest(lisa_server_t* s, struct mg_connection* conn, const char* name) {
    int status = 0;
    yyjson_doc* doc = read_json(conn, &status);
    if (doc == NULL) return status;
    yyjson_val* paths = yyjson_obj_get(yyjson_doc_get_root(doc), "paths");
    size_t n = yyjson_arr_size(paths);
    if (!yyjson_is_arr(paths) || n == 0 || n > MAX_PATHS) {
        yyjson_doc_free(doc);
        return send_error(conn, 400, "invalid_argument", "\"paths\" must be a list of 1 to 64 paths");
    }
    job_t* j = (job_t*)calloc(1, sizeof(job_t));
    if (j) j->paths = (char**)calloc(n, sizeof(char*));
    if (j == NULL || j->paths == NULL) {
        job_free(j);
        yyjson_doc_free(doc);
        return send_error(conn, 500, "internal", "out of memory");
    }
    snprintf(j->name, sizeof(j->name), "%s", name);
    j->st.struct_size = sizeof(j->st);
    size_t idx, max;
    yyjson_val* v;
    const char* bad = NULL;
    yyjson_arr_foreach(paths, idx, max, v) {
        const char* p = yyjson_get_str(v);
        if (p == NULL || p[0] != '/') {
            bad = "every path must be an absolute path";
            break;
        }
        if (!lisa_path_exists(p)) {
            bad = "a path does not exist";
            break;
        }
        j->paths[j->n_paths] = strdup(p);
        if (j->paths[j->n_paths] == NULL) {
            bad = "out of memory";
            break;
        }
        j->n_paths++;
    }
    yyjson_doc_free(doc);
    if (bad) {
        job_free(j);
        return send_error(conn, 400, "invalid_argument", bad);
    }

    lisa_mutex_lock(s->jobs_mu);
    int ok = 1;
    if (s->n_jobs == s->cap_jobs) {
        /* Forget the oldest finished job, or grow. */
        int64_t drop = -1;
        for (int64_t i = 0; i < s->n_jobs && drop < 0 && s->n_jobs >= MAX_JOBS_KEPT; i++) {
            if (s->jobs[i]->state >= JOB_SUCCEEDED) drop = i;
        }
        if (drop >= 0) {
            job_free(s->jobs[drop]);
            memmove(&s->jobs[drop], &s->jobs[drop + 1], (size_t)(s->n_jobs - drop - 1) * sizeof(job_t*));
            s->n_jobs--;
        } else {
            int64_t cap = s->cap_jobs ? s->cap_jobs * 2 : 16;
            job_t** g = (job_t**)realloc(s->jobs, (size_t)cap * sizeof(job_t*));
            if (g == NULL) ok = 0;
            else {
                s->jobs = g;
                s->cap_jobs = cap;
            }
        }
    }
    if (ok) {
        j->id = ++s->next_id;
        j->state = JOB_QUEUED;
        s->jobs[s->n_jobs++] = j;
    }
    yyjson_mut_doc* out = NULL;
    if (ok) {
        out = yyjson_mut_doc_new(NULL);
        yyjson_mut_val* root = yyjson_mut_obj(out);
        yyjson_mut_doc_set_root(out, root);
        job_json(out, yyjson_mut_obj_add_obj(out, root, "job"), j);
    }
    lisa_mutex_unlock(s->jobs_mu);
    if (!ok) {
        job_free(j);
        return send_error(conn, 500, "internal", "out of memory");
    }
    return send_doc(conn, 202, out);
}

static int h_job(lisa_server_t* s, struct mg_connection* conn, const char* id_text) {
    char* end = NULL;
    long long id = strtoll(id_text, &end, 10);
    if (end == id_text || *end != '\0') return send_error(conn, 404, "not_found", "no such job");
    yyjson_mut_doc* out = NULL;
    lisa_mutex_lock(s->jobs_mu);
    for (int64_t i = 0; i < s->n_jobs; i++) {
        if (s->jobs[i]->id == id) {
            out = yyjson_mut_doc_new(NULL);
            yyjson_mut_val* root = yyjson_mut_obj(out);
            yyjson_mut_doc_set_root(out, root);
            job_json(out, yyjson_mut_obj_add_obj(out, root, "job"), s->jobs[i]);
            break;
        }
    }
    lisa_mutex_unlock(s->jobs_mu);
    if (out == NULL) return send_error(conn, 404, "not_found", "no such job");
    return send_doc(conn, 200, out);
}

static int h_search(lisa_server_t* s, struct mg_connection* conn, const char* name,
                    const lisa_principal_t* principal) {
    int status = 0;
    yyjson_doc* doc = read_json(conn, &status);
    if (doc == NULL) return status;
    yyjson_val* root = yyjson_doc_get_root(doc);
    const char* query = yyjson_get_str(yyjson_obj_get(root, "query"));
    yyjson_val* tk = yyjson_obj_get(root, "topk");
    const char* mode = yyjson_get_str(yyjson_obj_get(root, "mode"));
    lisa_query_t q = LISA_QUERY_INIT;
    q.principal = principal;
    const char* bad = NULL;
    if (query == NULL || query[0] == '\0') bad = "\"query\" must be a non-empty string";
    if (tk && (!yyjson_is_int(tk) || yyjson_get_sint(tk) < 1 || yyjson_get_sint(tk) > MAX_TOPK))
        bad = "\"topk\" must be 1..100";
    else if (tk) q.top_k = yyjson_get_sint(tk);
    if (mode == NULL || strcmp(mode, "hybrid") == 0) q.mode = LISA_SEARCH_HYBRID;
    else if (strcmp(mode, "vector") == 0) q.mode = LISA_SEARCH_VECTOR;
    else if (strcmp(mode, "keyword") == 0) q.mode = LISA_SEARCH_KEYWORD;
    else bad = "\"mode\" must be hybrid, vector or keyword";
    if (bad) {
        yyjson_doc_free(doc);
        return send_error(conn, 400, "invalid_argument", bad);
    }
    if (s->embed == NULL) {
        yyjson_doc_free(doc);
        return send_error(conn, 503, "no_model", "no embedding model is loaded");
    }

    lisa_scored_hit_t hits[MAX_TOPK];
    int64_t n = 0;
    yyjson_mut_doc* out = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* oroot = yyjson_mut_obj(out);
    yyjson_mut_doc_set_root(out, oroot);
    yyjson_mut_val* arr = yyjson_mut_obj_add_arr(out, oroot, "hits");

    lisa_mutex_lock(s->engine);
    lisa_collection_t* c = NULL;
    int rc = get_collection(s, name, &c);
    if (rc == LISA_OK) rc = lisa_collection_query_text(c, s->embed, query, &q, hits, q.top_k, &n);
    for (int64_t i = 0; rc == LISA_OK && i < n; i++) {
        lisa_chunk_t* ch = NULL;
        if (lisa_collection_get_chunk(c, hits[i].id, &ch) != LISA_OK) continue;
        yyjson_mut_val* o = yyjson_mut_arr_add_obj(out, arr);
        yyjson_mut_obj_add_uint(out, o, "id", hits[i].id);
        yyjson_mut_obj_add_real(out, o, "score", hits[i].score);
        if (hits[i].distance >= 0) yyjson_mut_obj_add_real(out, o, "distance", hits[i].distance);
        if (hits[i].keyword_rank) yyjson_mut_obj_add_real(out, o, "keyword_score", hits[i].keyword_score);
        yyjson_mut_obj_add_strcpy(out, o, "path", ch->source_path);
        yyjson_mut_obj_add_int(out, o, "page", ch->page);
        yyjson_mut_obj_add_int(out, o, "offset", ch->offset);
        yyjson_mut_obj_add_int(out, o, "length", ch->length);
        yyjson_mut_obj_add_strcpy(out, o, "text", ch->text);
        lisa_chunk_free(ch);
    }
    lisa_mutex_unlock(s->engine);
    yyjson_doc_free(doc);
    if (rc != LISA_OK) {
        yyjson_mut_doc_free(out);
        return send_status(conn, rc);
    }
    return send_doc(conn, 200, out);
}

static int sse_event(struct mg_connection* conn, const char* event, yyjson_mut_doc* doc) {
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (json == NULL) return -1;
    int ok = mg_printf(conn, "event: %s\ndata: ", event) > 0 && mg_write(conn, json, len) > 0 &&
             mg_write(conn, "\n\n", 2) > 0;
    free(json);
    return ok ? 0 : -1;
}

static int on_sse_token(void* user, const char* text, int64_t len) {
    struct mg_connection* conn = (struct mg_connection*)user;
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) return 1;
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strncpy(doc, root, "text", text, (size_t)len);
    return sse_event(conn, "token", doc) != 0;   /* client gone: stop generating */
}

static int h_ask(lisa_server_t* s, struct mg_connection* conn, const char* name,
                 const lisa_principal_t* principal) {
    int status = 0;
    yyjson_doc* doc = read_json(conn, &status);
    if (doc == NULL) return status;
    yyjson_val* root = yyjson_doc_get_root(doc);
    yyjson_val* msgs = yyjson_obj_get(root, "messages");
    int stream = yyjson_get_bool(yyjson_obj_get(root, "stream"));
    yyjson_val* tk = yyjson_obj_get(root, "topk");
    lisa_message_t m[MAX_MESSAGES];
    size_t n = yyjson_arr_size(msgs);
    const char* bad = NULL;
    if (!yyjson_is_arr(msgs) || n == 0 || n > MAX_MESSAGES) bad = "\"messages\" must be a list of 1 to 16 messages";
    size_t idx, max;
    yyjson_val* v;
    yyjson_arr_foreach(msgs, idx, max, v) {
        if (bad || idx >= MAX_MESSAGES) break;
        m[idx].role = yyjson_get_str(yyjson_obj_get(v, "role"));
        m[idx].content = yyjson_get_str(yyjson_obj_get(v, "content"));
        if (m[idx].role == NULL || m[idx].content == NULL) bad = "each message needs \"role\" and \"content\"";
    }
    lisa_ask_options_t o = LISA_ASK_OPTIONS_INIT;
    o.principal = principal;
    if (tk && (!yyjson_is_int(tk) || yyjson_get_sint(tk) < 1 || yyjson_get_sint(tk) > MAX_TOPK))
        bad = "\"topk\" must be 1..100";
    else if (tk) o.top_k = yyjson_get_sint(tk);
    if (bad) {
        yyjson_doc_free(doc);
        return send_error(conn, 400, "invalid_argument", bad);
    }
    if (s->embed == NULL || s->chat == NULL) {
        yyjson_doc_free(doc);
        return send_error(conn, 503, "no_model", "the chat and embedding models are not both loaded");
    }

    lisa_mutex_lock(s->engine);
    lisa_collection_t* c = NULL;
    int rc = get_collection(s, name, &c);
    lisa_answer_t* a = NULL;
    int result;
    if (rc != LISA_OK) {
        result = send_status(conn, rc);
    } else if (stream) {
        /* Headers first; errors after this point arrive as an `error` event. */
        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/event-stream\r\n"
                  "Cache-Control: no-store\r\n"
                  "X-Content-Type-Options: nosniff\r\n"
                  "Connection: close\r\n"
                  "\r\n");
        o.on_token = on_sse_token;
        o.on_token_user = conn;
        rc = lisa_ask(c, s->embed, s->chat, m, (int64_t)n, &o, &a);
        yyjson_mut_doc* out = yyjson_mut_doc_new(NULL);
        yyjson_mut_val* oroot = yyjson_mut_obj(out);
        yyjson_mut_doc_set_root(out, oroot);
        if (rc == LISA_OK) {
            app_json_answer(out, oroot, a);
            sse_event(conn, "answer", out);
        } else {
            yyjson_mut_obj_add_str(out, oroot, "message", lisa_status_string(rc));
            sse_event(conn, "error", out);
        }
        result = 200;
    } else {
        rc = lisa_ask(c, s->embed, s->chat, m, (int64_t)n, &o, &a);
        if (rc != LISA_OK) {
            result = send_status(conn, rc);
        } else {
            yyjson_mut_doc* out = yyjson_mut_doc_new(NULL);
            yyjson_mut_val* oroot = yyjson_mut_obj(out);
            yyjson_mut_doc_set_root(out, oroot);
            app_json_answer(out, oroot, a);
            result = send_doc(conn, 200, out);
        }
    }
    lisa_mutex_unlock(s->engine);
    lisa_answer_free(a);
    yyjson_doc_free(doc);
    return result;
}

static void model_json(lisa_server_t* s, yyjson_mut_doc* doc, yyjson_mut_val* o, app_model_kind kind,
                       int loaded) {
    const char* source = NULL;
    char* path = app_find_model(s->app, kind, &source);
    if (path) yyjson_mut_obj_add_strcpy(doc, o, "path", path);
    else yyjson_mut_obj_add_null(doc, o, "path");
    yyjson_mut_obj_add_bool(doc, o, "loaded", loaded != 0);
    if (source) yyjson_mut_obj_add_str(doc, o, "source", source);
    free(path);
}

static int h_settings_get(lisa_server_t* s, struct mg_connection* conn) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "version", lisa_version(NULL, NULL, NULL));
    yyjson_mut_obj_add_strcpy(doc, root, "data_dir", s->app->data_dir);
    yyjson_mut_val* m = yyjson_mut_obj_add_obj(doc, root, "models");
    model_json(s, doc, yyjson_mut_obj_add_obj(doc, m, "chat"), APP_MODEL_CHAT, s->chat != NULL);
    model_json(s, doc, yyjson_mut_obj_add_obj(doc, m, "embedding"), APP_MODEL_EMBEDDING, s->embed != NULL);
    return send_doc(conn, 200, doc);
}

/*
 * The known model this file is, judged by exact file name and size (fast;
 * hashing a multi-GB file does not belong in a request). `lisa model`
 * verifies the full SHA-256.
 */
static int known_by_name_and_size(const char* path, lisa_known_model_t* out) {
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    int64_t size = lisa_file_size(path);
    if (size < 0) return LISA_E_NOT_FOUND;
    int64_t n = lisa_known_model_count();
    for (int64_t i = 0; i < n; i++) {
        if (lisa_known_model(i, out) == LISA_OK && out->file_size == size && strcmp(out->file_name, base) == 0)
            return LISA_OK;
    }
    return LISA_E_UNSUPPORTED;
}

static int h_settings_post(lisa_server_t* s, struct mg_connection* conn) {
    int status = 0;
    yyjson_doc* doc = read_json(conn, &status);
    if (doc == NULL) return status;
    yyjson_val* root = yyjson_doc_get_root(doc);
    static const char* const keys[2] = { "chat_model", "embedding_model" };
    const char* bad = NULL;
    int changed = 0;
    for (int k = 0; k < 2 && bad == NULL; k++) {
        yyjson_val* v = yyjson_obj_get(root, keys[k]);
        if (v == NULL) continue;
        const char* p = yyjson_get_str(v);
        lisa_known_model_t km;
        if (p == NULL || p[0] != '/') bad = "model paths must be absolute";
        else if (known_by_name_and_size(p, &km) != LISA_OK)
            bad = "not a known model file (unknown files can be set with `lisa model --set`)";
        else if ((km.is_embedding != 0) != (k == APP_MODEL_EMBEDDING))
            bad = k == APP_MODEL_CHAT ? "that is an embedding model" : "that is not an embedding model";
        else if (app_set_model(s->app, (app_model_kind)k, p) != LISA_OK)
            bad = "cannot write config.json";
        else changed++;
    }
    yyjson_doc_free(doc);
    if (bad) return send_error(conn, 400, "invalid_argument", bad);
    if (changed == 0) return send_error(conn, 400, "invalid_argument", "give chat_model and/or embedding_model");
    yyjson_mut_doc* out = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* oroot = yyjson_mut_obj(out);
    yyjson_mut_doc_set_root(out, oroot);
    yyjson_mut_obj_add_bool(out, oroot, "restart_required", true);
    return send_doc(conn, 200, out);
}

/* Built-in static files (the GUI). Returns 0 if path is not one. */
static int h_static(lisa_server_t* s, struct mg_connection* conn, const char* path) {
    if (strcmp(path, "/") == 0) path = "/index.html";
    for (int i = 0; i < s->n_assets; i++) {
        const server_asset_t* a = &s->assets[i];
        if (strcmp(a->path, path) != 0) continue;
        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: %s\r\n"
                  "Content-Length: %zu\r\n"
                  "Cache-Control: no-store\r\n"
                  "X-Content-Type-Options: nosniff\r\n"
                  "X-Frame-Options: DENY\r\n"
                  "Referrer-Policy: no-referrer\r\n"
                  "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; "
                  "img-src 'self' data:; connect-src 'self'; font-src 'self'; base-uri 'none'; "
                  "form-action 'none'; frame-ancestors 'none'\r\n"
                  "\r\n",
                  a->mime, a->len);
        mg_write(conn, a->data, a->len);
        return 200;
    }
    return 0;
}

/* Offer a request the core does not handle to the routes extension. */
static int h_extension(lisa_server_t* s, struct mg_connection* conn, const char* method,
                       const char* path, const lisa_principal_t* principal) {
    const lisa_http_routes_t* routes = lisa_context_http_routes(s->app->ctx);
    if (routes == NULL) return send_error(conn, 404, "not_found", "no such route");
    char* body = NULL;
    size_t len = 0;
    if (read_body(conn, &body, &len) != 0) return send_error(conn, 413, "too_large", "request body over 1 MiB");
    lisa_http_request_t req = { sizeof(req), method, path, body, (int64_t)len, principal };
    lisa_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.struct_size = sizeof(resp);
    int rc = routes->handle(routes->user, &req, &resp);
    free(body);
    if (rc == LISA_E_NOT_FOUND) {
        lisa_free(s->app->ctx, resp.body);
        return send_error(conn, 404, "not_found", "no such route");
    }
    if (rc != LISA_OK || resp.status_code < 100 || resp.status_code > 599) {
        lisa_free(s->app->ctx, resp.body);
        return send_error(conn, 500, "internal", "extension route failed");
    }
    send_body(conn, resp.status_code, resp.content_type ? resp.content_type : "application/octet-stream",
              resp.body, resp.body ? (size_t)resp.body_len : 0);
    lisa_free(s->app->ctx, resp.body);
    return resp.status_code;
}

/* ---- routing ------------------------------------------------------------- */

static int route(struct mg_connection* conn, void* cbdata) {
    lisa_server_t* s = (lisa_server_t*)cbdata;
    const struct mg_request_info* ri = mg_get_request_info(conn);
    const char* method = ri->request_method;
    const char* path = ri->local_uri ? ri->local_uri : "/";

    /* Local security: Host, Origin, token. */
    const char* host = mg_get_header(conn, "Host");
    if (host == NULL || !(ieq(host, s->host_ok[0]) || ieq(host, s->host_ok[1])))
        return send_error(conn, 403, "forbidden_host", "requests must be addressed to the local server");
    const char* origin = mg_get_header(conn, "Origin");
    if (origin && !(ieq(origin, s->origin_ok[0]) || ieq(origin, s->origin_ok[1])))
        return send_error(conn, 403, "forbidden_origin", "cross-site requests are not allowed");

    int is_health = strcmp(path, "/v1/health") == 0;
    if (is_health && strcmp(method, "GET") == 0) return h_health(s, conn);
    if (strcmp(method, "GET") == 0 && strncmp(path, "/v1/", 4) != 0) {
        int st = h_static(s, conn, path);
        if (st) return st;
    }

    const char* auth = mg_get_header(conn, "Authorization");
    if (auth == NULL || strncmp(auth, "Bearer ", 7) != 0 || !token_ok(auth + 7, s->token))
        return send_error(conn, 401, "unauthorized", "missing or wrong session token");

    /* Enterprise authentication, if installed. */
    lisa_principal_t principal;
    memset(&principal, 0, sizeof(principal));
    const lisa_principal_t* who = NULL;
    const lisa_auth_provider_t* ap = lisa_context_auth(s->app->ctx);
    if (ap) {
        const char* names[64];
        const char* values[64];
        int nh = ri->num_headers < 64 ? ri->num_headers : 64;
        for (int i = 0; i < nh; i++) {
            names[i] = ri->http_headers[i].name;
            values[i] = ri->http_headers[i].value;
        }
        lisa_auth_request_t req = { sizeof(req), method, path, names, values, nh, ri->remote_addr };
        if (ap->authenticate(ap->user, &req, &principal) != LISA_OK)
            return send_error(conn, 403, "denied", "authentication failed");
        who = &principal;
    }

    int result;
    char name[APP_NAME_MAX + 2];
    const char* rest = NULL;
    if (strcmp(path, "/v1/settings") == 0) {
        result = strcmp(method, "GET") == 0    ? h_settings_get(s, conn)
                 : strcmp(method, "POST") == 0 ? h_settings_post(s, conn)
                                               : send_error(conn, 405, "method_not_allowed", "use GET or POST");
    } else if (strcmp(path, "/v1/collections") == 0) {
        result = strcmp(method, "GET") == 0 ? h_collections(s, conn)
                                            : send_error(conn, 405, "method_not_allowed", "use GET");
    } else if (strncmp(path, "/v1/jobs/", 9) == 0) {
        result = strcmp(method, "GET") == 0 ? h_job(s, conn, path + 9)
                                            : send_error(conn, 405, "method_not_allowed", "use GET");
    } else if (strncmp(path, "/v1/collections/", 16) == 0 && (rest = strchr(path + 16, '/')) != NULL &&
               (size_t)(rest - (path + 16)) <= APP_NAME_MAX) {
        size_t nl = (size_t)(rest - (path + 16));
        memcpy(name, path + 16, nl);
        name[nl] = '\0';
        rest++;
        int known = strcmp(rest, "ingest") == 0 || strcmp(rest, "search") == 0 || strcmp(rest, "ask") == 0;
        if (!known) result = h_extension(s, conn, method, path, who);
        else if (!app_valid_name(name))
            result = send_error(conn, 400, "invalid_name", "collection names are 1-64 of [A-Za-z0-9_-]");
        else if (strcmp(method, "POST") != 0) result = send_error(conn, 405, "method_not_allowed", "use POST");
        else if (strcmp(rest, "ingest") == 0) result = h_ingest(s, conn, name);
        else if (strcmp(rest, "search") == 0) result = h_search(s, conn, name, who);
        else result = h_ask(s, conn, name, who);
    } else {
        result = h_extension(s, conn, method, path, who);
    }

    if (ap && ap->release) ap->release(ap->user, &principal);
    return result;
}

/* ---- ingest worker -------------------------------------------------------- */

static void set_job(lisa_server_t* s, job_t* j, job_state_t state, const lisa_ingest_status_t* st,
                    const char* error) {
    lisa_mutex_lock(s->jobs_mu);
    j->state = state;
    if (st) j->st = *st;
    if (error) snprintf(j->error, sizeof(j->error), "%s", error);
    lisa_mutex_unlock(s->jobs_mu);
}

static void run_job(lisa_server_t* s, job_t* j) {
    const char* err = NULL;
    if (s->ingest_embed == NULL && app_load_model(s->app, APP_MODEL_EMBEDDING, &s->ingest_embed, &err) != LISA_OK) {
        set_job(s, j, JOB_FAILED, NULL, err);
        return;
    }
    char* path = app_collection_path(s->app, j->name);
    int rc = path ? LISA_OK : LISA_E_INVALID_ARGUMENT;
    if (rc == LISA_OK && !lisa_path_exists(path))
        rc = lisa_collection_create_for_model(s->app->ctx, path, s->ingest_embed, 0);
    lisa_collection_t* c = NULL;
    if (rc == LISA_OK) rc = lisa_collection_open(s->app->ctx, path, LISA_OPEN_WRITE, NULL, &c);
    free(path);
    lisa_ingest_job_t* job = NULL;
    if (rc == LISA_OK)
        rc = lisa_ingest_start(c, s->ingest_embed, (const char* const*)j->paths, j->n_paths, NULL, &job);
    if (rc != LISA_OK) {
        set_job(s, j, JOB_FAILED, NULL, lisa_status_string(rc));
        LOG_WARN("ingest job %lld (%s) could not start: %s", (long long)j->id, j->name,
                 lisa_status_string(rc));
        lisa_collection_close(c);
        return;
    }
    lisa_ingest_status_t st = LISA_INGEST_STATUS_INIT;
    for (;;) {
        lisa_ingest_status(job, &st);
        if (st.state != LISA_JOB_RUNNING) break;
        set_job(s, j, JOB_RUNNING, &st, NULL);
        if (s->stop) lisa_ingest_cancel(job);
        lisa_sleep_ms(100);
    }
    lisa_ingest_wait(job, &st);
    lisa_ingest_free(job);
    lisa_collection_close(c);
    job_state_t final = st.state == LISA_JOB_SUCCEEDED ? JOB_SUCCEEDED
                      : st.state == LISA_JOB_CANCELLED ? JOB_CANCELLED : JOB_FAILED;
    set_job(s, j, final, &st, final == JOB_FAILED ? lisa_status_string(st.status) : NULL);
    log_write(final == JOB_FAILED ? LOG_WARN : LOG_INFO,
              "ingest job %lld (%s): %s, %lld files seen, %lld added, %lld updated, %lld unchanged, "
              "%lld without text, %lld failed, %lld chunks in %.1f s",
              (long long)j->id, j->name, k_job_state[final], (long long)st.files_seen,
              (long long)st.files_added, (long long)st.files_updated, (long long)st.files_unchanged,
              (long long)st.files_no_text, (long long)st.files_failed, (long long)st.chunks_added,
              st.elapsed_seconds);
}

static void worker(void* arg) {
    lisa_server_t* s = (lisa_server_t*)arg;
    while (!s->stop) {
        job_t* next = NULL;
        lisa_mutex_lock(s->jobs_mu);
        for (int64_t i = 0; i < s->n_jobs && next == NULL; i++) {
            if (s->jobs[i]->state == JOB_QUEUED) {
                next = s->jobs[i];
                next->state = JOB_RUNNING;
            }
        }
        lisa_mutex_unlock(s->jobs_mu);
        if (next) run_job(s, next);
        else lisa_sleep_ms(100);
    }
}

/* ---- start / stop ----------------------------------------------------------- */

static int quiet_log(const struct mg_connection* conn, const char* message) {
    (void)conn;
    (void)message;
    return 1;
}

int server_start(const server_options_t* opts, lisa_server_t** out, const char** err) {
    const char* dummy;
    if (err == NULL) err = &dummy;
    *err = "";
    *out = NULL;
    if (opts == NULL || opts->app == NULL || opts->port < 0 || opts->port > 65535) {
        *err = "invalid server options";
        return LISA_E_INVALID_ARGUMENT;
    }
    lisa_server_t* s = (lisa_server_t*)calloc(1, sizeof(*s));
    if (s == NULL) return LISA_E_NO_MEMORY;
    s->app = opts->app;
    s->chat = opts->chat;
    s->embed = opts->embed;
    s->assets = opts->assets;
    s->n_assets = opts->assets ? opts->n_assets : 0;

    if (opts->token) {
        if (strlen(opts->token) < 16 || strlen(opts->token) > SERVER_TOKEN_LEN) {
            free(s);
            *err = "token must be 16 to 64 characters";
            return LISA_E_INVALID_ARGUMENT;
        }
        snprintf(s->token, sizeof(s->token), "%s", opts->token);
    } else {
        unsigned char r[SERVER_TOKEN_LEN / 2];
        if (lisa_random_bytes(r, sizeof(r)) != LISA_PLAT_OK) {
            free(s);
            *err = "no secure random source";
            return LISA_E_IO;
        }
        for (size_t i = 0; i < sizeof(r); i++) snprintf(s->token + 2 * i, 3, "%02x", r[i]);
    }

    if (lisa_mutex_create(&s->engine) != LISA_PLAT_OK || lisa_mutex_create(&s->jobs_mu) != LISA_PLAT_OK) {
        server_stop(s);
        return LISA_E_NO_MEMORY;
    }

    mg_init_library(0);
    char ports[32];
    snprintf(ports, sizeof(ports), "127.0.0.1:%d", opts->port);
    const char* options[] = {
        "listening_ports", ports,
        "num_threads", "8",
        "request_timeout_ms", "600000",
        "enable_keep_alive", "yes",
        NULL
    };
    struct mg_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.log_message = quiet_log;
    s->mg = mg_start(&cb, s, options);
    if (s->mg == NULL) {
        server_stop(s);
        *err = "cannot listen on the port (in use?)";
        return LISA_E_IO;
    }
    struct mg_server_port sp[4];
    int np = mg_get_server_ports(s->mg, 4, sp);
    s->port = np > 0 ? sp[0].port : opts->port;
    snprintf(s->host_ok[0], sizeof(s->host_ok[0]), "127.0.0.1:%d", s->port);
    snprintf(s->host_ok[1], sizeof(s->host_ok[1]), "localhost:%d", s->port);
    snprintf(s->origin_ok[0], sizeof(s->origin_ok[0]), "http://127.0.0.1:%d", s->port);
    snprintf(s->origin_ok[1], sizeof(s->origin_ok[1]), "http://localhost:%d", s->port);
    mg_set_request_handler(s->mg, "/", route, s);

    if (lisa_thread_start(worker, s, &s->worker) != LISA_PLAT_OK) {
        server_stop(s);
        *err = "cannot start the ingest worker";
        return LISA_E_NO_MEMORY;
    }
    *out = s;
    return LISA_OK;
}

int server_port(const lisa_server_t* s) {
    return s ? s->port : 0;
}

const char* server_token(const lisa_server_t* s) {
    return s ? s->token : NULL;
}

void server_stop(lisa_server_t* s) {
    if (s == NULL) return;
    if (s->mg) {
        mg_stop(s->mg);   /* waits for running requests */
        mg_exit_library();
    }
    s->stop = 1;
    lisa_thread_join(s->worker);
    for (int i = 0; i < s->n_cache; i++) lisa_collection_close(s->cache[i].coll);
    for (int64_t i = 0; i < s->n_jobs; i++) job_free(s->jobs[i]);
    free(s->jobs);
    lisa_model_free(s->ingest_embed);
    lisa_mutex_destroy(s->engine);
    lisa_mutex_destroy(s->jobs_mu);
    free(s);
}
