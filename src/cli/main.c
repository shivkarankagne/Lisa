/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * lisa — command-line interface (plan §6 W9). See src/cli/README.md.
 *
 * Uses LISA through include/lisa.h, the program layer in src/app, the
 * HTTP server in src/http, and OS services through src/platform.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisa.h"
#include "yyjson.h"
#include "../app/app.h"
#include "../app/app_json.h"
#include "../app/log.h"
#include "../gui/gui.h"
#include "../http/server.h"
#include "../platform/platform.h"

/* Exit codes (stable; documented in README.md). */
#define EXIT_OK        0
#define EXIT_USAGE     1   /* bad command line */
#define EXIT_NOT_FOUND 2   /* collection, file, or model not found */
#define EXIT_MODEL     3   /* wrong or mismatched model */
#define EXIT_BUSY      4   /* collection in use by another writer */
#define EXIT_ERROR     5   /* anything else */
#define EXIT_STOPPED   130 /* interrupted (Ctrl-C) */

static const char k_usage[] =
    "usage:\n"
    "  lisa ingest  [--data <dir>] --collection <name> <path>...   index files and folders\n"
    "  lisa search  [--data <dir>] --collection <name> --query \"<text>\" [--topk N]\n"
    "  lisa ask     [--data <dir>] --collection <name> [--topk N] \"<question>\"\n"
    "  lisa serve   [--data <dir>] [--port <port>]                  local HTTP API\n"
    "  lisa gui     [--data <dir>] [--port <port>] [--browser]      desktop window\n"
    "  lisa model   [--data <dir>] [--set <file.gguf>]              show or set models\n"
    "  lisa migrate --from <v1-dir> --to <dir> [--model <id>]       convert a LISA 0.1 collection\n"
    "  lisa --version\n"
    "\n"
    "Everything runs on this computer: LISA makes no network connections except\n"
    "its own server on 127.0.0.1, and sends nothing anywhere.\n"
    "\n"
    "Add --json to ingest, search, ask, or model for machine-readable output, and\n"
    "--log-level error|warn|info|debug to any command (the log is <data>/lisa.log).\n"
    "Default data directory: ~/Library/Application Support/LISA (macOS).\n";

static int exit_for(int rc) {
    switch (rc) {
    case LISA_OK:                 return EXIT_OK;
    case LISA_E_INVALID_ARGUMENT: return EXIT_USAGE;
    case LISA_E_NOT_FOUND:        return EXIT_NOT_FOUND;
    case LISA_E_MODEL_MISMATCH:
    case LISA_E_WRONG_MODEL_KIND: return EXIT_MODEL;
    case LISA_E_BUSY:             return EXIT_BUSY;
    case LISA_E_CANCELLED:        return EXIT_STOPPED;
    default:                      return EXIT_ERROR;
    }
}

static int fail(int rc, const char* what, const char* detail) {
    fprintf(stderr, "lisa: %s: %s\n", what, detail && detail[0] ? detail : lisa_status_string(rc));
    return exit_for(rc);
}

/* ---- arguments ---------------------------------------------------------- */

typedef struct {
    const char* data;
    const char* collection;
    const char* query;
    const char* from;
    const char* to;
    const char* model;
    const char* set;
    const char* token;
    const char* log_level;
    int64_t     topk;
    int         port;
    int         json;
    int         browser;
    const char* pos[256];
    int         n_pos;
} args_t;

