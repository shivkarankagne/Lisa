/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Chunker.
 *
 * 1. The text is cut into units: paragraphs (runs of non-blank lines).
 *    A paragraph longer than max_chars is cut into sentences; a sentence
 *    longer than max_chars is cut at the last whitespace before the limit,
 *    or at the limit itself if there is none (e.g. very long tokens).
 * 2. Units are packed in order into chunks of about target_chars. Each
 *    new chunk starts with up to overlap_chars of the previous chunk's
 *    end, beginning at a word boundary, unless that would exceed
 *    max_chars.
 *
 * Sizes are in Unicode code points. Sentence ends: . ! ? and the
 * Devanagari danda (U+0964, U+0965) and ideographic full stop (U+3002)
 * when followed by whitespace, and line breaks inside a paragraph.
 */

#include "documents.h"

#include <stdlib.h>
#include <string.h>

#include "utf8proc.h"

typedef struct {
    int64_t s, e;   /* byte range, trimmed */
} unit_t;

typedef struct {
    unit_t* u;
    int64_t n, cap;
} units_t;

/* ---- UTF-8 helpers ---------------------------------------------------- */

static int64_t cp_len(const char* t, int64_t len, int64_t i) {
    unsigned char c = (unsigned char)t[i];
    int64_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    return i + n <= len ? n : len - i;
}

static int32_t cp_at(const char* t, int64_t len, int64_t i) {
    utf8proc_int32_t cp = 0xFFFD;
    utf8proc_iterate((const utf8proc_uint8_t*)t + i, len - i, &cp);
    return cp;
}

static int is_space_cp(int32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n') return 1;
    utf8proc_category_t c = utf8proc_category(cp);
    return c == UTF8PROC_CATEGORY_ZS || c == UTF8PROC_CATEGORY_ZL || c == UTF8PROC_CATEGORY_ZP;
}

static int is_sentence_end(int32_t cp) {
    return cp == '.' || cp == '!' || cp == '?' || cp == 0x0964 || cp == 0x0965 || cp == 0x3002;
}

static int64_t count_chars(const char* t, int64_t s, int64_t e) {
    int64_t n = 0;
    for (int64_t i = s; i < e; i++) n += ((unsigned char)t[i] & 0xC0) != 0x80;
    return n;
}

/* Byte offset reached by advancing k code points from s (not past e). */
static int64_t advance_chars(const char* t, int64_t len, int64_t s, int64_t e, int64_t k) {
    int64_t i = s;
    while (i < e && k-- > 0) i += cp_len(t, len, i);
    return i;
}

/* Trim whitespace from both ends of [*s, *e). */
static void trim(const char* t, int64_t len, int64_t* s, int64_t* e) {
    while (*s < *e && is_space_cp(cp_at(t, len, *s))) *s += cp_len(t, len, *s);
    while (*e > *s) {
        int64_t p = *e - 1;
        while (p > *s && ((unsigned char)t[p] & 0xC0) == 0x80) p--;
        if (!is_space_cp(cp_at(t, len, p))) break;
        *e = p;
    }
}

static int push(units_t* u, int64_t s, int64_t e) {
    if (s >= e) return DOC_OK;
    if (u->n == u->cap) {
        int64_t cap = u->cap ? u->cap * 2 : 256;
        unit_t* g = (unit_t*)realloc(u->u, (size_t)cap * sizeof(unit_t));
        if (g == NULL) return DOC_ENOMEM;
        u->u = g;
        u->cap = cap;
    }
    u->u[u->n].s = s;
    u->u[u->n].e = e;
    u->n++;
    return DOC_OK;
}

/* ---- units ------------------------------------------------------------ */

/* A range longer than max: cut at whitespace before the limit, or at it. */
static int split_words(const char* t, int64_t len, int64_t s, int64_t e, int64_t max, units_t* u) {
    while (s < e) {
        trim(t, len, &s, &e);
        if (s >= e) break;
        if (count_chars(t, s, e) <= max) return push(u, s, e);
        int64_t limit = advance_chars(t, len, s, e, max);
        int64_t cut = -1;
        for (int64_t i = s; i < limit; i += cp_len(t, len, i)) {
            if (is_space_cp(cp_at(t, len, i)) && i > s) cut = i;
        }
        if (cut < 0) cut = limit;
        int64_t ps = s, pe = cut;
        trim(t, len, &ps, &pe);
        int rc = push(u, ps, pe);
        if (rc != DOC_OK) return rc;
        s = cut;
    }
    return DOC_OK;
}

