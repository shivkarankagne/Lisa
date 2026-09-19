/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Documents: extractor registry, text normalisation, plain-text extractor.
 */

#include "documents.h"
#include "doc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utf8proc.h"
#include "../platform/platform.h"

/* ==== Registry ======================================================== */

static const char* const k_text_ext[] = { "txt", "text", NULL };
static const char* const k_md_ext[]   = { "md", "markdown", "mdown", "mkd", NULL };
#ifdef LISA_HAVE_PDFIUM
static const char* const k_pdf_ext[]  = { "pdf", NULL };
#endif

static const doc_extractor_t k_extractors[] = {
    { "text",     k_text_ext, doc_extract_text },
    { "markdown", k_md_ext,   doc_extract_markdown },
#ifdef LISA_HAVE_PDFIUM
    { "pdf",      k_pdf_ext,  doc_extract_pdf },
#endif
    { NULL, NULL, NULL },
};

const doc_extractor_t* doc_extractors(void) {
    return k_extractors;
}

static int ext_equal(const char* a, const char* lower) {
    for (; *a && *lower; a++, lower++) {
        char c = *a;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *lower) return 0;
    }
    return *a == '\0' && *lower == '\0';
}

const doc_extractor_t* doc_find_extractor(const char* path) {
    if (path == NULL) return NULL;
    const char* dot = strrchr(path, '.');
    const char* sep = strrchr(path, lisa_path_sep());
    if (dot == NULL || (sep && dot < sep) || dot[1] == '\0') return NULL;
    for (const doc_extractor_t* e = k_extractors; e->name; e++) {
        for (const char* const* x = e->extensions; *x; x++) {
            if (ext_equal(dot + 1, *x)) return e;
        }
    }
    return NULL;
}

int doc_extract(const char* path, doc_text_t* out) {
    if (path == NULL || out == NULL) return DOC_EINVAL;
    memset(out, 0, sizeof(*out));
    const doc_extractor_t* e = doc_find_extractor(path);
    if (e == NULL) return DOC_EUNSUPPORTED;
    int rc = e->extract(path, out);
    if (rc != DOC_OK) doc_text_free(out);
    return rc;
}

void doc_text_free(doc_text_t* t) {
    if (t == NULL) return;
    free(t->text);
    free(t->page_offsets);
    free(t->title);
    memset(t, 0, sizeof(*t));
}

int64_t doc_page_at(const doc_text_t* t, int64_t offset) {
    if (t == NULL || t->n_pages == 0) return 0;
    int64_t lo = 0, hi = t->n_pages - 1;
    while (lo < hi) {  /* last page whose start <= offset */
        int64_t mid = (lo + hi + 1) / 2;
        if (t->page_offsets[mid] <= offset) lo = mid;
        else hi = mid - 1;
    }
    return lo + 1;
}

/* ==== File reading ==================================================== */

int doc_read_file(const char* path, char** out, int64_t* out_len) {
    *out = NULL;
    *out_len = 0;
    int64_t size = lisa_file_size(path);
    if (size == LISA_PLAT_ENOENT) return DOC_ENOTFOUND;
    if (size < 0) return DOC_EIO;
    if (size > DOC_MAX_FILE_BYTES) return DOC_ETOOBIG;

    char* buf = (char*)malloc((size_t)size + 1);
    if (buf == NULL) return DOC_ENOMEM;
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        free(buf);
        return DOC_EIO;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    int err = ferror(f);
    fclose(f);
    if (err || (int64_t)got != size) {
        free(buf);
        return DOC_EIO;
    }
    buf[size] = '\0';
    *out = buf;
    *out_len = size;
    return DOC_OK;
}

/* ==== Normalisation =================================================== */

/* Windows-1252 bytes 0x80..0x9F (0 = undefined, dropped). */
static const int32_t k_cp1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

typedef struct {
    char*   data;
    int64_t len, cap;
    int     pending_cr;
} sink_t;

static int sink_bytes(sink_t* s, const char* b, int64_t n) {
    if (s->len + n + 1 > s->cap) {
        int64_t cap = s->cap ? s->cap * 2 : 4096;
        while (cap < s->len + n + 1) cap *= 2;
        char* d = (char*)realloc(s->data, (size_t)cap);
        if (d == NULL) return DOC_ENOMEM;
        s->data = d;
        s->cap = cap;
    }
    memcpy(s->data + s->len, b, (size_t)n);
    s->len += n;
    s->data[s->len] = '\0';
    return DOC_OK;
}