static int parse_int(const char* s, int64_t lo, int64_t hi, int64_t* out) {
    char* end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

/* Returns 0 on success, or prints the problem and returns EXIT_USAGE. */
static int parse_args(int argc, char** argv, args_t* a) {
    memset(a, 0, sizeof(*a));
    a->topk = -1;
    a->port = SERVER_DEFAULT_PORT;
    for (int i = 0; i < argc; i++) {
        const char* s = argv[i];
        const char** slot = NULL;
        if (strcmp(s, "--json") == 0) { a->json = 1; continue; }
        if (strcmp(s, "--browser") == 0) { a->browser = 1; continue; }
        if (strcmp(s, "--data") == 0) slot = &a->data;
        else if (strcmp(s, "--collection") == 0) slot = &a->collection;
        else if (strcmp(s, "--query") == 0) slot = &a->query;
        else if (strcmp(s, "--from") == 0) slot = &a->from;
        else if (strcmp(s, "--to") == 0) slot = &a->to;
        else if (strcmp(s, "--model") == 0) slot = &a->model;
        else if (strcmp(s, "--set") == 0) slot = &a->set;
        else if (strcmp(s, "--token") == 0) slot = &a->token;
        else if (strcmp(s, "--log-level") == 0) slot = &a->log_level;
        else if (strcmp(s, "--topk") == 0 || strcmp(s, "--port") == 0) {
            int64_t v;
            int is_port = s[2] == 'p';
            if (i + 1 >= argc || !parse_int(argv[i + 1], is_port ? 0 : 1, is_port ? 65535 : 100, &v)) {
                fprintf(stderr, "lisa: %s needs a number (%s)\n", s, is_port ? "0-65535" : "1-100");
                return EXIT_USAGE;
            }
            if (is_port) a->port = (int)v;
            else a->topk = v;
            i++;
            continue;
        } else if (s[0] == '-' && s[1] == '-') {
            fprintf(stderr, "lisa: unknown option %s\n%s", s, k_usage);
            return EXIT_USAGE;
        } else {
            if (a->n_pos < 256) a->pos[a->n_pos++] = s;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "lisa: %s needs a value\n", s);
            return EXIT_USAGE;
        }
        *slot = argv[++i];
    }
    return 0;
}

static int need_collection(const args_t* a) {
    if (a->collection == NULL) {
        fprintf(stderr, "lisa: --collection <name> is required\n");
        return EXIT_USAGE;
    }
    if (!app_valid_name(a->collection)) {
        fprintf(stderr, "lisa: collection names are 1-64 characters of A-Z a-z 0-9 _ -\n");
        return EXIT_USAGE;
    }
    return 0;
}

static void print_json(yyjson_mut_doc* doc) {
    char* s = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, NULL);
    if (s) printf("%s\n", s);
    free(s);
    yyjson_mut_doc_free(doc);
}

/* ---- ingest ------------------------------------------------------------- */

