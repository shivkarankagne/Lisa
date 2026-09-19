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

#include "doc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "../platform/platform.h"

#define MAX_PART_BYTES (256LL * 1024 * 1024)

typedef struct {
    char*   data;
    int64_t len, cap;
    int     oom;
} sbuf_t;

static void put(sbuf_t* b, const char* s, int64_t n) {
    if (b->oom || n <= 0) return;
    if (b->len + n + 1 > b->cap) {
        int64_t cap = b->cap ? b->cap : 4096;
        while (b->len + n + 1 > cap) cap *= 2;
        char* d = (char*)realloc(b->data, (size_t)cap);
        if (d == NULL) {
            b->oom = 1;
            return;
        }
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, (size_t)n);
    b->len += n;
    b->data[b->len] = '\0';
}

/* Append the UTF-8 encoding of code point cp (invalid ones are dropped). */
static void put_cp(sbuf_t* b, unsigned long cp) {
    char u[4];
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return;
    if (cp < 0x80) {
        u[0] = (char)cp;
        put(b, u, 1);
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        put(b, u, 2);
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        put(b, u, 3);
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        put(b, u, 4);
    }
}

/* Append XML character data [s, e) with entities decoded. */
static void put_xml_text(sbuf_t* b, const char* s, const char* e) {
    while (s < e) {
        const char* amp = memchr(s, '&', (size_t)(e - s));
        if (amp == NULL) {
            put(b, s, e - s);
            return;
        }
        put(b, s, amp - s);
        const char* semi = memchr(amp, ';', (size_t)(e - amp));
        if (semi == NULL || semi - amp > 12) {   /* not an entity: keep the '&' */
            put(b, "&", 1);
            s = amp + 1;
            continue;
        }
        size_t n = (size_t)(semi - amp - 1);
        const char* name = amp + 1;
        if (n == 3 && memcmp(name, "amp", 3) == 0) put(b, "&", 1);
        else if (n == 2 && memcmp(name, "lt", 2) == 0) put(b, "<", 1);
        else if (n == 2 && memcmp(name, "gt", 2) == 0) put(b, ">", 1);
        else if (n == 4 && memcmp(name, "quot", 4) == 0) put(b, "\"", 1);
        else if (n == 4 && memcmp(name, "apos", 4) == 0) put(b, "'", 1);
        else if (n >= 2 && name[0] == '#') {
            char tmp[16];
            memcpy(tmp, name + 1, n - 1);
            tmp[n - 1] = '\0';
            char* end = NULL;
            unsigned long cp = (tmp[0] == 'x' || tmp[0] == 'X') ? strtoul(tmp + 1, &end, 16)
                                                                : strtoul(tmp, &end, 10);
            if (end && *end == '\0') put_cp(b, cp);
        }
        s = semi + 1;
    }
}

/* Tag name at p (just after '<' or "</"), e.g. "w:t"; returns its length. */
static size_t tag_name(const char* p, const char* e) {
    size_t n = 0;
    while (p + n < e && p[n] != '>' && p[n] != '/' && p[n] != ' ' && p[n] != '\t' &&
           p[n] != '\n' && p[n] != '\r')
        n++;
    return n;
}

static int is(const char* p, size_t n, const char* name) {
    return strlen(name) == n && memcmp(p, name, n) == 0;
}

/* Text of word/document.xml. */
/* Drop trailing spaces and tabs (the end of a cell or row). */
static void trim_end(sbuf_t* b) {
    while (b->len > 0 && (b->data[b->len - 1] == ' ' || b->data[b->len - 1] == '\t')) b->data[--b->len] = '\0';
}

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
        size_t n = tag_name(name, gt);
        int self_closing = gt > lt && gt[-1] == '/';

        if (!closing && is(name, n, "w:t") && !self_closing) {
            const char* end = strstr(gt + 1, "</w:t>");
            if (end == NULL || end > e) break;
            put_xml_text(out, gt + 1, end);
            p = end + 6;
            continue;
        }
        if (!closing && is(name, n, "w:tab")) put(out, "\t", 1);
        else if (!closing && (is(name, n, "w:br") || is(name, n, "w:cr"))) put(out, "\n", 1);
        else if (closing && is(name, n, "w:p")) put(out, in_cell ? " " : "\n\n", in_cell ? 1 : 2);
        else if (!closing && !self_closing && is(name, n, "w:tc")) in_cell++;
        else if (closing && is(name, n, "w:tc")) {
            if (in_cell > 0) in_cell--;
            trim_end(out);
            put(out, "\t", 1);
        } else if (closing && is(name, n, "w:tr")) {
            trim_end(out);
            put(out, "\n", 1);
        } else if (closing && is(name, n, "w:tbl")) put(out, "\n", 1);
        p = gt + 1;
    }
}

