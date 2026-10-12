/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Excel (.xlsx) extractor. Each worksheet is one page ("Sheet: <name>").
 * Cells are placed by their column letter (gaps stay empty, so columns line
 * up). If the first non-empty row is all text it is taken as the header and
 * every later row is written as "Header: value | Header: value", so a chunk
 * cut from the middle of a large table still says what each number means.
 * Strings come from xl/sharedStrings.xml (rich-text runs joined, phonetic
 * guides skipped), inline strings, or the cached value <v>. Bounded: rows,
 * columns and sheets are capped.
 */

#include "ooxml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SHEETS  256
#define MAX_ROWS    100000
#define MAX_COLS    512
#define MAX_STRINGS 2000000

typedef struct {
    char**  s;
    int64_t n;
} strings_t;

static void strings_free(strings_t* t) {
    for (int64_t i = 0; i < t->n; i++) free(t->s[i]);
    free(t->s);
}

/* Shared strings: one entry per <si>, all its <t> joined (not <rPh>). */
static int read_strings(mz_zip_archive* z, strings_t* out) {
    memset(out, 0, sizeof(*out));
    char* x = ooxml_part(z, "xl/sharedStrings.xml", NULL, NULL);
    if (x == NULL) return DOC_OK;
    int64_t cap = 0;
    int rc = DOC_OK;
    for (const char* p = x; rc == DOC_OK && (p = strstr(p, "<si")) != NULL;) {
        if (p[3] != '>' && p[3] != ' ') {
            p += 3;
            continue;
        }
        const char* end = strstr(p, "</si>");
        if (end == NULL) break;
        /* One pass over the item's tags: text runs count unless inside a
         * phonetic guide (<rPh>). */
        sbuf_t b = { 0 };
        sbuf_put(&b, "", 0);
        int in_rph = 0;
        for (const char* q = p + 3; q < end;) {
            const char* lt = memchr(q, '<', (size_t)(end - q));
            if (lt == NULL) break;
            const char* gt = memchr(lt, '>', (size_t)(end - lt));
            if (gt == NULL) break;
            int closing = lt[1] == '/';
            const char* nm = lt + (closing ? 2 : 1);
            size_t n = xml_tag_name(nm, gt);
            if (xml_is(nm, n, "rPh")) in_rph = !closing && gt[-1] != '/';
            else if (!closing && xml_is(nm, n, "t") && gt[-1] != '/') {
                const char* te = strstr(gt, "</t>");
                if (te == NULL || te > end) break;
                if (!in_rph) sbuf_put_xml_text(&b, gt + 1, te);
                q = te + 4;
                continue;
            }
            q = gt + 1;
        }
        if (out->n == cap) {
            int64_t nc = cap ? cap * 2 : 1024;
            if (nc > MAX_STRINGS) nc = MAX_STRINGS;
            char** s = out->n < MAX_STRINGS ? (char**)realloc(out->s, (size_t)nc * sizeof(char*)) : NULL;
            if (s == NULL) {
                free(b.data);
                rc = out->n < MAX_STRINGS ? DOC_ENOMEM : DOC_ETOOBIG;
                break;
            }
            out->s = s;
            cap = nc;
        }
        if (b.oom) rc = DOC_ENOMEM;
        out->s[out->n++] = b.data;
        p = end + 5;
    }
    free(x);
    return rc;
}

/* "BC12" -> column index 54 (0-based), or -1. */
static int col_of(const char* ref) {
    int c = 0, any = 0;
    for (; *ref >= 'A' && *ref <= 'Z'; ref++) {
        c = c * 26 + (*ref - 'A' + 1);
        any = 1;
        if (c > MAX_COLS) return -1;
    }
    return any ? c - 1 : -1;
}

static int is_number(const char* s) {
    if (s == NULL || *s == '\0') return 0;
    char* end = NULL;
    strtod(s, &end);
    return end && *end == '\0';
}

/* Write one row: labeled with the header when there is one. */
static void put_row(sbuf_t* b, char** cells, int ncols, char** header, int nheader) {
    int first = 1;
    for (int c = 0; c < ncols; c++) {
        if (cells[c] == NULL || cells[c][0] == '\0') continue;
        if (!first) sbuf_puts(b, " | ");
        first = 0;
        if (header && c < nheader && header[c] && header[c][0]) {
            sbuf_puts(b, header[c]);
            sbuf_puts(b, ": ");
        }
        sbuf_puts(b, cells[c]);
    }
    if (!first) sbuf_put(b, "\n", 1);
}

static void free_cells(char** cells, int n) {
    for (int i = 0; i < n; i++) {
        free(cells[i]);
        cells[i] = NULL;
    }
}

