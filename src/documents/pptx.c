/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * PowerPoint (.pptx) extractor. Slides are read in presentation order
 * (ppt/presentation.xml <p:sldIdLst> through its relationships; falling back
 * to slide file numbers), each slide is one page, and its speaker notes are
 * appended to it ("Notes: ..."), since notes often hold what the slide only
 * hints at. Text is <a:t> runs; </a:p> ends a paragraph; slide-number fields
 * (<a:fld>) are skipped. Table rows read as "cell | cell" lines, so a value
 * stays next to what it is.
 */

#include "ooxml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SLIDES 2000

/* Text of one slide or notes part. */
static void scan_slide(const char* x, sbuf_t* out) {
    const char* p = x;
    const char* e = x + strlen(x);
    int in_field = 0, in_cell = 0, cells_in_row = 0;
    while (p < e) {
        const char* lt = memchr(p, '<', (size_t)(e - p));
        if (lt == NULL) break;
        const char* gt = memchr(lt, '>', (size_t)(e - lt));
        if (gt == NULL) break;
        int closing = lt[1] == '/';
        const char* name = lt + (closing ? 2 : 1);
        size_t n = xml_tag_name(name, gt);
        int self_closing = gt[-1] == '/';
        if (!closing && !self_closing && xml_is(name, n, "a:t")) {
            const char* end = strstr(gt + 1, "</a:t>");
            if (end == NULL) break;
            if (!in_field) sbuf_put_xml_text(out, gt + 1, end);
            p = end + 6;
            continue;
        }
        if (!closing && !self_closing && xml_is(name, n, "a:fld")) in_field = 1;
        else if (closing && xml_is(name, n, "a:fld")) in_field = 0;
        else if (!closing && xml_is(name, n, "a:tr")) cells_in_row = 0;
        else if (!closing && !self_closing && xml_is(name, n, "a:tc")) {
            if (cells_in_row++ > 0) sbuf_puts(out, " | ");
            in_cell = 1;
        } else if (closing && xml_is(name, n, "a:tc")) {
            sbuf_trim_end(out);
            in_cell = 0;
        } else if (closing && xml_is(name, n, "a:tr")) sbuf_put(out, "\n", 1);
        else if (closing && xml_is(name, n, "a:p")) {
            if (in_cell) sbuf_put(out, " ", 1);
            else sbuf_put(out, "\n", 1);
        }
        else if (!closing && xml_is(name, n, "a:br")) sbuf_put(out, "\n", 1);
        else if (!closing && xml_is(name, n, "a:tab")) sbuf_put(out, "\t", 1);
        p = gt + 1;
    }
}

/* Slide part names in presentation order; *n set. Falls back to
 * ppt/slides/slide1.xml, slide2.xml, ... when presentation.xml can't be read. */
static char** slide_order(mz_zip_archive* z, int* n) {
    *n = 0;
    char** out = (char**)calloc(MAX_SLIDES, sizeof(char*));
    if (out == NULL) return NULL;
    char* pres = ooxml_part(z, "ppt/presentation.xml", NULL, NULL);
    char* rels = ooxml_part(z, "ppt/_rels/presentation.xml.rels", NULL, NULL);
    if (pres && rels) {
        const char* p = strstr(pres, "<p:sldIdLst");
        const char* end = p ? strstr(p, "</p:sldIdLst>") : NULL;
        while (p && end && *n < MAX_SLIDES && (p = strstr(p + 1, "<p:sldId ")) != NULL && p < end) {
            const char* gt = strchr(p, '>');
            if (gt == NULL) break;
            char* rid = xml_attr(p, gt, "r:id");
            if (rid) {
                char* t = ooxml_rel_target(rels, rid, "ppt/");
                if (t) out[(*n)++] = t;
                free(rid);
            }
        }
    }
    free(pres);
    free(rels);
    if (*n == 0) {
        for (int i = 1; i <= MAX_SLIDES; i++) {
            char name[64];
            snprintf(name, sizeof(name), "ppt/slides/slide%d.xml", i);
            if (mz_zip_reader_locate_file(z, name, NULL, 0) < 0) break;
            out[(*n)++] = strdup(name);
        }
    }
    return out;
}

/* The notes part for a slide (from the slide's relationships), or NULL. */
static char* notes_for(mz_zip_archive* z, const char* slide) {
    const char* slash = strrchr(slide, '/');
    if (slash == NULL) return NULL;
    char rels_name[600], dir[512];
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - slide + 1), slide);
    snprintf(rels_name, sizeof(rels_name), "%s_rels/%s.rels", dir, slash + 1);
    char* rels = ooxml_part(z, rels_name, NULL, NULL);
    if (rels == NULL) return NULL;
    char* notes = NULL;
    for (const char* p = rels; (p = strstr(p, "<Relationship ")) != NULL; p++) {
        const char* gt = strchr(p, '>');
        if (gt == NULL) break;
        char* type = xml_attr(p, gt, "Type");
        int is_notes = type && strstr(type, "/notesSlide") != NULL;
        free(type);
        if (is_notes) {
            char* id = xml_attr(p, gt, "Id");
            char* target = id ? ooxml_rel_target(rels, id, dir) : NULL;
            free(id);
            if (target) notes = ooxml_part(z, target, NULL, NULL);
            free(target);
            break;
        }
    }
    free(rels);
    return notes;
}

int doc_extract_pptx(const char* path, doc_text_t* out) {
    mz_zip_archive z;
    int rc = ooxml_open(path, &z);
    if (rc != DOC_OK) return rc;
    int n = 0;
    char** slides = slide_order(&z, &n);
    if (slides == NULL || n == 0) {
        free(slides);
        mz_zip_reader_end(&z);
        return slides == NULL ? DOC_ENOMEM : DOC_EFORMAT;
    }
    pages_t pg = { 0 };
    for (int i = 0; i < n; i++) {
        int too_big = 0;
        char* x = ooxml_part(&z, slides[i], NULL, &too_big);
        sbuf_t b = { 0 };
        sbuf_put(&b, "", 0);
        if (x) scan_slide(x, &b);
        free(x);
        char* notes = notes_for(&z, slides[i]);
        if (notes) {
            sbuf_t nb = { 0 };
            sbuf_put(&nb, "", 0);
            scan_slide(notes, &nb);
            if (nb.len > 0) {
                sbuf_puts(&b, "\nNotes: ");
                sbuf_put(&b, nb.data, nb.len);
            }
            free(nb.data);
            free(notes);
        }
        if (b.oom) pg.rc = DOC_ENOMEM;
        pages_add(&pg, b.data, b.len);   /* an unreadable slide keeps its page number */
        free(b.data);
        free(slides[i]);
    }
    free(slides);
    char* title = ooxml_title(&z);
    mz_zip_reader_end(&z);
    return pages_finish(&pg, out, title);
}
