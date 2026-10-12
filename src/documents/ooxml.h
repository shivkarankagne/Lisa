/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_OOXML_H
#define LISA_OOXML_H

/*
 * Shared by the Office Open XML extractors (.docx, .pptx, .xlsx): growable
 * text buffer, a small streaming XML scanner, zip part access, and a
 * builder for paged text (slides, sheets). Not a full XML parser — just
 * enough to read text out of the parts we know. Internal to src/documents/.
 */

#include <stdint.h>

#include "doc_internal.h"
#include "miniz.h"

#define OOXML_MAX_PART_BYTES (256LL * 1024 * 1024) /* also guards against zip bombs */

typedef struct {
    char*   data;
    int64_t len, cap;
    int     oom;
} sbuf_t;

void sbuf_put(sbuf_t* b, const char* s, int64_t n);
void sbuf_puts(sbuf_t* b, const char* s);
/* Append XML character data [s, e) with entities decoded. */
void sbuf_put_xml_text(sbuf_t* b, const char* s, const char* e);
/* Drop trailing spaces and tabs. */
void sbuf_trim_end(sbuf_t* b);

/* Tag name at p (just after '<' or "</"), e.g. "w:t"; returns its length. */
size_t xml_tag_name(const char* p, const char* e);
int    xml_is(const char* p, size_t n, const char* name);
/* Value of attribute `name` within the tag [lt, gt), entity-decoded, malloc'd;
 * NULL if absent. */
char* xml_attr(const char* lt, const char* gt, const char* name);

/* Open an OOXML zip: DOC_OK, or DOC_EIO / DOC_EENCRYPTED (an encrypted
 * Office file is an OLE compound file, not a zip) / DOC_EFORMAT / limits. */
int ooxml_open(const char* path, mz_zip_archive* z);
/* Extract one part, NUL-terminated; NULL if missing (*too_big set if over
 * the limit). */
char* ooxml_part(mz_zip_archive* z, const char* name, int64_t* out_len, int* too_big);
/* <dc:title> from docProps/core.xml, or NULL. */
char* ooxml_title(mz_zip_archive* z);
/* Target of relationship `id` in a .rels part, resolved against base_dir
 * ("ppt/"), malloc'd; NULL if absent. */
char* ooxml_rel_target(const char* rels, const char* id, const char* base_dir);

/* Paged text: each page is normalized, pages are joined by a blank line, and
 * page_offsets records where each starts (a slide or sheet is a "page", so a
 * citation can say "deck.pptx p.3"). */
typedef struct {
    char*    text;
    int64_t  len, cap;
    int64_t* offsets;
    int64_t  n, ncap;
    int      rc;
} pages_t;

void pages_add(pages_t* p, const char* raw, int64_t raw_len);
/* Move the result into out (title owned by out); DOC_OK or the first error. */
int  pages_finish(pages_t* p, doc_text_t* out, char* title);

#endif /* LISA_OOXML_H */