static int cmd_ingest(app_t* app, const args_t* a) {
    int bad = need_collection(a);
    if (bad) return bad;
    if (a->n_pos == 0) {
        fprintf(stderr, "lisa: give at least one file or folder to ingest\n");
        return EXIT_USAGE;
    }
    char* abs[256];
    int n = 0;
    for (int i = 0; i < a->n_pos; i++) {
        abs[n] = lisa_path_absolute(a->pos[i]);
        if (abs[n] == NULL) {
            for (int k = 0; k < n; k++) free(abs[k]);
            fprintf(stderr, "lisa: no such file or folder: %s\n", a->pos[i]);
            return EXIT_NOT_FOUND;
        }
        n++;
    }

    const char* err = NULL;
    lisa_model_t* embed = NULL;
    char* path = app_collection_path(app, a->collection);
    lisa_collection_t* c = NULL;
    lisa_ingest_job_t* job = NULL;
    int rc = app_load_model(app, APP_MODEL_EMBEDDING, &embed, &err);
    int created = 0;
    if (rc == LISA_OK && !lisa_path_exists(path)) {
        rc = lisa_collection_create_for_model(app->ctx, path, embed, 0);
        created = rc == LISA_OK;
        err = rc == LISA_OK ? NULL : "cannot create the collection";
    }
    if (rc == LISA_OK) {
        rc = lisa_collection_open(app->ctx, path, LISA_OPEN_WRITE, NULL, &c);
        if (rc == LISA_E_BUSY) err = "the collection is being written by another process";
        else if (rc != LISA_OK) err = "cannot open the collection";
    }
    if (rc == LISA_OK) {
        rc = lisa_ingest_start(c, embed, (const char* const*)abs, n, NULL, &job);
        if (rc == LISA_E_MODEL_MISMATCH) err = "the collection was built with a different embedding model";
    }

    lisa_ingest_status_t st = LISA_INGEST_STATUS_INIT;
    if (rc == LISA_OK) {
        if (!a->json) fprintf(stderr, "Indexing into '%s'%s... (Ctrl-C stops safely)\n", a->collection,
                              created ? " (new collection)" : "");
        lisa_stop_signals_install();
        int64_t last_seen = -1;
        for (;;) {
            lisa_ingest_status(job, &st);
            if (st.state != LISA_JOB_RUNNING) break;
            if (lisa_stop_requested()) lisa_ingest_cancel(job);
            if (!a->json && st.files_seen != last_seen && st.files_seen % 10 == 0 && st.files_seen > 0) {
                fprintf(stderr, "  %lld files, %lld chunks so far\n", (long long)st.files_seen,
                        (long long)st.chunks_added);
                last_seen = st.files_seen;
            }
            lisa_sleep_ms(100);
        }
        rc = lisa_ingest_wait(job, &st);
        lisa_ingest_free(job);
        if (st.state == LISA_JOB_CANCELLED) rc = LISA_E_CANCELLED;
    }

    if (a->json && (rc == LISA_OK || rc == LISA_E_CANCELLED)) {
        yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_str(doc, root, "collection", a->collection);
        yyjson_mut_obj_add_bool(doc, root, "cancelled", rc == LISA_E_CANCELLED);
        app_json_ingest_status(doc, root, &st);
        print_json(doc);
    }
    if (rc == LISA_OK || rc == LISA_E_CANCELLED) {
        log_write(st.files_failed > 0 ? LOG_WARN : LOG_INFO,
                  "ingest '%s': %s, %lld files seen, %lld added, %lld updated, %lld unchanged, "
                  "%lld without text, %lld failed, %lld unsupported, %lld chunks in %.1f s",
                  a->collection, rc == LISA_OK ? "done" : "stopped", (long long)st.files_seen,
                  (long long)st.files_added, (long long)st.files_updated,
                  (long long)st.files_unchanged, (long long)st.files_no_text,
                  (long long)st.files_failed, (long long)st.files_skipped,
                  (long long)st.chunks_added, st.elapsed_seconds);
    }
    if (!a->json && (rc == LISA_OK || rc == LISA_E_CANCELLED)) {
        printf("%s '%s': %lld added, %lld updated, %lld unchanged, %lld removed, %lld without text, "
               "%lld failed, %lld unsupported skipped; %lld chunks added in %.1f s\n",
               rc == LISA_OK ? "Indexed" : "Stopped indexing", a->collection,
               (long long)st.files_added, (long long)st.files_updated, (long long)st.files_unchanged,
               (long long)st.files_removed, (long long)st.files_no_text, (long long)st.files_failed,
               (long long)st.files_skipped, (long long)st.chunks_added, st.elapsed_seconds);
        if (st.files_failed > 0 || st.files_no_text > 0)
            printf("See which files: lisa does not index scanned PDFs (no text layer) or unreadable files.\n");
    }
    lisa_collection_close(c);
    lisa_model_free(embed);
    free(path);
    for (int k = 0; k < n; k++) free(abs[k]);
    if (rc == LISA_E_CANCELLED) return EXIT_STOPPED;
    return rc == LISA_OK ? EXIT_OK : fail(rc, "ingest", err);
}

/* ---- search and ask ------------------------------------------------------ */

/* Open the named collection read-only and load the models needed. */
static int open_for_query(app_t* app, const args_t* a, int need_chat, lisa_collection_t** c,
                          lisa_model_t** embed, lisa_model_t** chat, const char** err) {
    *c = NULL;
    *embed = *chat = NULL;
    char* path = app_collection_path(app, a->collection);
    int rc = lisa_path_is_dir(path) ? LISA_OK : LISA_E_NOT_FOUND;
    if (rc != LISA_OK) *err = "no such collection (create it with `lisa ingest`)";
    if (rc == LISA_OK) {
        rc = lisa_collection_open(app->ctx, path, LISA_OPEN_READ, NULL, c);
        if (rc != LISA_OK) *err = "cannot open the collection";
    }
    free(path);
    if (rc == LISA_OK) rc = app_load_model(app, APP_MODEL_EMBEDDING, embed, err);
    if (rc == LISA_OK && need_chat) rc = app_load_model(app, APP_MODEL_CHAT, chat, err);
    return rc;
}

