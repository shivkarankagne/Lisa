/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * CSV / TSV extractor. RFC 4180 quoting (delimiters and line breaks inside
 * quotes, "" for a quote); the delimiter is ',' ';' or tab, picked from the
 * first line (always tab for .tsv). If the first row is all text it is the
 * header and every later row is written "Header: value | Header: value", so
 * a chunk from the middle of the file still says what each value means. A
 * blank line every ROWS_PER_PARA rows gives the chunker natural breaks.
 * Bounded by DOC_MAX_FILE_BYTES and MAX_ROWS / MAX_COLS.
 */

#include "ooxml.h"

#include <stdlib.h>
#include <string.h>

#define MAX_ROWS      200000
#define MAX_COLS      512
#define ROWS_PER_PARA 10

static char pick_delim(const char* s, int tsv) {
    if (tsv) return '\t';
    int comma = 0, semi = 0, tab = 0, inq = 0;
    for (; *s && (inq || *s != '\n'); s++) {
        if (*s == '"') inq = !inq;
        else if (!inq && *s == ',') comma++;
        else if (!inq && *s == ';') semi++;
        else if (!inq && *s == '\t') tab++;
    }
    if (tab > comma && tab > semi) return '\t';
    return semi > comma ? ';' : ',';
}

static int is_number(const char* s) {
    if (*s == '\0') return 0;
    char* end = NULL;
    strtod(s, &end);
    return end && *end == '\0';
}

/* Parse one record starting at *p into fields (malloc'd); returns count. */
static int record(const char** p, char delim, char** f) {
    int n = 0;
    const char* s = *p;
    for (;;) {
        sbuf_t b = { 0 };
        sbuf_put(&b, "", 0);
        if (*s == '"') {
            s++;
            for (; *s; s++) {
                if (*s == '"' && s[1] == '"') {
                    sbuf_put(&b, "\"", 1);
                    s++;
                } else if (*s == '"') {
                    s++;
                    break;
                } else {
                    sbuf_put(&b, s, 1);
                }
            }
        }
        while (*s && *s != delim && *s != '\n') sbuf_put(&b, s++, 1);
        /* trim the field's surrounding spaces; newlines inside quotes become spaces */
        char* d = b.data;
        for (char* c = d; c && *c; c++)
            if (*c == '\n' || *c == '\t') *c = ' ';
        while (d && *d == ' ') d++;
        int64_t len = d ? (int64_t)strlen(d) : 0;
        while (len > 0 && d[len - 1] == ' ') d[--len] = '\0';
        if (n < MAX_COLS && d) {
            f[n] = (char*)malloc((size_t)len + 1);
            if (f[n]) {
                memcpy(f[n], d, (size_t)len + 1);
                n++;
            }
        }
        free(b.data);
        if (*s == delim) {
            s++;
            continue;
        }
        if (*s == '\n') s++;
        break;
    }
    *p = s;
    return n;
}

int doc_extract_csv(const char* path, doc_text_t* out) {
    char* raw = NULL;
    int64_t raw_len = 0;
    int rc = doc_read_file(path, &raw, &raw_len);
    if (rc != DOC_OK) return rc;
    char* text = NULL;
    int64_t len = 0;
    rc = doc_normalize(raw, raw_len, &text, &len);   /* encoding -> UTF-8, "\n" endings */
    free(raw);
    if (rc != DOC_OK) return rc;

    const char* dot = strrchr(path, '.');
    int tsv = dot && (dot[1] == 't' || dot[1] == 'T');
    char delim = pick_delim(text, tsv);
    char* header[MAX_COLS] = { 0 };
    int nheader = 0, rows = 0, decided = 0;
    sbuf_t b = { 0 };
    sbuf_put(&b, "", 0);
    for (const char* p = text; *p && rows < MAX_ROWS; rows++) {
        char* f[MAX_COLS];
        int n = record(&p, delim, f);
        int nonempty = 0, all_text = 1;
        for (int i = 0; i < n; i++) {
            if (f[i][0]) {
                nonempty++;
                if (is_number(f[i])) all_text = 0;
            }
        }
        if (nonempty > 0 && !decided) {
            decided = 1;
            if (all_text && nonempty > 1) {
                for (int i = 0; i < n; i++) header[i] = f[i];
                nheader = n;
                for (int i = 0, first = 1; i < n; i++) {
                    if (!header[i][0]) continue;
                    sbuf_puts(&b, first ? "" : " | ");
                    sbuf_puts(&b, header[i]);
                    first = 0;
                }
                sbuf_puts(&b, "\n\n");
                continue;
            }
        }
        if (nonempty > 0) {
            for (int i = 0, first = 1; i < n; i++) {
                if (!f[i][0]) continue;
                sbuf_puts(&b, first ? "" : " | ");
                if (i < nheader && header[i][0]) {
                    sbuf_puts(&b, header[i]);
                    sbuf_puts(&b, ": ");
                }
                sbuf_puts(&b, f[i]);
                first = 0;
            }
            sbuf_puts(&b, rows % ROWS_PER_PARA == 0 ? "\n\n" : "\n");
        }
        for (int i = 0; i < n; i++) free(f[i]);
    }
    for (int i = 0; i < nheader; i++) free(header[i]);
    free(text);
    if (b.oom) {
        free(b.data);
        return DOC_ENOMEM;
    }
    rc = doc_normalize(b.data, b.len, &out->text, &out->len);
    free(b.data);
    if (rc != DOC_OK) return rc;
    out->n_pages = 0;
    out->page_offsets = NULL;
    out->title = NULL;
    return DOC_OK;
}
