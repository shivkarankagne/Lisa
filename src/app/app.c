/* SPDX-License-Identifier: BUSL-1.1 */
/* Shared program layer (app.h). */

#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"
#include "../platform/platform.h"

static const char* const k_kind_key[2] = { "chat", "embedding" };

static char* dup_str(const char* s) {
    if (s == NULL) return NULL;
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---- config.json ------------------------------------------------------ */

static int read_config(app_t* app, const char** err) {
    if (!lisa_path_exists(app->config_path)) return LISA_OK;
    yyjson_read_err e;
    yyjson_doc* doc = yyjson_read_file(app->config_path, 0, NULL, &e);
    if (doc == NULL) {
        *err = "config.json is not valid JSON";
        return LISA_E_FORMAT;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    yyjson_val* ver = yyjson_obj_get(root, "version");
    int rc = LISA_OK;
    if (!yyjson_is_obj(root) || !yyjson_is_int(ver)) {
        *err = "config.json has no version";
        rc = LISA_E_FORMAT;
    } else if (yyjson_get_int(ver) > APP_CONFIG_VERSION) {
        *err = "config.json is from a newer LISA";
        rc = LISA_E_FORMAT;
    }
    yyjson_val* models = yyjson_obj_get(root, "models");
    for (int k = 0; rc == LISA_OK && k < 2; k++) {
        const char* p = yyjson_get_str(yyjson_obj_get(models, k_kind_key[k]));
        if (p && p[0]) {
            app->config_model[k] = dup_str(p);
            if (app->config_model[k] == NULL) rc = LISA_E_NO_MEMORY;
        }
    }
    yyjson_doc_free(doc);
    return rc;
}

static int write_config(const app_t* app) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) return LISA_E_NO_MEMORY;
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "version", APP_CONFIG_VERSION);
    yyjson_mut_val* models = yyjson_mut_obj_add_obj(doc, root, "models");
    for (int k = 0; k < 2; k++) {
        if (app->config_model[k]) yyjson_mut_obj_add_str(doc, models, k_kind_key[k], app->config_model[k]);
    }
    size_t len = 0;
    char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
    yyjson_mut_doc_free(doc);
    if (json == NULL) return LISA_E_NO_MEMORY;

    /* Write a temporary file, sync, rename over the old one. */
    size_t n = strlen(app->config_path) + 5;
    char* tmp = (char*)malloc(n);
    int rc = tmp ? LISA_OK : LISA_E_NO_MEMORY;
    FILE* f = NULL;
    if (rc == LISA_OK) {
        snprintf(tmp, n, "%s.tmp", app->config_path);
        f = fopen(tmp, "wb");
        if (f == NULL) rc = LISA_E_IO;
    }
    if (rc == LISA_OK && (fwrite(json, 1, len, f) != len || fputc('\n', f) == EOF ||
                          lisa_file_sync(f) != LISA_PLAT_OK))
        rc = LISA_E_IO;
    if (f && fclose(f) != 0) rc = LISA_E_IO;
    if (rc == LISA_OK && lisa_rename_replace(tmp, app->config_path) != LISA_PLAT_OK) rc = LISA_E_IO;
    if (rc != LISA_OK && tmp) lisa_remove_file(tmp);
    free(tmp);
    free(json);
    return rc;
}

/* ---- open / close ----------------------------------------------------- */

int app_open(app_t* app, const char* data_dir, const char** err) {
    static const char* none = "";
    const char* dummy;
    if (err == NULL) err = &dummy;
    *err = none;
    memset(app, 0, sizeof(*app));
    app->data_dir = data_dir ? dup_str(data_dir) : lisa_default_data_dir();
    if (app->data_dir == NULL) {
        *err = "cannot determine the data directory; pass --data";
        return LISA_E_INVALID_ARGUMENT;
    }
    if (lisa_mkdirs(app->data_dir) != LISA_PLAT_OK) {
        *err = "cannot create the data directory";
        app_close(app);
        return LISA_E_IO;
    }
    app->collections_dir = lisa_path_join(app->data_dir, "collections");
    app->config_path = lisa_path_join(app->data_dir, "config.json");
    if (app->collections_dir == NULL || app->config_path == NULL) {
        app_close(app);
        return LISA_E_NO_MEMORY;
    }
    if (lisa_mkdirs(app->collections_dir) != LISA_PLAT_OK) {
        *err = "cannot create the collections directory";
        app_close(app);
        return LISA_E_IO;
    }
    int rc = read_config(app, err);
    if (rc == LISA_OK) {
        rc = lisa_context_create(NULL, &app->ctx);
        if (rc != LISA_OK) *err = "cannot create the LISA context";
    }
    if (rc != LISA_OK) app_close(app);
    return rc;
}