static void close_query(lisa_collection_t* c, lisa_model_t* embed, lisa_model_t* chat) {
    lisa_collection_close(c);
    lisa_model_free(chat);   /* llama.cpp aborts at exit if a model is still loaded */
    lisa_model_free(embed);
}

static void print_snippet(const char* text, size_t max) {
    size_t n = 0;
    printf("   ");
    for (const char* p = text; *p && n < max; p++, n++) putchar(*p == '\n' ? ' ' : *p);
    if (strlen(text) > max) {
        /* Do not cut a UTF-8 sequence in half. */
        const char* p = text + max;
        while (((unsigned char)*p & 0xC0) == 0x80) putchar(*p++);
        printf("...");
    }
    printf("\n");
}

static int cmd_search(app_t* app, const args_t* a) {
    int bad = need_collection(a);
    if (bad) return bad;
    const char* q = a->query ? a->query : (a->n_pos == 1 ? a->pos[0] : NULL);
    if (q == NULL || q[0] == '\0') {
        fprintf(stderr, "lisa: --query \"<text>\" is required\n");
        return EXIT_USAGE;
    }
    lisa_collection_t* c;
    lisa_model_t *embed, *chat;
    const char* err = NULL;
    int rc = open_for_query(app, a, 0, &c, &embed, &chat, &err);
    lisa_query_t opt = LISA_QUERY_INIT;
    if (a->topk > 0) opt.top_k = a->topk;
    lisa_scored_hit_t hits[100];
    int64_t n = 0;
    if (rc == LISA_OK) {
        rc = lisa_collection_query_text(c, embed, q, &opt, hits, opt.top_k, &n);
        if (rc == LISA_E_MODEL_MISMATCH) err = "the collection was built with a different embedding model";
    }
    yyjson_mut_doc* doc = NULL;
    yyjson_mut_val* arr = NULL;
    if (rc == LISA_OK && a->json) {
        doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        arr = yyjson_mut_obj_add_arr(doc, root, "hits");
    }
    if (rc == LISA_OK && !a->json && n == 0) printf("No matches.\n");
    for (int64_t i = 0; rc == LISA_OK && i < n; i++) {
        lisa_chunk_t* ch = NULL;
        if (lisa_collection_get_chunk(c, hits[i].id, &ch) != LISA_OK) continue;
        if (doc) {
            yyjson_mut_val* o = yyjson_mut_arr_add_obj(doc, arr);
            yyjson_mut_obj_add_uint(doc, o, "id", hits[i].id);
            yyjson_mut_obj_add_real(doc, o, "score", hits[i].score);
            if (hits[i].distance >= 0)   /* unit vectors: cosine = 1 - d/2 */
                yyjson_mut_obj_add_real(doc, o, "similarity", 1.0 - hits[i].distance / 2.0);
            yyjson_mut_obj_add_strcpy(doc, o, "path", ch->source_path);
            yyjson_mut_obj_add_int(doc, o, "page", ch->page);
            yyjson_mut_obj_add_int(doc, o, "offset", ch->offset);
            yyjson_mut_obj_add_int(doc, o, "length", ch->length);
            yyjson_mut_obj_add_strcpy(doc, o, "text", ch->text);
        } else {
            if (ch->page > 0) printf("%lld. %s (page %lld)\n", (long long)(i + 1), ch->source_path, (long long)ch->page);
            else printf("%lld. %s\n", (long long)(i + 1), ch->source_path);
            print_snippet(ch->text, 200);
        }
        lisa_chunk_free(ch);
    }
    if (doc) print_json(doc);
    close_query(c, embed, chat);
    return rc == LISA_OK ? EXIT_OK : fail(rc, "search", err);
}

static int print_piece(void* user, const char* text, int64_t len) {
    (void)user;
    fwrite(text, 1, (size_t)len, stdout);
    fflush(stdout);
    return lisa_stop_requested();
}

