/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Word (.docx) extractor. A .docx is a zip of XML parts; the text is in
 * word/document.xml and the title in docProps/core.xml. Reading is a
 * small streaming scan, not a full XML parser:
 *
 *   <w:t>...</w:t>        text (entities decoded)
 *   <w:tab/>              tab
 *   <w:br/>, <w:cr/>      line break
 *   </w:p>                paragraph end (blank line; a space inside a table cell)
 *   </w:tc>, </w:tr>      table cell (tab) and row (line) ends; a table ends a paragraph
 *   <w:delText>           tracked deletion: skipped (not a <w:t>)
 *
 * Headers, footers, footnotes and comments are not read.
 */

#include "ooxml.h"

#include <stdlib.h>
#include <string.h>

/* Text of word/document.xml. */
static void scan_document(const char* x, int64_t len, sbuf_t* out) {
    const char* p = x;
    const char* e = x + len;
    int in_cell = 0;
    while (p < e) {
        const char* lt = memchr(p, '<', (size_t)(e - p));
        if (lt == NULL) break;
        const char* gt = memchr(lt, '>', (size_t)(e - lt));
        if (gt == NULL) break;
        int closing = lt + 1 < e && lt[1] == '/';
        const char* name = lt + (closing ? 2 : 1);
        size_t n = xml_tag_name(name, gt);
        int self_closing = gt > lt && gt[-1] == '/';

        if (!closing && xml_is(name, n, "w:t") && !self_closing) {
            const char* end = strstr(gt + 1, "</w:t>");
            if (end == NULL || end > e) break;
            sbuf_put_xml_text(out, gt + 1, end);
            p = end + 6;
            continue;
        }
        if (!closing && xml_is(name, n, "w:tab")) sbuf_put(out, "\t", 1);
        else if (!closing && (xml_is(name, n, "w:br") || xml_is(name, n, "w:cr"))) sbuf_put(out, "\n", 1);
        else if (closing && xml_is(name, n, "w:p")) sbuf_put(out, in_cell ? " " : "\n\n", in_cell ? 1 : 2);
        else if (!closing && !self_closing && xml_is(name, n, "w:tc")) in_cell++;
        else if (closing && xml_is(name, n, "w:tc")) {
            if (in_cell > 0) in_cell--;
            sbuf_trim_end(out);
            sbuf_put(out, "\t", 1);
        } else if (closing && xml_is(name, n, "w:tr")) {
            sbuf_trim_end(out);
            sbuf_put(out, "\n", 1);
        } else if (closing && xml_is(name, n, "w:tbl")) sbuf_put(out, "\n", 1);
        p = gt + 1;
    }
}

int doc_extract_docx(const char* path, doc_text_t* out) {
    mz_zip_archive z;
    int rc = ooxml_open(path, &z);
    if (rc != DOC_OK) return rc;
    int too_big = 0;
    int64_t xlen = 0;
    char* xml = ooxml_part(&z, "word/document.xml", &xlen, &too_big);
    char* title = xml ? ooxml_title(&z) : NULL;
    mz_zip_reader_end(&z);
    if (xml == NULL) return too_big ? DOC_ETOOBIG : DOC_EFORMAT;

    sbuf_t raw = { 0 };
    sbuf_put(&raw, "", 0);
    scan_document(xml, xlen, &raw);
    free(xml);
    rc = raw.oom ? DOC_ENOMEM : DOC_OK;
    char* text = NULL;
    int64_t len = 0;
    if (rc == DOC_OK) rc = doc_normalize(raw.data ? raw.data : "", raw.len, &text, &len);
    free(raw.data);
    if (rc != DOC_OK) {
        free(title);
        return rc;
    }
    out->text = text;
    out->len = len;
    out->n_pages = 0;
    out->page_offsets = NULL;
    out->title = title;
    return DOC_OK;
}