static int split_sentences(const char* t, int64_t len, int64_t s, int64_t e, int64_t max, units_t* u) {
    int64_t start = s;
    for (int64_t i = s; i < e; i += cp_len(t, len, i)) {
        int32_t cp = cp_at(t, len, i);
        int64_t next = i + cp_len(t, len, i);
        int boundary = cp == '\n' ||
                       (is_sentence_end(cp) && (next >= e || is_space_cp(cp_at(t, len, next))));
        if (!boundary) continue;
        int64_t ss = start, se = next;
        trim(t, len, &ss, &se);
        if (ss < se) {
            int rc = count_chars(t, ss, se) <= max ? push(u, ss, se)
                                                   : split_words(t, len, ss, se, max, u);
            if (rc != DOC_OK) return rc;
        }
        start = next;
    }
    int64_t ss = start, se = e;
    trim(t, len, &ss, &se);
    if (ss >= se) return DOC_OK;
    return count_chars(t, ss, se) <= max ? push(u, ss, se) : split_words(t, len, ss, se, max, u);
}

/* Paragraphs: maximal runs of lines that contain non-whitespace. */
static int make_units(const char* t, int64_t len, int64_t max, units_t* u) {
    int64_t para = -1;  /* start of the current paragraph, or -1 */
    int64_t i = 0;
    while (i <= len) {
        int64_t eol = i;
        while (eol < len && t[eol] != '\n') eol++;
        int64_t ls = i, le = eol;
        trim(t, len, &ls, &le);
        int blank = ls >= le;
        if (!blank && para < 0) para = i;
        if ((blank || eol >= len) && para >= 0) {
            int64_t ps = para, pe = blank ? i : eol;
            trim(t, len, &ps, &pe);
            int rc = count_chars(t, ps, pe) <= max ? push(u, ps, pe)
                                                   : split_sentences(t, len, ps, pe, max, u);
            if (rc != DOC_OK) return rc;
            para = -1;
        }
        if (eol >= len) break;
        i = eol + 1;
    }
    return DOC_OK;
}

/* ---- packing ---------------------------------------------------------- */

/*
 * Start of the overlap: up to k chars before e, at a word start, strictly
 * after s (so a chunk never repeats the whole previous chunk). Returns e
 * when there is no such word start.
 */
static int64_t overlap_start(const char* t, int64_t len, int64_t s, int64_t e, int64_t k) {
    if (k <= 0) return e;
    int64_t chars = count_chars(t, s, e);
    int64_t b = chars <= k ? s : advance_chars(t, len, s, e, chars - k);
    /* Move forward to the first non-space character after a space. */
    while (b < e && !is_space_cp(cp_at(t, len, b))) b += cp_len(t, len, b);
    while (b < e && is_space_cp(cp_at(t, len, b))) b += cp_len(t, len, b);
    return b;
}

int doc_chunk(const doc_text_t* dt, const doc_chunk_params_t* p,
              doc_chunk_t** out, int64_t* n_out) {
    if (dt == NULL || p == NULL || out == NULL || n_out == NULL) return DOC_EINVAL;
    *out = NULL;
    *n_out = 0;
    if (p->target_chars < 1 || p->max_chars < p->target_chars || p->overlap_chars < 0 ||
        p->overlap_chars >= p->target_chars)
        return DOC_EINVAL;
    if (dt->text == NULL || dt->len == 0) return DOC_OK;

    const char* t = dt->text;
    int64_t len = dt->len;
    units_t u = { NULL, 0, 0 };
    int rc = make_units(t, len, p->max_chars, &u);

    doc_chunk_t* c = NULL;
    int64_t n = 0, cap = 0;
    int64_t cs = -1, ce = -1;           /* current chunk: byte range */
    int64_t prev_s = -1, prev_e = -1;   /* previous chunk, for overlap */

    for (int64_t i = 0; rc == DOC_OK && i <= u.n; i++) {
        int flush = i == u.n;
        /* Measure the real span (separators included) the chunk would have. */
        if (!flush && cs >= 0 && count_chars(t, cs, u.u[i].e) > p->target_chars) flush = 1;
        if (flush && cs >= 0) {
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                doc_chunk_t* g = (doc_chunk_t*)realloc(c, (size_t)cap * sizeof(*c));
                if (g == NULL) {
                    rc = DOC_ENOMEM;
                    break;
                }
                c = g;
            }
            c[n].offset = cs;
            c[n].length = ce - cs;
            c[n].page = doc_page_at(dt, cs);
            n++;
            prev_s = cs;
            prev_e = ce;
            cs = -1;
        }
        if (i == u.n) break;
        if (cs < 0) {
            cs = u.u[i].s;
            if (prev_e >= 0) {
                int64_t b = overlap_start(t, len, prev_s, prev_e, p->overlap_chars);
                if (b < prev_e && count_chars(t, b, u.u[i].e) <= p->max_chars) cs = b;
            }
        }
        ce = u.u[i].e;
    }
    free(u.u);
    if (rc != DOC_OK) {
        free(c);
        return rc;
    }
    *out = c;
    *n_out = n;
    return DOC_OK;
}