static int cmd_ask(app_t* app, const args_t* a) {
    int bad = need_collection(a);
    if (bad) return bad;
    if (a->n_pos != 1 || a->pos[0][0] == '\0') {
        fprintf(stderr, "lisa: give one question, in quotes\n");
        return EXIT_USAGE;
    }
    lisa_collection_t* c;
    lisa_model_t *embed, *chat;
    const char* err = NULL;
    int rc = open_for_query(app, a, 1, &c, &embed, &chat, &err);
    lisa_ask_options_t o = LISA_ASK_OPTIONS_INIT;
    if (a->topk > 0) o.top_k = a->topk;
    if (!a->json) {
        o.on_token = print_piece;
        lisa_stop_signals_install();
    }
    lisa_answer_t* ans = NULL;
    if (rc == LISA_OK) {
        lisa_collection_info_t info = LISA_COLLECTION_INFO_INIT;
        if (lisa_collection_info(c, &info) == LISA_OK && info.chunk_count == 0)
            fprintf(stderr, "lisa: note: '%s' has no readable text yet (scanned pages and unsupported "
                            "files are skipped), so nothing can be found in it.\n", a->collection);
    }
    if (rc == LISA_OK) {
        lisa_message_t m = { "user", a->pos[0] };
        rc = lisa_ask(c, embed, chat, &m, 1, &o, &ans);
        if (rc == LISA_E_MODEL_MISMATCH) err = "the collection was built with a different embedding model";
        else if (rc == LISA_E_INTERNAL)
            err = "the model produced nothing; another AI app may be holding the GPU (close it and retry)";
    }
    if (rc == LISA_OK && a->json) {
        yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        app_json_answer(doc, root, ans);
        print_json(doc);
    } else if (rc == LISA_OK) {
        printf("\n");
        if (ans->citation_count > 0) printf("\nSources:\n");
        for (int64_t i = 0; i < ans->citation_count; i++) {
            const lisa_citation_t* ci = &ans->citations[i];
            printf("  [S%d] %s", ci->number, ci->source_path);
            if (ci->page > 0) printf(", page %lld", (long long)ci->page);
            printf("\n");
        }
        if (!ans->complete) printf("(stopped)\n");
    }
    int stopped = rc == LISA_OK && !ans->complete;
    lisa_answer_free(ans);
    close_query(c, embed, chat);
    if (stopped) return EXIT_STOPPED;
    return rc == LISA_OK ? EXIT_OK : fail(rc, "ask", err);
}

/* ---- serve / gui ----------------------------------------------------------- */

/* Load what models exist, start the server with the GUI files. */
static int start_server(app_t* app, const args_t* a, int gui, lisa_model_t** embed, lisa_model_t** chat,
                        lisa_server_t** srv, const char** err) {
    *embed = *chat = NULL;
    const char* why = NULL;
    if (app_load_model(app, APP_MODEL_EMBEDDING, embed, &why) != LISA_OK)
        fprintf(stderr, "lisa: warning: %s; search and ask are unavailable\n", why);
    if (app_load_model(app, APP_MODEL_CHAT, chat, &why) != LISA_OK)
        fprintf(stderr, "lisa: warning: %s; ask is unavailable\n", why);
    server_options_t so = { app, a->port, *chat, *embed, a->token, lisa_gui_assets, lisa_gui_asset_count };
    int rc = server_start(&so, srv, err);
    if (rc != LISA_OK && gui && a->port == SERVER_DEFAULT_PORT) {
        so.port = 0;   /* the default port is taken: any free port will do for the window */
        rc = server_start(&so, srv, err);
    }
    return rc;
}

static int cmd_serve(app_t* app, const args_t* a) {
    const char* err = NULL;
    lisa_model_t *embed, *chat;
    lisa_server_t* srv = NULL;
    lisa_stop_signals_install();
    int rc = start_server(app, a, 0, &embed, &chat, &srv, &err);
    if (rc == LISA_OK) {
        printf("LISA %s serving http://127.0.0.1:%d/v1 (local only)\n", lisa_version(NULL, NULL, NULL),
               server_port(srv));
        printf("Session token: %s\n", server_token(srv));
        printf("Send it as 'Authorization: Bearer <token>'. GUI: http://127.0.0.1:%d/#token=%s\n",
               server_port(srv), server_token(srv));
        printf("Ctrl-C stops the server.\n");
        LOG_INFO("serve: listening on 127.0.0.1:%d", server_port(srv));
        fflush(stdout);
        while (!lisa_stop_requested()) lisa_sleep_ms(200);
        printf("Stopping...\n");
        LOG_INFO("serve: stopping");
        server_stop(srv);
    }
    lisa_model_free(chat);
    lisa_model_free(embed);
    return rc == LISA_OK ? EXIT_OK : fail(rc, "serve", err);
}