/* <dc:title> from docProps/core.xml, or NULL. */
static char* scan_title(const char* x) {
    const char* s = strstr(x, "<dc:title>");
    if (s == NULL) return NULL;
    s += 10;
    const char* e = strstr(s, "</dc:title>");
    if (e == NULL) return NULL;
    sbuf_t b = { 0 };
    put_xml_text(&b, s, e);
    if (b.oom || b.len == 0) {
        free(b.data);
        return NULL;
    }
    return b.data;
}

/* Extract one part of the archive, NUL-terminated; NULL if missing. */
static char* read_part(mz_zip_archive* z, const char* name, int64_t* out_len, int* too_big) {
    *too_big = 0;
    int idx = mz_zip_reader_locate_file(z, name, NULL, 0);
    if (idx < 0) return NULL;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(z, (mz_uint)idx, &st)) return NULL;
    if ((int64_t)st.m_uncomp_size > MAX_PART_BYTES) {   /* also guards against zip bombs */
        *too_big = 1;
        return NULL;
    }
    size_t n = 0;
    char* raw = (char*)mz_zip_reader_extract_to_heap(z, (mz_uint)idx, &n, 0);
    if (raw == NULL) return NULL;
    char* s = (char*)malloc(n + 1);
    if (s) {
        memcpy(s, raw, n);
        s[n] = '\0';
    }
    mz_free(raw);
    if (out_len) *out_len = (int64_t)n;
    return s;
}

int doc_extract_docx(const char* path, doc_text_t* out) {
    int64_t size = lisa_file_size(path);
    if (size == LISA_PLAT_ENOENT) return DOC_ENOTFOUND;
    if (size < 0) return DOC_EIO;
    if (size > DOC_MAX_FILE_BYTES) return DOC_ETOOBIG;

    mz_zip_archive z;
    memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_file(&z, path, 0)) {
        if (mz_zip_get_last_error(&z) == MZ_ZIP_FILE_OPEN_FAILED) return DOC_EIO;
        /* A password-protected .docx is not a zip but an OLE compound file. */
        static const unsigned char ole[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
        unsigned char head[8] = { 0 };
        FILE* f = fopen(path, "rb");
        size_t got = f ? fread(head, 1, sizeof(head), f) : 0;
        if (f) fclose(f);
        return got == sizeof(head) && memcmp(head, ole, sizeof(ole)) == 0 ? DOC_EENCRYPTED : DOC_EFORMAT;
    }
    int too_big = 0;
    int64_t xlen = 0;
    char* xml = read_part(&z, "word/document.xml", &xlen, &too_big);
    int64_t clen = 0;
    int dummy = 0;
    char* core = xml ? read_part(&z, "docProps/core.xml", &clen, &dummy) : NULL;
    mz_zip_reader_end(&z);
    if (xml == NULL) {
        free(core);
        return too_big ? DOC_ETOOBIG : DOC_EFORMAT;
    }

    sbuf_t raw = { 0 };
    put(&raw, "", 0);
    scan_document(xml, xlen, &raw);
    free(xml);
    int rc = raw.oom ? DOC_ENOMEM : DOC_OK;
    char* text = NULL;
    int64_t len = 0;
    if (rc == DOC_OK) rc = doc_normalize(raw.data ? raw.data : "", raw.len, &text, &len);
    free(raw.data);
    if (rc != DOC_OK) {
        free(core);
        return rc;
    }
    out->text = text;
    out->len = len;
    out->n_pages = 0;
    out->page_offsets = NULL;
    out->title = core ? scan_title(core) : NULL;
    free(core);
    return DOC_OK;
}