void app_close(app_t* app) {
    if (app == NULL) return;
    lisa_context_destroy(app->ctx);
    free(app->data_dir);
    free(app->collections_dir);
    free(app->config_path);
    free(app->config_model[0]);
    free(app->config_model[1]);
    memset(app, 0, sizeof(*app));
}

/* ---- collections ------------------------------------------------------ */

int app_valid_name(const char* name) {
    if (name == NULL) return 0;
    size_t n = strlen(name);
    if (n == 0 || n > APP_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

char* app_collection_path(const app_t* app, const char* name) {
    if (!app_valid_name(name)) return NULL;
    return lisa_path_join(app->collections_dir, name);
}

typedef struct {
    app_name_fn fn;
    void*       user;
} list_ctx_t;

static int on_entry(void* user, const char* name, int is_dir) {
    list_ctx_t* l = (list_ctx_t*)user;
    if (!is_dir || !app_valid_name(name)) return 0;
    return l->fn(l->user, name);
}

int app_list_collections(const app_t* app, app_name_fn fn, void* user) {
    list_ctx_t l = { fn, user };
    int rc = lisa_dir_list(app->collections_dir, on_entry, &l);
    return rc < 0 ? LISA_E_IO : rc;
}

/* ---- models ----------------------------------------------------------- */

static char* search_known(const char* dir, int want_embedding) {
    if (dir == NULL) return NULL;
    int64_t n = lisa_known_model_count();
    for (int64_t i = 0; i < n; i++) {
        lisa_known_model_t m;
        if (lisa_known_model(i, &m) != LISA_OK || (m.is_embedding != 0) != (want_embedding != 0)) continue;
        char* p = lisa_path_join(dir, m.file_name);
        if (p && lisa_path_exists(p)) return p;
        free(p);
    }
    return NULL;
}

static char* parent_dir(const char* path) {
    char* d = dup_str(path);
    if (d == NULL) return NULL;
    char* slash = strrchr(d, '/');
    if (slash == NULL || slash == d) {
        free(d);
        return NULL;
    }
    *slash = '\0';
    return d;
}

char* app_find_model(const app_t* app, app_model_kind kind, const char** source) {
    if (source) *source = NULL;
    if (app->config_model[kind]) {
        if (source) *source = "config";
        return dup_str(app->config_model[kind]);
    }
    int emb = kind == APP_MODEL_EMBEDDING;
    char* exe = lisa_executable_path();
    char* exe_dir = exe ? parent_dir(exe) : NULL;
    char* up = exe_dir ? parent_dir(exe_dir) : NULL;
    char* dirs[3] = { lisa_path_join(app->data_dir, "models"),
                      exe_dir ? lisa_path_join(exe_dir, "models") : NULL,
                      up ? lisa_path_join(up, "models") : NULL };
    char* found = NULL;
    for (int i = 0; i < 3 && found == NULL; i++) found = search_known(dirs[i], emb);
    for (int i = 0; i < 3; i++) free(dirs[i]);
    free(exe);
    free(exe_dir);
    free(up);
    if (found && source) *source = "search";
    return found;
}

int app_set_model(app_t* app, app_model_kind kind, const char* path) {
    char* abs = lisa_path_absolute(path);
    if (abs == NULL) return LISA_E_NOT_FOUND;
    char* old = app->config_model[kind];
    app->config_model[kind] = abs;
    int rc = write_config(app);
    if (rc != LISA_OK) {
        app->config_model[kind] = old;
        free(abs);
        return rc;
    }
    free(old);
    return LISA_OK;
}

int app_load_model(const app_t* app, app_model_kind kind, lisa_model_t** out, const char** err) {
    const char* dummy;
    if (err == NULL) err = &dummy;
    *out = NULL;
    char* path = app_find_model(app, kind, NULL);
    if (path == NULL) {
        *err = kind == APP_MODEL_CHAT
                   ? "no chat model found; run `lisa model --set <file.gguf>` (see docs/models.md)"
                   : "no embedding model found; run `lisa model --set <file.gguf>` (see docs/models.md)";
        return LISA_E_NOT_FOUND;
    }
    int rc = lisa_model_load(app->ctx, path, NULL, out);
    free(path);
    if (rc != LISA_OK) {
        *err = "cannot load the model file";
        return rc;
    }
    lisa_model_info_t info = LISA_MODEL_INFO_INIT;
    lisa_model_info(*out, &info);
    if ((info.is_embedding != 0) != (kind == APP_MODEL_EMBEDDING)) {
        *err = kind == APP_MODEL_CHAT ? "the configured chat model is an embedding model"
                                      : "the configured embedding model is not an embedding model";
        lisa_model_free(*out);
        *out = NULL;
        return LISA_E_WRONG_MODEL_KIND;
    }
    return LISA_OK;
}
