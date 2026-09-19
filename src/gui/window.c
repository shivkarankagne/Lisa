/* SPDX-License-Identifier: BUSL-1.1 */
/* Native window for `lisa gui` (gui.h), on webview/webview. */

#include "gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"
#include "../platform/platform.h"

#ifdef LISA_HAVE_WEBVIEW
#include "webview.h"

typedef struct {
    webview_t w;
    volatile int done;
} win_t;

/* JSON string literal for s (or null), malloc'd. */
static char* json_string(const char* s) {
    if (s == NULL) {
        char* n = (char*)malloc(5);
        if (n) memcpy(n, "null", 5);
        return n;
    }
    yyjson_mut_doc* d = yyjson_mut_doc_new(NULL);
    if (d == NULL) return NULL;
    yyjson_mut_doc_set_root(d, yyjson_mut_strcpy(d, s));
    char* out = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    return out;
}

/* window.lisaChooseFolder() -> Promise<string|null>; runs on the UI thread. */
static void choose_folder(const char* id, const char* req, void* arg) {
    (void)req;
    win_t* win = (win_t*)arg;
    char* path = lisa_choose_folder();
    char* js = json_string(path);
    webview_return(win->w, id, 0, js ? js : "null");
    free(js);
    free(path);
}

/* Files dropped on the window: hand their paths to the page. */
static void on_drop(void* user, const char* const* paths, int count) {
    win_t* win = (win_t*)user;
    yyjson_mut_doc* d = yyjson_mut_doc_new(NULL);
    if (d == NULL) return;
    yyjson_mut_val* arr = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, arr);
    for (int i = 0; i < count; i++) yyjson_mut_arr_add_strcpy(d, arr, paths[i]);
    char* list = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    if (list == NULL) return;
    size_t n = strlen(list) + 64;
    char* js = (char*)malloc(n);
    if (js) {
        snprintf(js, n, "window.lisaDropped && window.lisaDropped(%s);", list);
        webview_eval(win->w, js);
    }
    free(js);
    free(list);
}

/* Ctrl-C in the terminal closes the window. */
static void watch_stop(void* arg) {
    win_t* win = (win_t*)arg;
    while (!win->done) {
        if (lisa_stop_requested()) {
            webview_terminate(win->w);
            return;
        }
        lisa_sleep_ms(200);
    }
}

int gui_native_available(void) {
    return 1;
}

int gui_show_window(const char* url) {
    win_t win = { NULL, 0 };
    win.w = webview_create(0, NULL);
    if (win.w == NULL) return -1;
    webview_set_title(win.w, "LISA");
    webview_set_size(win.w, 1120, 780, WEBVIEW_HINT_NONE);
    webview_bind(win.w, "lisaChooseFolder", choose_folder, &win);
    void* view = webview_get_native_handle(win.w, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER);
    if (view) lisa_file_drops_install(view, on_drop, &win);
    webview_navigate(win.w, url);

    lisa_thread_t* watcher = NULL;
    lisa_thread_start(watch_stop, &win, &watcher);
    webview_run(win.w);
    win.done = 1;
    lisa_thread_join(watcher);
    webview_destroy(win.w);
    return 0;
}

#else

int gui_native_available(void) {
    return 0;
}

int gui_show_window(const char* url) {
    (void)url;
    return -1;
}

#endif
