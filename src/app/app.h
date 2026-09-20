/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_APP_H
#define LISA_APP_H
/*
 * What the lisa program (CLI and server) shares: the data directory, the
 * config file, collection names, and finding and loading the models.
 * LISA functionality is used only through include/lisa.h; OS services
 * through src/platform.
 *
 * Data directory layout:
 *
 *     <data>/config.json            {"version": 1, "models": {...}, "watch": {...}}
 *     <data>/collections/<name>/    one collection per name
 *     <data>/models/                a place to put model files (searched)
 *
 * Model files are found, in order: the path in config.json; then a known
 * model file (lisa_known_model) in <data>/models, <exe dir>/models, or
 * <exe dir>/../models.
 */

#include <stdint.h>

#include "lisa.h"

#define APP_CONFIG_VERSION 1
#define APP_NAME_MAX       64
#define APP_MAX_WATCHED    16

typedef enum { APP_MODEL_CHAT = 0, APP_MODEL_EMBEDDING = 1 } app_model_kind;

/*
 * The folders LISA keeps indexed while it is running, and the collection
 * they go into ("watch" in config.json). Chosen once, in the GUI's first
 * run or with `lisa watch --add`.
 */
typedef struct {
    char*   collection;                 /* NULL: nothing watched yet */
    char*   folders[APP_MAX_WATCHED];
    int     n_folders;
} app_watch_t;

typedef struct {
    lisa_context_t* ctx;              /* owned */
    char*           data_dir;         /* absolute or as given; created */
    char*           collections_dir;
    char*           config_path;
    char*           config_model[2];  /* from config.json; NULL if unset */
    app_watch_t     watch;
} app_t;

/*
 * Open the data directory (NULL: the platform default), creating it and
 * collections/ if needed, and read config.json if present. Returns a
 * lisa status; *err (static text) says what failed.
 */
int  app_open(app_t* app, const char* data_dir, const char** err);
void app_close(app_t* app);

/* 1 if name is 1..APP_NAME_MAX characters of [A-Za-z0-9_-]. */
int app_valid_name(const char* name);

/* <collections>/<name>, malloc'd; NULL if the name is invalid. */
char* app_collection_path(const app_t* app, const char* name);

/* Call fn for each collection directory name, sorted. */
typedef int (*app_name_fn)(void* user, const char* name);
int app_list_collections(const app_t* app, app_name_fn fn, void* user);

/*
 * Path of the model of this kind (malloc'd), or NULL if none is
 * configured or found. *source (may be NULL) gets "config" or "search".
 */
char* app_find_model(const app_t* app, app_model_kind kind, const char** source);

/* Record path as the model of this kind in config.json (atomic write). */
int app_set_model(app_t* app, app_model_kind kind, const char* path);

/*
 * Replace the watched folders (absolute paths that exist) and the
 * collection they are indexed into, and save config.json. count 0 stops
 * watching.
 */
int app_set_watch(app_t* app, const char* collection, const char* const* folders, int count);

/* The folders a new user should be offered: Documents, Desktop, Downloads
 * that exist. Fills up to `cap` malloc'd paths; returns how many. */
int app_default_watch_folders(char** out, int cap);

/* Find and load the model of this kind. *err explains a failure. */
int app_load_model(const app_t* app, app_model_kind kind, lisa_model_t** out, const char** err);

#endif /* LISA_APP_H */