/* Text of one worksheet. */
static void scan_sheet(const char* x, const strings_t* ss, sbuf_t* out) {
    char* cells[MAX_COLS] = { 0 };
    char* header[MAX_COLS] = { 0 };
    int nheader = 0, have_header = 0, rows = 0;
    for (const char* p = x; (p = strstr(p, "<row")) != NULL && rows < MAX_ROWS; rows++) {
        if (p[4] != ' ' && p[4] != '>') {
            p += 4;
            continue;
        }
        const char* rend = strstr(p, "</row>");
        const char* rgt = strchr(p, '>');
        if (rgt == NULL) break;
        if (rgt[-1] == '/') { /* empty row */
            p = rgt + 1;
            continue;
        }
        if (rend == NULL) break;
        int ncols = 0, next = 0;
        for (const char* c = rgt; (c = strstr(c, "<c")) != NULL && c < rend;) {
            if (c[2] != ' ' && c[2] != '>') {
                c += 2;
                continue;
            }
            const char* cgt = strchr(c, '>');
            if (cgt == NULL || cgt > rend) break;
            char* ref = xml_attr(c, cgt, "r");
            char* type = xml_attr(c, cgt, "t");
            int col = ref ? col_of(ref) : next;
            free(ref);
            sbuf_t v = { 0 };
            sbuf_put(&v, "", 0);
            const char* cend = cgt[-1] == '/' ? cgt + 1 : strstr(cgt, "</c>");
            if (cend == NULL || cend > rend) cend = rend;
            if (cgt[-1] != '/') {
                if (type && strcmp(type, "inlineStr") == 0) {
                    for (const char* t = cgt; (t = strstr(t, "<t")) != NULL && t < cend;) {
                        const char* tgt = strchr(t, '>');
                        const char* te = tgt ? strstr(tgt, "</t>") : NULL;
                        if (tgt == NULL || te == NULL || te > cend) break;
                        if (tgt[-1] != '/') sbuf_put_xml_text(&v, tgt + 1, te);
                        t = te + 4;
                    }
                } else {
                    const char* vs = strstr(cgt, "<v>");
                    const char* ve = vs ? strstr(vs, "</v>") : NULL;
                    if (vs && ve && ve < cend) {
                        if (type && strcmp(type, "s") == 0) {
                            long i = strtol(vs + 3, NULL, 10);
                            if (i >= 0 && i < ss->n && ss->s[i]) sbuf_puts(&v, ss->s[i]);
                        } else if (type && strcmp(type, "b") == 0) {
                            sbuf_puts(&v, vs[3] == '1' ? "TRUE" : "FALSE");
                        } else {
                            sbuf_put_xml_text(&v, vs + 3, ve);
                        }
                    }
                }
            }
            free(type);
            if (col >= 0 && col < MAX_COLS && !v.oom) {
                free(cells[col]);
                cells[col] = v.data;
                if (col + 1 > ncols) ncols = col + 1;
                next = col + 1;
            } else {
                free(v.data);
            }
            c = cend;
        }
        int nonempty = 0, all_text = 1;
        for (int i = 0; i < ncols; i++) {
            if (cells[i] && cells[i][0]) {
                nonempty++;
                if (is_number(cells[i])) all_text = 0;
            }
        }
        if (nonempty > 0) {
            if (!have_header && all_text && nonempty > 1) {
                for (int i = 0; i < ncols; i++) {
                    header[i] = cells[i];
                    cells[i] = NULL;
                }
                nheader = ncols;
                have_header = 1;
                put_row(out, header, nheader, NULL, 0); /* the header itself, once */
            } else {
                have_header = 1; /* first data row decides: no header after it */
                put_row(out, cells, ncols, nheader ? header : NULL, nheader);
                if (rows % 10 == 0) sbuf_put(out, "\n", 1); /* paragraph break for the chunker */
            }
        }
        free_cells(cells, ncols);
        p = rend + 6;
    }
    free_cells(header, nheader);
}

int doc_extract_xlsx(const char* path, doc_text_t* out) {
    mz_zip_archive z;
    int rc = ooxml_open(path, &z);
    if (rc != DOC_OK) return rc;
    char* wb = ooxml_part(&z, "xl/workbook.xml", NULL, NULL);
    char* rels = ooxml_part(&z, "xl/_rels/workbook.xml.rels", NULL, NULL);
    if (wb == NULL || rels == NULL) {
        free(wb);
        free(rels);
        mz_zip_reader_end(&z);
        return DOC_EFORMAT;
    }
    strings_t ss;
    rc = read_strings(&z, &ss);
    pages_t pg = { 0 };
    pg.rc = rc;
    int sheets = 0;
    for (const char* p = wb; pg.rc == DOC_OK && sheets < MAX_SHEETS && (p = strstr(p, "<sheet ")) != NULL; sheets++) {
        const char* gt = strchr(p, '>');
        if (gt == NULL) break;
        char* name = xml_attr(p, gt, "name");
        char* rid = xml_attr(p, gt, "r:id");
        char* target = rid ? ooxml_rel_target(rels, rid, "xl/") : NULL;
        int too_big = 0;
        char* x = target ? ooxml_part(&z, target, NULL, &too_big) : NULL;
        sbuf_t b = { 0 };
        sbuf_puts(&b, "Sheet: ");
        sbuf_puts(&b, name ? name : "");
        sbuf_puts(&b, "\n\n");
        if (x) scan_sheet(x, &ss, &b);
        if (b.oom) pg.rc = DOC_ENOMEM;
        pages_add(&pg, b.data, b.len);
        free(b.data);
        free(x);
        free(target);
        free(rid);
        free(name);
        p = gt + 1;
    }
    strings_free(&ss);
    free(wb);
    free(rels);
    char* title = ooxml_title(&z);
    mz_zip_reader_end(&z);
    if (pg.rc == DOC_OK && sheets == 0) {
        free(title);
        free(pg.text);
        free(pg.offsets);
        return DOC_EFORMAT;
    }
    return pages_finish(&pg, out, title);
}