/* Append one code point, applying the control-character rules. */
static int sink_cp(sink_t* s, int32_t cp) {
    if (s->pending_cr) {
        s->pending_cr = 0;
        if (cp == '\n') return sink_bytes(s, "\n", 1);  /* CRLF -> LF */
        int rc = sink_bytes(s, "\n", 1);                /* lone CR -> LF */
        if (rc != DOC_OK) return rc;
    }
    if (cp == '\r') {
        s->pending_cr = 1;
        return DOC_OK;
    }
    if (cp == '\f') return sink_bytes(s, "\n\n", 2);    /* page break = paragraph break */
    if (cp == '\n' || cp == '\t') {
        char c = (char)cp;
        return sink_bytes(s, &c, 1);
    }
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || cp == 0xFEFF) return DOC_OK;
    if (!utf8proc_codepoint_valid(cp)) cp = 0xFFFD;
    utf8proc_uint8_t buf[4];
    utf8proc_ssize_t n = utf8proc_encode_char(cp, buf);
    return sink_bytes(s, (const char*)buf, (int64_t)n);
}

static int sink_finish(sink_t* s) {
    if (s->pending_cr) {
        s->pending_cr = 0;
        return sink_bytes(s, "\n", 1);
    }
    return s->data ? DOC_OK : sink_bytes(s, "", 0);
}

static int is_valid_utf8(const unsigned char* p, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t k = utf8proc_iterate(p + i, n - i, &cp);
        if (k <= 0) return 0;
        i += k;
    }
    return 1;
}

int doc_normalize(const char* raw, int64_t raw_len, char** out, int64_t* out_len) {
    if (raw == NULL || raw_len < 0 || out == NULL || out_len == NULL) return DOC_EINVAL;
    *out = NULL;
    *out_len = 0;
    const unsigned char* p = (const unsigned char*)raw;
    sink_t s = { NULL, 0, 0, 0 };
    int rc = DOC_OK;

    if (raw_len >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) {
        int le = p[0] == 0xFF;
        for (int64_t i = 2; rc == DOC_OK && i + 1 < raw_len; i += 2) {
            int32_t u = le ? (p[i] | (p[i + 1] << 8)) : ((p[i] << 8) | p[i + 1]);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < raw_len) {
                int32_t lo = le ? (p[i + 2] | (p[i + 3] << 8)) : ((p[i + 2] << 8) | p[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                } else {
                    u = 0xFFFD;
                }
            } else if (u >= 0xD800 && u <= 0xDFFF) {
                u = 0xFFFD;
            }
            rc = sink_cp(&s, u);
        }
    } else {
        int64_t start = (raw_len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) ? 3 : 0;
        if (is_valid_utf8(p + start, raw_len - start)) {
            int64_t i = start;
            while (rc == DOC_OK && i < raw_len) {
                utf8proc_int32_t cp;
                utf8proc_ssize_t k = utf8proc_iterate(p + i, raw_len - i, &cp);
                rc = sink_cp(&s, cp);
                i += k;
            }
        } else {
            for (int64_t i = start; rc == DOC_OK && i < raw_len; i++) {
                int32_t cp = p[i];
                if (cp >= 0x80 && cp <= 0x9F) cp = k_cp1252[cp - 0x80];
                if (cp != 0) rc = sink_cp(&s, cp);
            }
        }
    }
    if (rc == DOC_OK) rc = sink_finish(&s);
    if (rc != DOC_OK) {
        free(s.data);
        return rc;
    }

    /* Canonical composition, so equal text always has equal bytes. */
    utf8proc_uint8_t* nfc = utf8proc_NFC((const utf8proc_uint8_t*)s.data);
    free(s.data);
    if (nfc == NULL) return DOC_ENOMEM;
    *out = (char*)nfc;
    *out_len = (int64_t)strlen((char*)nfc);
    return DOC_OK;
}

/* ==== Plain text ====================================================== */

int doc_extract_text(const char* path, doc_text_t* out) {
    char* raw = NULL;
    int64_t n = 0;
    int rc = doc_read_file(path, &raw, &n);
    if (rc != DOC_OK) return rc;
    rc = doc_normalize(raw, n, &out->text, &out->len);
    free(raw);
    return rc;
}