static int cmd_gui(app_t* app, const args_t* a) {
    const char* err = NULL;
    lisa_model_t *embed, *chat;
    lisa_server_t* srv = NULL;
    lisa_stop_signals_install();
    int rc = start_server(app, a, 1, &embed, &chat, &srv, &err);
    if (rc == LISA_OK) {
        char url[256];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/#token=%s", server_port(srv), server_token(srv));
        int use_browser = a->browser || !gui_native_available();
        if (!use_browser) {
            fprintf(stderr, "LISA is open in its window. Close the window (or press Ctrl-C) to quit.\n");
            if (gui_show_window(url) != 0) {
                fprintf(stderr, "lisa: cannot open a window; using the browser\n");
                use_browser = 1;
            }
        }
        if (use_browser) {
            if (lisa_open_url(url) != LISA_PLAT_OK) fprintf(stderr, "lisa: open this address in a browser:\n");
            printf("LISA is at %s\nKeep this running while you use it; Ctrl-C quits.\n", url);
            fflush(stdout);
            while (!lisa_stop_requested()) lisa_sleep_ms(200);
        }
        server_stop(srv);
    }
    lisa_model_free(chat);
    lisa_model_free(embed);
    return rc == LISA_OK ? EXIT_OK : fail(rc, "gui", err);
}

/* ---- model ----------------------------------------------------------------- */

static int cmd_model(app_t* app, const args_t* a) {
    if (a->set) {
        lisa_known_model_t km;
        int vr = lisa_model_verify(a->set, &km, NULL);
        if (vr == LISA_E_NOT_FOUND) return fail(vr, "model", "no such file");
        int is_embedding;
        if (vr == LISA_OK) {
            is_embedding = km.is_embedding != 0;
        } else {
            /* Not a known file: load it to learn what it is. */
            lisa_model_t* m = NULL;
            int rc = lisa_model_load(app->ctx, a->set, NULL, &m);
            if (rc != LISA_OK) return fail(rc, "model", "not a usable GGUF model");
            lisa_model_info_t info = LISA_MODEL_INFO_INIT;
            lisa_model_info(m, &info);
            is_embedding = info.is_embedding != 0;
            lisa_model_free(m);
            fprintf(stderr, "lisa: warning: not a known model file (unverified); it loads\n");
        }
        app_model_kind kind = is_embedding ? APP_MODEL_EMBEDDING : APP_MODEL_CHAT;
        int rc = app_set_model(app, kind, a->set);
        if (rc != LISA_OK) return fail(rc, "model", "cannot write config.json");
        printf("Set %s model: %s\n", is_embedding ? "embedding" : "chat", app->config_model[kind]);
        return EXIT_OK;
    }

    yyjson_mut_doc* doc = a->json ? yyjson_mut_doc_new(NULL) : NULL;
    yyjson_mut_val* root = NULL;
    if (doc) {
        root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
    }
    int missing = 0;
    static const char* const label[2] = { "chat", "embedding" };
    for (int k = 0; k < 2; k++) {
        const char* source = NULL;
        char* path = app_find_model(app, (app_model_kind)k, &source);
        lisa_known_model_t km;
        char sha[65] = "";
        int vr = path ? lisa_model_verify(path, &km, sha) : LISA_E_NOT_FOUND;
        const char* status = vr == LISA_OK ? "verified" : vr == LISA_E_UNSUPPORTED ? "unverified (not a known file)"
                                                                                   : "missing";
        if (vr == LISA_E_NOT_FOUND) missing = 1;
        if (doc) {
            yyjson_mut_val* o = yyjson_mut_obj_add_obj(doc, root, label[k]);
            if (path) yyjson_mut_obj_add_strcpy(doc, o, "path", path);
            if (source) yyjson_mut_obj_add_str(doc, o, "source", source);
            yyjson_mut_obj_add_str(doc, o, "status", vr == LISA_OK ? "verified"
                                                     : vr == LISA_E_UNSUPPORTED ? "unverified" : "missing");
            if (vr == LISA_OK) {
                yyjson_mut_obj_add_strcpy(doc, o, "id", km.id);
                yyjson_mut_obj_add_strcpy(doc, o, "license", km.license);
            }
            if (sha[0]) yyjson_mut_obj_add_strcpy(doc, o, "sha256", sha);
        } else {
            printf("%-9s  %s\n", label[k], path ? path : "(none found)");
            if (path) {
                printf("           %s", status);
                if (vr == LISA_OK) printf(": %s, %s", km.id, km.license);
                printf("%s\n", source && strcmp(source, "config") == 0 ? "  [from config.json]" : "");
            }
        }
        free(path);
    }
    if (doc) print_json(doc);
    else if (missing) printf("\nSet a model with: lisa model --set <file.gguf>  (see docs/models.md)\n");
    return missing ? EXIT_NOT_FOUND : EXIT_OK;
}

/* ---- migrate ---------------------------------------------------------------- */

static int cmd_migrate(app_t* app, const args_t* a) {
    if (a->from == NULL || a->to == NULL) {
        fprintf(stderr, "lisa: migrate needs --from <v1-dir> --to <dir>\n");
        return EXIT_USAGE;
    }
    const char* model = a->model ? a->model : "unknown-v1";
    int rc = lisa_collection_migrate_v1(app->ctx, a->from, a->to, model);
    if (rc != LISA_OK) {
        return fail(rc, "migrate", rc == LISA_E_EXISTS ? "the destination already exists"
                                 : rc == LISA_E_NOT_FOUND ? "no v1 collection at --from" : NULL);
    }
    printf("Migrated %s -> %s (embedding model recorded as '%s'; vector search only)\n", a->from, a->to, model);
    return EXIT_OK;
}

/* ---- main ------------------------------------------------------------------- */

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "%s", k_usage);
        return EXIT_USAGE;
    }
    const char* cmd = argv[1];
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0 || strcmp(cmd, "version") == 0) {
        printf("lisa %s\n", lisa_version(NULL, NULL, NULL));
        printf("api %s, collection format %d, vector file format %d\n", LISA_VERSION_STRING,
               LISA_COLLECTION_FORMAT_VERSION, LISA_VECTOR_FILE_VERSION);
        printf("no telemetry; the only network socket is the local server\n");
        return EXIT_OK;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        printf("%s", k_usage);
        return EXIT_OK;
    }
    int (*fn)(app_t*, const args_t*) = NULL;
    if (strcmp(cmd, "ingest") == 0) fn = cmd_ingest;
    else if (strcmp(cmd, "search") == 0) fn = cmd_search;
    else if (strcmp(cmd, "ask") == 0) fn = cmd_ask;
    else if (strcmp(cmd, "serve") == 0) fn = cmd_serve;
    else if (strcmp(cmd, "model") == 0) fn = cmd_model;
    else if (strcmp(cmd, "migrate") == 0) fn = cmd_migrate;
    else if (strcmp(cmd, "gui") == 0) fn = cmd_gui;
    else {
        fprintf(stderr, "lisa: unknown command '%s'\n%s", cmd, k_usage);
        return EXIT_USAGE;
    }

    args_t a;
    int bad = parse_args(argc - 2, argv + 2, &a);
    if (bad) return bad;
    app_t app;
    const char* err = NULL;
    int rc = app_open(&app, a.data, &err);
    if (rc != LISA_OK) return fail(rc, "data directory", err);
    int level = a.log_level ? log_level_from_name(a.log_level) : LOG_INFO;
    if (level < 0) {
        fprintf(stderr, "lisa: --log-level must be error, warn, info or debug\n");
        app_close(&app);
        return EXIT_USAGE;
    }
    log_open(app.data_dir, (log_level_t)level);
    LOG_DEBUG("%s: data %s", cmd, app.data_dir);
    int code = fn(&app, &a);
    if (code != EXIT_OK) LOG_DEBUG("%s: exit %d", cmd, code);
    log_close();
    app_close(&app);
    return code;
}
